#include "discovery/mdns.h"

#include "common/log.h"
#include "common/util.h"
#include "discovery/mdns_names.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wchanges-meaning"
#endif

#include "mDNSEmbeddedAPI.h"
#include "mDNSPosix.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <fcntl.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace squeeze2raop2 {

// DNSQuestion value-init replaces memset-0 below; that swap is only sound
// for a plain C aggregate.
static_assert(std::is_trivially_copyable_v<DNSQuestion>,
              "DNSQuestion value-init must remain equivalent to memset-0");

namespace {

constexpr mDNSu32 kRrCacheSize = 900;
CacheEntity gRRCache[kRrCacheSize];

extern "C" {
mDNS mDNSStorage;  // the vendored code references this client-owned global
extern const char ProgramName[] = "squeeze2raop2";
}  // extern "C"

mDNS& gMdns = mDNSStorage;  // keep C++-side name
mDNS_PlatformSupport gMdnsPlatformSupport;

// Only this guard and the C-mandated storage above are file-scope state; every
// other piece of browser state lives in MdnsBrowser::Impl.
std::atomic<MdnsBrowser*> g_activeBrowser{nullptr};

constexpr std::array<std::string_view, 2> kServiceTypes{"_raop._tcp", "_airplay._tcp"};

std::string domainToString(const domainname* name) {
    char buffer[MAX_ESCAPED_DOMAIN_NAME];
    ConvertDomainNameToCString(name, buffer);
    return std::string(buffer);
}

uint16_t ipPortHostOrder(const mDNSIPPort& port) {
    return static_cast<uint16_t>((port.b[0] << 8) | port.b[1]);
}

// The rdata union provides typed members; use them instead of reinterpreting
// u.data (mirrors how mDNSResponder's core reads SRV/A answers).
std::string addressFromRData4(const ResourceRecord* rr) {
    const mDNSu8(&octets)[4] = rr->rdata->u.ipv4.b;
    return std::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
}

// Runs a cleanup action on scope exit unless released. start()'s failure paths
// converge on one teardown through this instead of three hand-copied blocks.
class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
        if (fn_) fn_();
    }
    void release() { fn_ = nullptr; }

private:
    std::function<void()> fn_;
};

// Owns the wake pipe fd; no manual close or ordering comment needed.
struct UniqueFd {
    int fd = -1;

    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
    ~UniqueFd() {
        if (fd >= 0) ::close(fd);
    }
};

}  // namespace

// All mDNS core access happens on the loop thread: mDNS is single-threaded
// and its core state has no locks. Loop-thread code (callbacks) calls the
// API directly; other threads enqueue commands through Impl::post().
struct MdnsBrowser::Impl {
    // One resolve tracker per service instance. QuestionContext points here,
    // and owner points back so callbacks can reach Impl without globals.
    struct ResolveTracker {
        DNSQuestion srvQ;
        DNSQuestion txtQ;
        DNSQuestion addrQ;
        bool srvActive = false;
        bool txtActive = false;
        bool addrActive = false;

        uint16_t port = 0;
        std::string targetHost;
        std::string address;
        std::map<std::string, std::string> txt;

        std::string instance;
        std::string serviceType;
        Impl* owner = nullptr;
    };

    RecordCallback cb;
    std::jthread loopThread;
    mDNSInterfaceID iface = mDNSInterface_Any;
    DNSQuestion browseQ[kServiceTypes.size()] = {};
    bool browseActive[kServiceTypes.size()] = {};
    std::map<std::string, ResolveTracker> resolvers;

    // mDNS debug flags are process-global; remember the prior values so stop()
    // does not leak debug mode into a later start() or another library user.
    int savedLogging = 0;
    int savedDebugMode = 0;

    // command queue + wake pipe so start/stop/teardown requests run on the
    // loop thread instead of racing mDNSPosixProcessFDSet
    std::mutex cmdMutex;
    std::deque<std::function<void()>> cmdQueue;
    UniqueFd wakeRd;
    UniqueFd wakeWr;

    ~Impl() {
        // Join before member destruction: the loop thread must be gone before
        // the wake fds (and the rest of this state) are destroyed.
        if (loopThread.joinable()) {
            loopThread.request_stop();
            loopThread.join();
        }
    }

