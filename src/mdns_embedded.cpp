#include "mdns.h"

#include "log.h"
#include "util.h"

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

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>

namespace squeeze2raop2 {

// DNSQuestion value-init replaces memset-0 below; that swap is only sound
// for a plain C aggregate.
static_assert(std::is_trivially_copyable_v<DNSQuestion>,
              "DNSQuestion value-init must remain equivalent to memset-0");

namespace {

#define SQUEEZE2RAOP2_RR_CACHE_SIZE 900
CacheEntity gRRCache[SQUEEZE2RAOP2_RR_CACHE_SIZE];

extern "C" {
mDNS mDNSStorage;          // the vendored code references this client-owned global
extern const char ProgramName[] = "squeeze2raop2";
} // namespace
mDNS& gMdns = mDNSStorage; // keep C++-side name
mDNS_PlatformSupport gMdnsPlatformSupport;

constexpr std::array<std::string_view, 2> kServiceTypes{"_raop._tcp", "_airplay._tcp"};
constexpr size_t kNumServiceTypes = 2;

std::string domainToString(const domainname* name) {
    char buffer[MAX_ESCAPED_DOMAIN_NAME];
    ConvertDomainNameToCString(name, buffer);
    return std::string(buffer);
}

uint16_t ipPortHostOrder(const mDNSIPPort& port) {
    return static_cast<uint16_t>((port.b[0] << 8) | port.b[1]);
}

mDNSInterfaceID g_iface = mDNSInterface_Any;

// The rdata union provides typed members; use them instead of reinterpreting
// u.data (mirrors how mDNSResponder's core reads SRV/A answers).
std::string addressFromRData4(const ResourceRecord* rr) {
    const mDNSu8 (&octets)[4] = rr->rdata->u.ipv4.b;
    return std::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
}

// one resolve tracker per service instance (owned by g_mdns_owner)
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
};

MdnsBrowser* g_owner = nullptr;
MdnsBrowser::RecordCallback g_cb;
std::map<std::string, ResolveTracker> g_resolvers;

std::string stripServiceSuffix(const std::string& fqdnIn, const std::string& serviceType) {
    std::string fqdn = fqdnIn;
    while (fqdn.size() > 1 && fqdn.back() == '.') fqdn.pop_back();
    std::string suffix = std::string(".") + serviceType + ".local";
    if (fqdn.size() >= suffix.size() &&
        fqdn.compare(fqdn.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return fqdn.substr(0, fqdn.size() - suffix.size());
    }
    return fqdn;
}

} // namespace

// All mDNS core access happens on the loop thread: mDNS is single-threaded
// and its core state has no locks. Loop-thread code (callbacks) calls the
// API directly; other threads enqueue commands through Impl::post().
struct MdnsBrowser::Impl {
    RecordCallback cb;
    std::jthread loopThread;
    mDNSInterfaceID iface = mDNSInterface_Any;
    DNSQuestion browseQ[2];
    bool browseActive[2] = {false, false};

    // command queue + wake pipe so start/stop/teardown requests run on the
    // loop thread instead of racing mDNSPosixProcessFDSet
    std::mutex cmdMutex;
    std::deque<std::function<void()>> cmdQueue;
    int wakeRd = -1;
    int wakeWr = -1;

    ~Impl() {
        // join before closing the wake fds (jthread would auto-join after
        // them, member destruction order)
        if (loopThread.joinable()) {
            loopThread.request_stop();
            loopThread.join();
        }
        if (wakeRd >= 0) ::close(wakeRd);
        if (wakeWr >= 0) ::close(wakeWr);
    }

    void post(std::function<void()> cmd) {
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            cmdQueue.push_back(std::move(cmd));
        }
        wake();
    }

    void wake() {
        if (wakeWr >= 0) {
            const char token = 1;
            (void)::write(wakeWr, &token, 1);
        }
    }

    void drainWake() {
        char buf[64];
        while (::read(wakeRd, buf, sizeof(buf)) > 0) {}
    }

    void runCommands() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            batch.swap(cmdQueue);
        }
        for (auto& cmd : batch) cmd();
    }
};

namespace {

void publishResolved(ResolveTracker& t) {
    if (t.port == 0 || t.address.empty()) {
        log::debug("mdns: resolve incomplete for {}", t.instance);
        return;
    }
    if (!g_cb) return;
    MdnsRecord record;
    record.type = t.serviceType;
    record.instance = t.instance;
    record.host = t.address;
    record.port = t.port;
    record.txt = t.txt;
    log::info("mdns: {} resolved {}:{} ({} txt keys)", t.instance, record.host,
              record.port, record.txt.size());
    g_cb(record, MdnsBrowser::RecordEvent::Added);
} // namespace

void stopTracker(ResolveTracker& t) {
    if (t.srvActive) {
        t.srvActive = false;
        mDNS_StopQuery(&gMdns, &t.srvQ);
    }
    if (t.txtActive) {
        t.txtActive = false;
        mDNS_StopQuery(&gMdns, &t.txtQ);
    }
    if (t.addrActive) {
        t.addrActive = false;
        mDNS_StopQuery(&gMdns, &t.addrQ);
    }
}

ResolveTracker* trackerForQuestion(DNSQuestion* q) {
    return static_cast<ResolveTracker*>(q->QuestionContext);
}

void resolveQuestionCb(mDNS* m, DNSQuestion* q, const ResourceRecord* rr,
                       QC_result add) {
    (void)m;
    {
        char rrtypeName[16];
        mDNS_snprintf(rrtypeName, sizeof(rrtypeName), "%u", rr->rrtype);
        log::debug("mdns: A-cb add={} rrtype={}", static_cast<int>(add), rrtypeName);
    }
    if (add != QC_add) return;

    ResolveTracker* owner = trackerForQuestion(q);
    if (!owner) return;
    ResolveTracker& t = *owner;

    if (rr->rrtype == kDNSType_A) {
        if (rr->rdlength >= 4) {
            t.address = addressFromRData4(rr);
        }
        if (t.addrActive) {
            t.addrActive = false;
            mDNS_StopQuery(&gMdns, q);
        }
    } else if (rr->rrtype == kDNSType_SRV) {
        // typed union overlay, exactly how mDNSResponder's core reads these
        t.port = ipPortHostOrder(rr->rdata->u.srv.port);
        t.targetHost = domainToString(&rr->rdata->u.srv.target);
        if (t.srvActive) {
            t.srvActive = false;
            mDNS_StopQuery(&gMdns, q);
        }
    } else if (rr->rrtype == kDNSType_TXT) {
        t.txt = parseTxtKeyValues(
            std::string(reinterpret_cast<const char*>(rr->rdata->u.data), rr->rdlength));
        if (t.txtActive) {
            t.txtActive = false;
            mDNS_StopQuery(&gMdns, q);
        }
    } else {
        return;
    }

    if (!t.srvActive && !t.txtActive && !t.addrActive) {
        if (t.port && !t.targetHost.empty() && t.address.empty()) {
            t.addrQ = DNSQuestion{};   // value-init instead of memset
            t.addrQ.QuestionContext = &t;
            MakeDomainNameFromDNSNameString(&t.addrQ.qname, t.targetHost.c_str());
            t.addrQ.InterfaceID = g_iface;
            t.addrQ.qtype = kDNSType_A;
            t.addrQ.qclass = kDNSClass_IN;
            t.addrQ.QuestionCallback = resolveQuestionCb;
            t.addrQ.ForceMCast = mDNStrue;
            if (mDNS_StartQuery(&gMdns, &t.addrQ) == mStatus_NoError) {
                t.addrActive = true;
                return;
            }
        }
        publishResolved(t);
    }
}

} // namespace