    void post(std::function<void()> cmd) {
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            cmdQueue.push_back(std::move(cmd));
        }
        wake();
    }

    void wake() {
        if (wakeWr.fd >= 0) {
            const char token = 1;
            (void)::write(wakeWr.fd, &token, 1);
        }
    }

    void drainWake() {
        char buf[64];
        while (::read(wakeRd.fd, buf, sizeof(buf)) > 0) {
        }
    }

    void runCommands() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            batch.swap(cmdQueue);
        }
        for (auto& cmd : batch) cmd();
    }

    void runLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            runCommands();
            if (stopToken.stop_requested()) break;
            fd_set readfds;
            fd_set writefds;
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            struct timeval timeout{};
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            int nfds = 0;
            mDNSPosixGetFDSet(&gMdns, &nfds, &readfds, &writefds, &timeout);
            if (wakeRd.fd >= 0) {
                FD_SET(wakeRd.fd, &readfds);
                if (wakeRd.fd + 1 > nfds) nfds = wakeRd.fd + 1;
            }
            int rc = ::select(nfds, &readfds, &writefds, nullptr, &timeout);
            if (rc > 0) {
                if (wakeRd.fd >= 0 && FD_ISSET(wakeRd.fd, &readfds)) drainWake();
                mDNSPosixProcessFDSet(&gMdns, &readfds, &writefds);
                runCommands();
            }
        }
    }

    void stopQuestion(DNSQuestion* q, bool& active) {
        if (!active) return;
        active = false;
        mDNS_StopQuery(&gMdns, q);
    }

    // Fills `q` from scratch; sole place that knows how a resolve question is
    // shaped. Returns false if the name is malformed.
    bool initQuestion(DNSQuestion& q, const std::string& fqdn, mDNSu16 qtype,
                      ResolveTracker* owner) {
        q = DNSQuestion{};  // value-init instead of memset
        q.QuestionContext = owner;
        if (MakeDomainNameFromDNSNameString(&q.qname, fqdn.c_str()) == nullptr) return false;
        q.InterfaceID = iface;
        q.qtype = qtype;
        q.qclass = kDNSClass_IN;
        q.QuestionCallback = resolveQuestionCb;
        q.ForceMCast = mDNStrue;
        return true;
    }

    void stopTracker(ResolveTracker& t) {
        stopQuestion(&t.srvQ, t.srvActive);
        stopQuestion(&t.txtQ, t.txtActive);
        stopQuestion(&t.addrQ, t.addrActive);
    }

    static ResolveTracker* trackerForQuestion(DNSQuestion* q) {
        return static_cast<ResolveTracker*>(q->QuestionContext);
    }

    void publishResolved(ResolveTracker& t) {
        if (t.port == 0 || t.address.empty()) {
            log::debug("mdns: resolve incomplete for {}", t.instance);
            return;
        }
        if (!cb) return;
        MdnsRecord record;
        record.type = t.serviceType;
        record.instance = t.instance;
        record.host = t.address;
        record.port = t.port;
        record.txt = t.txt;
        log::info("mdns: {} resolved {}:{} ({} txt keys)", t.instance, record.host, record.port,
                  record.txt.size());
        cb(record, MdnsBrowser::RecordEvent::Added);
    }

    static void resolveQuestionCb(mDNS* m, DNSQuestion* q, const ResourceRecord* rr,
                                  QC_result add) {
        (void)m;
        // Any non-QC_rmv value is an add (mDNSEmbeddedAPI.h QC_result contract).
        // Accepting QC_addnocache matters: mDNS delivers it when the RR cache is
        // full, and dropping it would silently lose SRV/TXT/A answers.
        if (add == QC_rmv) return;
        log::debug("mdns: resolve cb add={} rrtype={}", static_cast<int>(add), rr->rrtype);

        ResolveTracker* owner = trackerForQuestion(q);
        if (!owner || !owner->owner) return;
        Impl& impl = *owner->owner;
        ResolveTracker& t = *owner;

        if (rr->rrtype == kDNSType_A) {
            if (rr->rdlength >= 4) t.address = addressFromRData4(rr);
            impl.stopQuestion(q, t.addrActive);
        } else if (rr->rrtype == kDNSType_SRV) {
            // typed union overlay, exactly how mDNSResponder's core reads these
            t.port = ipPortHostOrder(rr->rdata->u.srv.port);
            t.targetHost = domainToString(&rr->rdata->u.srv.target);
            impl.stopQuestion(q, t.srvActive);
        } else if (rr->rrtype == kDNSType_TXT) {
            t.txt = parseTxtKeyValues(
                std::string_view(reinterpret_cast<const char*>(rr->rdata->u.data), rr->rdlength));
            impl.stopQuestion(q, t.txtActive);
        } else {
            return;
        }

        if (!t.srvActive && !t.txtActive && !t.addrActive) {
            if (t.port && !t.targetHost.empty() && t.address.empty()) {
                if (impl.initQuestion(t.addrQ, t.targetHost, kDNSType_A, &t) &&
                    mDNS_StartQuery(&gMdns, &t.addrQ) == mStatus_NoError) {
                    t.addrActive = true;
                    return;
                }
            }
            impl.publishResolved(t);
        }
    }

    void resetTracker(ResolveTracker& t) {
        t.srvQ = DNSQuestion{};  // value-init instead of memset
        t.txtQ = DNSQuestion{};
        t.addrQ = DNSQuestion{};
        t.srvActive = t.txtActive = t.addrActive = false;
        t.port = 0;
        t.targetHost.clear();
        t.address.clear();
        t.txt.clear();
    }

    bool startTracker(ResolveTracker& t, const std::string& instance,
                      const std::string& serviceType) {
        resetTracker(t);
        t.instance = instance;
        t.serviceType = serviceType;
        t.owner = this;

        std::string fqdn = instance + "." + serviceType + ".local";
        if (!initQuestion(t.srvQ, fqdn, kDNSType_SRV, &t)) {
            log::warn("mdns: cannot form SRV name {}", fqdn);
            return false;
        }
        if (mDNS_StartQuery(&gMdns, &t.srvQ) != mStatus_NoError) {
            log::warn("mdns: SRV query failed for {}", fqdn);
            return false;
        }
        t.srvActive = true;

        if (initQuestion(t.txtQ, fqdn, kDNSType_TXT, &t)) {
            if (mDNS_StartQuery(&gMdns, &t.txtQ) == mStatus_NoError) {
                t.txtActive = true;
            }
        } else {
            log::debug("mdns: cannot form TXT name {}", fqdn);
        }
        log::debug("mdns: resolver started for {}, srv={} txt={}", instance,
                   static_cast<int>(t.srvActive), static_cast<int>(t.txtActive));
        return true;
    }

    void startResolveForInstance(const std::string& instance, const std::string& serviceType) {
        std::string fqdn = instance + "." + serviceType + ".local";
        auto it = resolvers.find(fqdn);
        if (it != resolvers.end()) return;
        ResolveTracker& t = resolvers[fqdn];
        if (!startTracker(t, instance, serviceType)) {
            resolvers.erase(fqdn);
        }
    }

    void stopResolveForInstance(const std::string& instance, const std::string& serviceType) {
        std::string fqdn = instance + "." + serviceType + ".local";
        auto it = resolvers.find(fqdn);
        if (it == resolvers.end()) return;
        stopTracker(it->second);
        resolvers.erase(it);
    }

    static void browseQuestionCb(mDNS* m, DNSQuestion* q, const ResourceRecord* rr, QC_result add) {
        (void)m;
        if (rr->rrtype != kDNSType_PTR || rr->rdlength == 0) return;
        auto* impl = static_cast<Impl*>(q->QuestionContext);
        if (!impl) return;

        std::string ptrName = domainToString(&rr->rdata->u.name);
        log::debug("mdns: browse ptr '{}' type={} add={}", ptrName, rr->rrtype,
                   static_cast<int>(add));
        std::string recordName = domainToString(rr->name);

        for (std::string_view serviceType : kServiceTypes) {
            if (recordName.find(serviceType) == std::string::npos) continue;
            std::string_view instance = stripServiceSuffix(ptrName, serviceType);
            if (instance.empty()) continue;
            if (add == QC_add || add == QC_addnocache) {
                impl->startResolveForInstance(std::string(instance), std::string(serviceType));
            } else {
                impl->stopResolveForInstance(std::string(instance), std::string(serviceType));
                if (impl->cb) {
                    MdnsRecord record;
                    record.type = serviceType;
                    record.instance = instance;
                    impl->cb(record, MdnsBrowser::RecordEvent::Removed);
                }
            }
            return;
        }
    }

    void teardown() {
        for (size_t i = 0; i < kServiceTypes.size(); ++i) {
            if (browseActive[i]) {
                browseActive[i] = false;
                mDNS_StopQuery(&gMdns, &browseQ[i]);
            }
        }
        for (auto& kv : resolvers) stopTracker(kv.second);
        resolvers.clear();
        cb = nullptr;
        mDNS_Close(&gMdns);
        mDNS_LoggingEnabled = savedLogging;
        mDNS_DebugMode = savedDebugMode;
        loopThread.request_stop();
    }
};