namespace {

void resetTracker(ResolveTracker& t) {
    t.srvQ = DNSQuestion{};   // value-init instead of memset
    t.txtQ = DNSQuestion{};
    t.addrQ = DNSQuestion{};
    t.srvActive = t.txtActive = t.addrActive = false;
    t.port = 0;
    t.targetHost.clear();
    t.address.clear();
    t.txt.clear();
} // namespace

bool startTracker(ResolveTracker& t, const std::string& instance,
                  const std::string& serviceType) {
    resetTracker(t);
    t.instance = instance;
    t.serviceType = serviceType;

    std::string fqdn = instance + "." + serviceType + ".local";
    t.srvQ.QuestionContext = &t;
    t.txtQ.QuestionContext = &t;
    t.addrQ.QuestionContext = &t;

    if (MakeDomainNameFromDNSNameString(&t.srvQ.qname, fqdn.c_str()) == nullptr) {
        log::warn("mdns: cannot form SRV name {}", fqdn);
        return false;
    }
    t.srvQ.InterfaceID = g_iface;
    t.srvQ.qtype = kDNSType_SRV;
    t.srvQ.qclass = kDNSClass_IN;
    t.srvQ.QuestionCallback = resolveQuestionCb;
    t.srvQ.ForceMCast = mDNStrue;
    if (mDNS_StartQuery(&gMdns, &t.srvQ) != mStatus_NoError) {
        log::warn("mdns: SRV query failed for {}", fqdn);
        return false;
    }
    t.srvActive = true;

    if (MakeDomainNameFromDNSNameString(&t.txtQ.qname, fqdn.c_str()) == nullptr) {
        log::debug("mdns: cannot form TXT name {}", fqdn);
    } else {
        t.txtQ.InterfaceID = g_iface;
        t.txtQ.qtype = kDNSType_TXT;
        t.txtQ.qclass = kDNSClass_IN;
        t.txtQ.QuestionCallback = resolveQuestionCb;
        t.txtQ.ForceMCast = mDNStrue;
        if (mDNS_StartQuery(&gMdns, &t.txtQ) == mStatus_NoError) {
            t.txtActive = true;
        }
    }
    log::debug("mdns: resolver started for {}, srv={} txt={}", instance,
               static_cast<int>(t.srvActive), static_cast<int>(t.txtActive));
    return true;
}

void startResolveForInstance(const std::string& instance, const std::string& serviceType) {
    std::string fqdn = instance + "." + serviceType + ".local";
    auto it = g_resolvers.find(fqdn);
    if (it != g_resolvers.end()) return;
    ResolveTracker& t = g_resolvers[fqdn];
    if (!startTracker(t, instance, serviceType)) {
        g_resolvers.erase(fqdn);
    }
}

void stopResolveForInstance(const std::string& instance, const std::string& serviceType) {
    std::string fqdn = instance + "." + serviceType + ".local";
    auto it = g_resolvers.find(fqdn);
    if (it == g_resolvers.end()) return;
    stopTracker(it->second);
    g_resolvers.erase(it);
}

void browseQuestionCb(mDNS* m, DNSQuestion* q, const ResourceRecord* rr, QC_result add) {
    (void)m;
    (void)q;
    if (rr->rrtype != kDNSType_PTR || rr->rdlength == 0) return;

    std::string ptrName = domainToString(&rr->rdata->u.name);
    log::debug("mdns: browse ptr '{}' type={} add={}", ptrName, rr->rrtype,
               static_cast<int>(add));
    std::string recordName = domainToString(rr->name);

    for (std::string_view serviceTypeView : kServiceTypes) {
        std::string serviceType(serviceTypeView);
        if (recordName.find(serviceType) == std::string::npos) continue;
        std::string instance = stripServiceSuffix(ptrName, serviceType);
        if (instance.empty()) continue;
        if (add == QC_add || add == QC_addnocache) {
            startResolveForInstance(instance, serviceType);
        } else {
            stopResolveForInstance(instance, serviceType);
            if (g_cb) {
                MdnsRecord record;
                record.type = serviceType;
                record.instance = instance;
                g_cb(record, MdnsBrowser::RecordEvent::Removed);
            }
        }
        return;
    }
}

} // namespace

MdnsBrowser::MdnsBrowser() = default;

MdnsBrowser::~MdnsBrowser() { stop(); }

bool MdnsBrowser::start(const std::string& ifaceName, RecordCallback cb, std::string& errorOut) {
    if (impl_ || g_owner) {
        errorOut = "mDNS browser already running (single instance per process)";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->cb = std::move(cb);
    g_cb = impl->cb;

    if (!ifaceName.empty()) {
        unsigned index = if_nametoindex(ifaceName.c_str());
        if (index == 0) {
            g_cb = nullptr;
            errorOut = std::string("cannot find interface ") + ifaceName;
            return false;
        }
        log::warn("mdns: --iface filtering not supported by embedded mDNS v1; "
                  "browsing on all interfaces");
    }
    g_iface = mDNSInterface_Any;

    if (log::level() >= log::Level::Debug) {
        mDNS_LoggingEnabled = mDNStrue;
        mDNS_DebugMode = mDNStrue;
    }

    // Everything below runs on this thread while no loop thread exists yet:
    // mDNS_Init and the browse questions are therefore race-free here.
    mStatus status = mDNS_Init(&gMdns, &gMdnsPlatformSupport, gRRCache, SQUEEZE2RAOP2_RR_CACHE_SIZE,
                               mDNS_Init_DontAdvertiseLocalAddresses,
                               mDNS_Init_NoInitCallback, mDNS_Init_NoInitCallbackContext);
    if (status != mStatus_NoError) {
        g_cb = nullptr;
        errorOut = std::string("mDNS_Init failed: ") +
                   std::to_string(static_cast<int>(status));
        return false;
    }

    for (size_t i = 0; i < kNumServiceTypes; ++i) {
        domainname srv;
        domainname dom;
        MakeDomainNameFromDNSNameString(&srv, std::string(kServiceTypes[i]).c_str());
        MakeDomainNameFromDNSNameString(&dom, "local");
        impl->browseQ[i] = DNSQuestion{};
        mStatus st = mDNS_StartBrowse(&gMdns, &impl->browseQ[i], &srv, &dom,
                                      impl->iface, 0, mDNSfalse, mDNSfalse,
                                      browseQuestionCb, impl.get());
        if (st != mStatus_NoError) {
            for (size_t j = 0; j < i; ++j) {
                if (impl->browseActive[j]) {
                    impl->browseActive[j] = false;
                    mDNS_StopQuery(&gMdns, &impl->browseQ[j]);
                }
            }
            g_cb = nullptr;
            mDNS_Close(&gMdns);
            errorOut = std::string("mDNS_StartBrowse(") + std::string(kServiceTypes[i]) +
                       ") failed: " + std::to_string(static_cast<int>(st));
            return false;
        }
        impl->browseActive[i] = true;
        log::info("mdns: browsing {}", kServiceTypes[i]);
    }

    // Wake pipe so pending commands interrupt the select() wait immediately.
    int fds[2];
    if (::pipe(fds) != 0) {
        for (size_t i = 0; i < kNumServiceTypes; ++i) {
            if (impl->browseActive[i]) {
                impl->browseActive[i] = false;
                mDNS_StopQuery(&gMdns, &impl->browseQ[i]);
            }
        }
        g_cb = nullptr;
        mDNS_Close(&gMdns);
        errorOut = "cannot create mDNS wake pipe";
        return false;
    }
    for (int fd : fds) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    impl->wakeRd = fds[0];
    impl->wakeWr = fds[1];

    Impl* raw = impl.get();
    raw->loopThread = std::jthread([raw](std::stop_token st) {
        while (!st.stop_requested()) {
            raw->runCommands();
            if (st.stop_requested()) break;
            fd_set readfds;
            fd_set writefds;
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            struct timeval timeout{};
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            int nfds = 0;
            mDNSPosixGetFDSet(&gMdns, &nfds, &readfds, &writefds, &timeout);
            if (raw->wakeRd >= 0) {
                FD_SET(raw->wakeRd, &readfds);
                if (raw->wakeRd + 1 > nfds) nfds = raw->wakeRd + 1;
            }
            int rc = ::select(nfds, &readfds, &writefds, nullptr, &timeout);
            if (rc > 0) {
                if (raw->wakeRd >= 0 && FD_ISSET(raw->wakeRd, &readfds)) raw->drainWake();
                mDNSPosixProcessFDSet(&gMdns, &readfds, &writefds);
                raw->runCommands();
            }
        }
    });

    impl_ = std::move(impl);
    g_owner = this;
    return true;
}

void MdnsBrowser::stop() {
    Impl* raw = impl_.get();
    if (!raw) return;
    // Teardown must run on the loop thread (mDNS core is single-threaded).
    // When the loop thread does not exist yet, start() failed early and the
    // same steps are safe on the calling thread.
    auto teardown = [raw]() {
        for (size_t i = 0; i < kNumServiceTypes; ++i) {
            if (raw->browseActive[i]) {
                raw->browseActive[i] = false;
                mDNS_StopQuery(&gMdns, &raw->browseQ[i]);
            }
        }
        for (auto& kv : g_resolvers) stopTracker(kv.second);
        g_resolvers.clear();
        g_cb = nullptr;
        mDNS_Close(&gMdns);
        raw->loopThread.request_stop();
    };
    if (raw->loopThread.joinable()) {
        raw->post(teardown);
        raw->loopThread.join();
    } else {
        teardown();
    }
    impl_.reset();
    if (g_owner == this) g_owner = nullptr;
    log::info("mdns: stopped");
}

}