MdnsBrowser::MdnsBrowser() = default;

MdnsBrowser::~MdnsBrowser() { stop(); }

bool MdnsBrowser::start(const std::string& ifaceName, RecordCallback cb, std::string& errorOut) {
    if (impl_ || g_activeBrowser.load()) {
        errorOut = "mDNS browser already running (single instance per process)";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->cb = std::move(cb);

    unsigned index = 0;
    if (!ifaceName.empty()) {
        index = if_nametoindex(ifaceName.c_str());
        if (index == 0) {
            errorOut = std::string("cannot find interface ") + ifaceName;
            return false;
        }
    }

    // Everything below runs on this thread while no loop thread exists yet:
    // mDNS_Init and the browse questions are therefore race-free here.
    impl->savedLogging = mDNS_LoggingEnabled;
    impl->savedDebugMode = mDNS_DebugMode;
    if (log::level() >= log::Level::Debug) {
        mDNS_LoggingEnabled = mDNStrue;
        mDNS_DebugMode = mDNStrue;
    }

    mStatus status = mDNS_Init(&gMdns, &gMdnsPlatformSupport, gRRCache, kRrCacheSize,
                               mDNS_Init_DontAdvertiseLocalAddresses, mDNS_Init_NoInitCallback,
                               mDNS_Init_NoInitCallbackContext);
    if (status != mStatus_NoError) {
        mDNS_LoggingEnabled = impl->savedLogging;
        mDNS_DebugMode = impl->savedDebugMode;
        errorOut = std::string("mDNS_Init failed: ") + std::to_string(static_cast<int>(status));
        return false;
    }

    ScopeExit rollback([&] {
        for (size_t i = 0; i < kServiceTypes.size(); ++i) {
            if (impl->browseActive[i]) {
                impl->browseActive[i] = false;
                mDNS_StopQuery(&gMdns, &impl->browseQ[i]);
            }
        }
        mDNS_Close(&gMdns);
        mDNS_LoggingEnabled = impl->savedLogging;
        mDNS_DebugMode = impl->savedDebugMode;
    });

    if (index != 0) {
        // Map the kernel ifindex to the core's interface ID. NULL means the
        // interface was not registered during mDNS_Init; fall back to all.
        mDNSInterfaceID id = mDNSPlatformInterfaceIDfromInterfaceIndex(&gMdns, index);
        if (id) {
            impl->iface = id;
        } else {
            log::warn("mdns: interface {} not registered; browsing on all interfaces", ifaceName);
        }
    }

    for (size_t i = 0; i < kServiceTypes.size(); ++i) {
        const std::string serviceType(kServiceTypes[i]);
        domainname srv{};
        domainname dom{};
        MakeDomainNameFromDNSNameString(&srv, serviceType.c_str());
        MakeDomainNameFromDNSNameString(&dom, "local");
        impl->browseQ[i] = DNSQuestion{};
        mStatus st = mDNS_StartBrowse(&gMdns, &impl->browseQ[i], &srv, &dom, impl->iface, 0,
                                      mDNSfalse, mDNSfalse, Impl::browseQuestionCb, impl.get());
        if (st != mStatus_NoError) {
            errorOut = std::string("mDNS_StartBrowse(") + serviceType +
                       ") failed: " + std::to_string(static_cast<int>(st));
            return false;
        }
        impl->browseActive[i] = true;
        log::info("mdns: browsing {}", kServiceTypes[i]);
    }

    // Wake pipe so pending commands interrupt the select() wait immediately.
    // pipe2 is atomic CLOEXEC|NONBLOCK: no fcntl dance, no fd leak across exec.
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        errorOut = "cannot create mDNS wake pipe";
        return false;
    }
    impl->wakeRd = UniqueFd(fds[0]);
    impl->wakeWr = UniqueFd(fds[1]);

    Impl* raw = impl.get();
    raw->loopThread = std::jthread([raw](std::stop_token st) { raw->runLoop(st); });

    rollback.release();
    impl_ = std::move(impl);
    g_activeBrowser.store(this);
    return true;
}

void MdnsBrowser::stop() {
    Impl* raw = impl_.get();
    if (!raw) return;
    // Teardown must run on the loop thread (mDNS core is single-threaded).
    // When the loop thread does not exist yet, start() failed early and the
    // same steps are safe on the calling thread.
    if (raw->loopThread.joinable()) {
        raw->post([raw] { raw->teardown(); });
        raw->loopThread.join();
    } else {
        raw->teardown();
    }
    impl_.reset();
    MdnsBrowser* expected = this;
    g_activeBrowser.compare_exchange_strong(expected, nullptr);
    log::info("mdns: stopped");
}

}  // namespace squeeze2raop2