#include "config.h"
#include "device_registry.h"
#include "lms_stream.h"
#include "log.h"
#include "mdns.h"
#include "mp3_decoder.h"
#include "raop_player.h"
#include "slimproto.h"
#include "state_store.h"
#include "util.h"
#include "wav_sink.h"

#include <arpa/inet.h>
#include <csignal>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace squeeze2raop2 {

namespace {

std::atomic<bool> g_run{true};

void onSignal(int) { g_run.store(false); }

std::string ipToString(uint32_t netOrder) {
    in_addr a{};
    a.s_addr = htonl(netOrder);
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &a, buf, sizeof(buf))) return std::string();
    return std::string(buf);
} // namespace

class PlayerSession {
public:
    PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                  std::optional<std::string> lmsHost, uint16_t lmsPort,
                  bool paceRealtime, std::optional<std::string> sinkPath,
                  std::optional<RaopTarget> raopTarget,
                  RaopPlayer::CredentialSink credSink, float volPct)
        : deviceId_(std::move(deviceId)),
          name_(std::move(name)),
          mac_(mac),
          lmsHost_(std::move(lmsHost)),
          lmsPort_(lmsPort),
          paceRealtime_(paceRealtime),
          sinkPath_(std::move(sinkPath)),
          raopTarget_(std::move(raopTarget)),
          credSink_(std::move(credSink)),
          fixedVolumePct_(volPct) {}

    ~PlayerSession() { stop(); }

    void start() {
        SlimProtoClient::Events events;
        events.onStart = [this](const StrmStart& st) { startStream(st); };
        events.onCont = [this](uint32_t) {
            // NB: mutex_ is non-recursive — currentStats() locks it too, so
            // no currentStats()/sendStat call may happen under our lock.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!autostartPending_) return;
                autostartPending_ = false;
            }
            client_->sendStat("STMs", currentStats());
            streamActive_.store(true);
            streamThread_ = std::thread([this] { streamLoop(); });
        };
        events.onStop = [this]() {
            stopPlayback();
            {
                std::lock_guard<std::mutex> lock(targetMutex_);
                if (raop_) {
                    raop_->discardAudio();
                    raop_->stop();
                    raop_.reset();
                }
            }
            client_->sendStat("STMf", currentStats());
        };
        events.onFlush = [this](bool) {
            // Track transitions send 'strm f': drop buffered audio but KEEP
            // the AirPlay session — the next track's audio keeps streaming
            // through it (no ~1 s re-pairing gap). The streamLoop's exit
            // path honors flushed_ by skipping the session teardown.
            flushed_.store(true);
            stopPlayback();
            {
                std::lock_guard<std::mutex> lock(targetMutex_);
                if (raop_) raop_->discardAudio();
            }
            client_->sendStat("STMf", currentStats());
        };
        events.onPause = [this](uint32_t ms) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a
                // timed pause (transition gaps). LMS's stop for a remote
                // stream is a fade-down followed by 'p 0'.
                pauseUntilMs_ = ms ? nowMs() + ms
                                   : std::numeric_limits<uint64_t>::max();
            }
            log::debug("pause {}", ms);
            client_->sendStat("STMp", currentStats());
        };
        events.onUnpause = [this](uint32_t) {
            std::lock_guard<std::mutex> lock(mutex_);
            pauseUntilMs_ = 0;
        };
        events.onVolume = [this](double l, double r) {
            // Volume control via LMS mixer is deferred (LMS 9.1 sends mute-form
            // AUDG gains at low sliders and the mapping needs a calibration
            // pass); sessions run at the fixed --vol-pct level instead.
            double pct = (l == r) ? r : (l + r) / 2.0;
            log::info("volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})",
                      l, r, pct, fixedVolumePct_);
            (void)pct;
        };

        client_ = std::make_unique<SlimProtoClient>(
            mac_,
            "Model=squeeze2raop2,ModelName=" + name_ +
                ",AccuratePlayPoints=1,HasDigitalOut=1,MaxSampleRate=96000,"
                "Firmware=squeeze2raop2-m1,aac,flc,alc,wav,aif,pcm,mp3",
            std::move(events));
        client_->setPlayerName(name_);
        client_->setStatsProvider([this] { return currentStats(); });
        client_->start(lmsHost_.value_or(""), lmsPort_);

        std::string macText = macToString(mac_);
        std::string identity = macText;
        std::ranges::transform(identity, identity.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::erase(identity, ':');
        raopIdentity_ = identity;
        // the AirPlay session is launched lazily in ensureRaop() when the
        // first audio arrives; connecting eagerly hits receivers that
        // immediately drop idle sessions (HomePod/Sonos)
    }

    bool ensureRaop(uint32_t sampleRate) {
        // raop_/raopTarget_ are also read (locked) from SessionManager's
        // thread via updateTarget(); everything here runs on the stream
        // thread and must therefore hold targetMutex_ while mutating them.
        std::lock_guard<std::mutex> lock(targetMutex_);
        if (raop_ && raop_->active()) return true;
        if (raop_) {   // dead session (e.g. receiver teardown): recreate
            raop_->stop();
            raop_.reset();
        }
        if (!raopTarget_) return false;
        RaopPlayer::CredentialSink sink = credSink_;
        raop_ = std::make_unique<RaopPlayer>(name_, raopIdentity_, *raopTarget_);
        raop_->setCredentialSink(sink);
        raop_->setClosedCallback([this] { onRaopDeviceClosed(); });
        raop_->setInputRate(sampleRate);
        raop_->start();
        // Set volume AFTER start(): RaopSender::start() wipes pendingVolumeDb_
        // ("never carry volume between devices"), so a pre-start setVolume is
        // lost and startStreaming_ falls back to 0 dB = full blast. Post-start
        // it only stores until the handshake finishes; startStreaming_ sends
        // the stored value before the audio pacer starts.
        raop_->setVolume(static_cast<double>(fixedVolumePct_));
        log::info("[ap] fixed volume {} pct applied (post-start)", fixedVolumePct_);
        if (raop_) log::info("[ap] session launching for {} ({})", name_,
                             raopTarget_->airplay2 ? "ap2" : "ap1");
        return true;
    }

    void updateTarget(RaopTarget t) {
        std::lock_guard<std::mutex> lock(targetMutex_);
        if (raop_ && raop_->active()) return;  // in-flight audio keeps its setup
        raopTarget_ = std::move(t);
    }

    void stop() {
        // Join the stream thread first: it reads raop_ throughout streamLoop,
        // so the session must outlive it (teardown used to happen first).
        stopPlayback();
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (raop_) {
                raop_->stop();
                raop_.reset();
            }
        }
        if (client_) client_->stop();
    }

private:
    StreamStats currentStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        StreamStats st;
        st.streamBufferSize = 1 << 20;
        st.streamBufferFullness = static_cast<uint32_t>(
            std::max<int64_t>(0, static_cast<int64_t>(receivedBytes_ - fedBytes_)));
        st.bytesReceived = receivedBytes_;
        st.outputBufferSize = 0;
        st.outputBufferFullness = 0;
        uint32_t rate = format_.sampleRate ? format_.sampleRate : 44100;
        st.elapsedMs = static_cast<uint32_t>(fedSamples_ * 1000ULL / rate);
        return st;
    }

    void startStream(const StrmStart& st) {
        stopPlayback();
        flushed_.store(false);
        deviceLost_.store(false);
        retryUsed_.store(false);
        lastTitle_.clear();

        std::string host = st.serverIp ? ipToString(st.serverIp)
                                       : (client_ ? client_->serverHost() : std::string());
        uint16_t port = st.serverPort ? st.serverPort : 9000;
        if (host.empty() || st.request.empty()) {
            log::error("strm-s missing stream target or request header");
            client_->sendStat("STMn", currentStats());
            return;
        }

        std::string error;
        {
            std::string firstLine = st.request.substr(0, st.request.find("\r\n"));
            log::info("stream GET {}:{} icy={}", host, port,
                      st.request.find("Icy-MetaData") != std::string::npos ? "req" : "none");
            (void)firstLine;
        }
        // Ask for in-band ICY metadata on LMS-proxied streams (the embedded
        // request is bare): LMS's /stream.mp3 only interleaves StreamTitle
        // blocks when the client sends Icy-MetaData: 1 — the same request
        // LMS itself makes to remote servers (Protocols/HTTP.pm requestString).
        std::string request = st.request;
        if (request.find("Icy-MetaData") == std::string::npos) {
            const std::string hdr = "Icy-MetaData: 1\r\n";
            const auto end = request.find("\r\n\r\n");
            if (end != std::string::npos)
                request.insert(end + 2, hdr);
            else
                request += (request.size() >= 2 &&
                            request.compare(request.size() - 2, 2, "\r\n") == 0)
                               ? hdr
                               : "\r\n" + hdr;
        }
        if (!reader_.openBlocking(host, port, request, error)) {
            log::error("stream connect {}:{} failed: {}", host, port, error);
            client_->sendStat("STMn", currentStats());
            return;
        }
        reader_.setMetaCallback([this](const char* d, size_t n) { onIcyMeta(d, n); });
        client_->sendResp(reader_.headers());
        client_->sendStat("STMc", currentStats());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            receivedBytes_ = 0;
            fedBytes_ = 0;
            fedSamples_ = 0;
            pauseUntilMs_ = 0;
            format_ = pcmFormat(st.pcm, 44100);
            bytesPerFrame_ = format_.channels * (format_.bitsPerSample / 8);
            if (!bytesPerFrame_) bytesPerFrame_ = 4;
            isMp3_ = (st.format == StreamFormat::Mp3);
            mp3_.reset();
            if (isMp3_) {
                mp3_ = std::make_unique<Mp3Decoder>();
                log::info("strm s: mp3 native stream, decoding locally");
            }
        }

        if (st.autostart >= 2) {
            std::lock_guard<std::mutex> lock(mutex_);
            autostartPending_ = true;
            log::info("autostart={}, waiting for cont", st.autostart);
            return;
        }
        client_->sendStat("STMs", currentStats());
        streamActive_.store(true);
        streamThread_ = std::thread([this] { streamLoop(); });
    }

    void streamLoop() {
        PcmFormat fmt;
        uint32_t bytesPerFrame = 4;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fmt = format_;
            bytesPerFrame = bytesPerFrame_;
        }

        std::unique_ptr<PcmFileSink> sink;
        if (sinkPath_) {
            std::string error;
            sink = std::make_unique<PcmFileSink>(*sinkPath_);
            if (!sink->open(fmt, error)) {
                log::error("cannot open sink: {}", error);
                client_->sendStat("STMn", currentStats());
                streamActive_.store(false);
                return;
            }
        }
        std::optional<RaopTarget> target;
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            target = raopTarget_;
        }
        if (target && target->port) {
            if (!ensureRaop(fmt.sampleRate)) {
                log::error("[ap] cannot start airplay session for {}", name_);
                client_->sendStat("STMn", currentStats());
                streamActive_.store(false);
                if (sink) sink->close();
                return;
            }
            raop_->setInputRate(fmt.sampleRate);
            if (fmt.channels != 2)
                log::warn("input is {}-channel; bridges Apple receivers expect stereo", fmt.channels);
        }

        uint64_t activeMs = 0;
        char buf[4096];

        while (g_run.load() && streamActive_.load()) {
            bool paused = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pauseUntilMs_ && nowMs() < pauseUntilMs_) paused = true;
                else pauseUntilMs_ = 0;
            }
            if (paused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            uint64_t iterStart = nowMs();
            size_t got = 0;
            auto rr = reader_.read(buf, sizeof(buf), &got, 150);
            uint64_t iterCost = nowMs() - iterStart;

            if (rr == HttpStreamReader::ReadResult::Data && got > 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    receivedBytes_ += got;
                }
                if (isMp3_) {
                    feedMp3(std::as_bytes(std::span{buf}).first(got), fmt, sink.get());
                } else {
                    if (sink) sink->feed(std::as_bytes(std::span{buf}).first(got), fmt);
                    if (raop_ && fmt.bitsPerSample == 16) {
                        pushToRaop(std::as_bytes(std::span{buf}).first(got), fmt);
                    } else if (raop_) {
                        log::warn("raop feed requires 16-bit pcm; dropping {} bytes", got);
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    fedBytes_ += got;
                    fedSamples_ += got / bytesPerFrame;
                }
                activeMs += iterCost;
            } else {
                activeMs += 150;
            }

            if (rr == HttpStreamReader::ReadResult::AtEof) {
                if (isMp3_ && mp3_) {
                    // Decode + emit the remaining tail frames of the stream.
                    mp3_->finish();
                    feedMp3({}, fmt, sink.get());
                }
                break;
            }

            if (paceRealtime_) {
                uint64_t timeline;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    timeline = fedSamples_ * 1000ULL / fmt.sampleRate;
                }
                if (timeline > activeMs + 60) {
                    uint64_t sleepMs = std::min<uint64_t>(timeline - (activeMs + 60), 120);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                    activeMs += sleepMs;
                }
            }
        }

        if (sink) sink->close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            log::info("stream ended, received={} bytes", receivedBytes_);
        }

        // Exit handling: a device-initiated session loss gets ONE transparent
        // retry (the receiver may have been transiently busy); otherwise the
        // stream ends for real. A flush transition keeps the session for the
        // next track.
        for (;;) {
            const bool keepSession = flushed_.exchange(false);
            const bool lost = deviceLost_.exchange(false);
            {
                std::lock_guard<std::mutex> lock(targetMutex_);
                if (!keepSession && raop_) {
                    raop_->discardAudio();
                    raop_->stop();
                    raop_.reset();
                }
            }
            if (!g_run.load()) break;
            if (!lost) {
                client_->sendStat("STMu", currentStats());   // normal end
                break;
            }
            if (retryUsed_.exchange(true)) {
                log::info("[ap] receiver session lost again; giving up (STMd)");
                client_->sendStat("STMd", currentStats());
                break;
            }
            log::info("[ap] receiver session lost; retrying in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!ensureRaop(fmt.sampleRate)) {
                log::error("[ap] cannot restart airplay session for {}", name_);
                client_->sendStat("STMn", currentStats());
                break;
            }
            if (raop_ && !lastTitle_.empty())
                raop_->setNowPlaying(lastTitle_, "", "");
            streamActive_.store(true);
        }
        streamActive_.store(false);
    }

    // The receiver ended the session on its own (e.g. the phone took over the
    // HomePod). Stop the local stream and tell LMS with STMu so it stops the
    // track instead of streaming into a dead session. Runs on the sender's
    // io thread: only flag here — the streamLoop's normal exit path does the
    // raop discard/stop/reset (no locking needed beyond streamActive_).
    void onRaopDeviceClosed() {
        // Runs on the sender's io thread: flag only. The streamLoop decides
        // between a transparent session retry and reporting to LMS.
        deviceLost_.store(true);
        streamActive_.store(false);
    }

    // ICY in-band metadata block (squeezelite parity): forward the raw
    // chunk to LMS and push the StreamTitle to the AirPlay receiver.
    void onIcyMeta(const char* data, size_t len) {
        client_->sendMeta(data, len);
        const std::string raw(data, len);
        const std::string key = "StreamTitle='";
        const auto p = raw.find(key);
        if (p == std::string::npos) return;
        const auto e = raw.find('\'', p + key.size());
        if (e == std::string::npos) return;
        const std::string title = raw.substr(p + key.size(), e - p - key.size());
        if (title.empty()) return;
        log::info("icy title: {}", title);
        lastTitle_ = title;
        if (raop_) raop_->setNowPlaying(title, "", "");
    }

    void pushToRaop(std::span<const std::byte> data, const PcmFormat& fmt) {
        std::vector<int16_t> samples;
        const size_t count = data.size() / 2;
        samples.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            // memcpy extraction instead of reinterpret_cast: no int16_t
            // object ever lives in the receive buffer, so pointer-casting
            // the raw bytes is strict-aliasing UB.
            uint16_t raw = 0;
            std::memcpy(&raw, data.data() + 2 * i, 2);
            samples.push_back(fmt.bigEndian ? static_cast<int16_t>(ntohs(raw))
                                            : static_cast<int16_t>(raw));
        }
        if (fmt.channels == 1 && !samples.empty()) {
            std::vector<int16_t> stereo;
            stereo.reserve(samples.size() * 2);
            for (int16_t s : samples) {
                stereo.push_back(s);
                stereo.push_back(s);
            }
            feedRing(stereo);
        } else {
            feedRing(samples);
        }
    }

    void feedMp3(std::span<const std::byte> data, PcmFormat& fmt, PcmFileSink* sink) {
        constexpr size_t kPcmChunk = size_t{1152} * 2;
        if (!mp3_) return;
        mp3_->feed(data);
        std::array<int16_t, kPcmChunk> pcm{};
        for (;;) {
            size_t n = mp3_->drain(pcm);
            if (!n) {
                if (mp3_->hasError()) {
                    log::error("mp3 decode failed; dropping stream");
                    client_->sendStat("STMn", currentStats());
                    streamActive_.store(false);
                }
                break;
            }
            if (mp3_->valid() &&
                (fmt.sampleRate != mp3_->sampleRate() || fmt.channels != mp3_->channels() ||
                 fmt.bitsPerSample != 16 || fmt.bigEndian)) {
                fmt = PcmFormat{.sampleRate = mp3_->sampleRate(),
                                .bitsPerSample = 16,
                                .channels = static_cast<uint8_t>(mp3_->channels()),
                                .bigEndian = false};
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    format_ = fmt;
                    bytesPerFrame_ = 2 * fmt.channels;
                }
                if (raop_) raop_->setInputRate(fmt.sampleRate);
                log::info("[ap] mp3 audio: {} Hz, {} ch", fmt.sampleRate, fmt.channels);
            }
            const size_t byteLen = n * sizeof(int16_t);
            if (sink) sink->feed(std::as_bytes(std::span{pcm}).first(byteLen), fmt);
            if (raop_) pushToRaop(std::as_bytes(std::span{pcm}).first(byteLen), fmt);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                fedSamples_ += n / (fmt.channels ? fmt.channels : 2);
                fedBytes_ = receivedBytes_ - mp3_->pendingBytes();
            }
        }
    }

    void feedRing(const std::vector<int16_t>& samples) {
        size_t offset = 0;
        while (offset < samples.size() && g_run.load() && streamActive_.load()) {
            size_t freeSpace = raop_->availableWrite();
            if (freeSpace == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            size_t take = std::min(freeSpace, samples.size() - offset);
            if (!raop_->push(std::span<const int16_t>(samples.data() + offset, take))) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            offset += take;
        }
    }

    void stopPlayback() {
        if (streamThread_.joinable()) {
            streamActive_.store(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pauseUntilMs_ = 0;
                autostartPending_ = false;
            }
            streamThread_.join();
        }
        reader_.close();
    }

    std::string deviceId_;
    std::string name_;
    std::array<uint8_t, 6> mac_{};
    std::optional<std::string> lmsHost_;
    uint16_t lmsPort_;
    bool paceRealtime_;
    std::optional<std::string> sinkPath_;

    std::unique_ptr<SlimProtoClient> client_;

    HttpStreamReader reader_;
    std::thread streamThread_;
    std::atomic<bool> streamActive_{false};
    bool autostartPending_ = false;

    PcmFormat format_{};
    uint32_t bytesPerFrame_ = 4;

    std::unique_ptr<Mp3Decoder> mp3_;
    bool isMp3_ = false;

    std::mutex mutex_;
    std::mutex targetMutex_;
    uint64_t pauseUntilMs_ = 0;
    uint64_t receivedBytes_ = 0;
    uint64_t fedBytes_ = 0;
    uint64_t fedSamples_ = 0;

    std::optional<RaopTarget> raopTarget_;
    RaopPlayer::CredentialSink credSink_;
    std::unique_ptr<RaopPlayer> raop_;
    std::string raopIdentity_;
    // Session resilience flags (see streamLoop exit handling + callbacks).
    std::atomic<bool> flushed_{false};     // strm f: keep session for next track
    std::atomic<bool> deviceLost_{false};  // receiver ended the session
    std::atomic<bool> retryUsed_{false};   // one transparent retry per stream
    std::string lastTitle_;                // re-applied on session recreate
    // Fixed AirPlay volume percent (--vol-pct), applied to every session
    // before RECORD so audio never starts at the receiver's hardware default.
    float fixedVolumePct_;
};

class SessionManager {
public:
    SessionManager(const Settings& settings, StateStore& store)
        : settings_(settings), store_(store) {}

    void onRegistryEvent(DeviceRegistry::Event ev, const AirplayDevice& dev) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ev == DeviceRegistry::Event::Removed) {
            auto it = sessions_.find(dev.id);
            if (it == sessions_.end()) return;
            log::info("session closed: {} ({})", dev.name, dev.id);
            it->second.reset();   // ~PlayerSession stops client + stream
            sessions_.erase(it);
            return;
        }
        if (ev == DeviceRegistry::Event::Updated) {
            auto it = sessions_.find(dev.id);
            if (it == sessions_.end()) return;
            if (!dev.host.empty() && (dev.hasRaop() || dev.hasAirplay())) {
                RaopTarget t;
                t.host = dev.host;
                t.port = dev.airplay2() ? (dev.airplayPort ? dev.airplayPort : dev.raopPort)
                                        : (dev.raopPort ? dev.raopPort : dev.airplayPort);
                t.airplay2 = dev.airplay2();
                t.password = settings_.ap.password;
                t.storedCreds = store_.credsFor(dev.id).value_or(std::string());
                it->second->updateTarget(t);
                if (t.port)
                    log::debug("session target refreshed: {} {}:{}",
                               dev.name, t.host, t.port);
            }
            return;
        }
        if (ev != DeviceRegistry::Event::Added) return;
        if (sessions_.count(dev.id)) return;

        bool assigned = false;
        std::array<uint8_t, 6> mac = store_.macFor(dev.id, assigned);
        std::optional<std::string> sinkPath;
        if (settings_.sinkPath) {
            std::string base = *settings_.sinkPath;
            size_t dot = base.find_last_of('.');
            std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
            std::string ext = (dot != std::string::npos) ? base.substr(dot) : "";
            sinkPath = stem + "-" + dev.id + ext;
        }
        log::info("session created: {} mac={} ({} {}:{}{}{})",
                  dev.name, macToString(mac), dev.airplay2() ? "ap2" : "ap1",
                  dev.host, dev.airplay2() ? dev.airplayPort : dev.raopPort,
                  dev.pw ? " password" : "", assigned ? " new-mac" : "");

        std::optional<RaopTarget> raopTarget;
        if (settings_.ap.enabled) {
            RaopTarget target;
            target.host = settings_.ap.host;
            target.port = settings_.ap.port;
            target.airplay2 = settings_.ap.airplay2;
            target.password = settings_.ap.password;
            target.storedCreds = store_.credsFor(dev.id).value_or(std::string());
            raopTarget = target;
        } else if (!dev.host.empty()) {
            RaopTarget target;
            target.host = dev.host;
            target.port = dev.airplay2() ? dev.airplayPort : dev.raopPort;
            target.airplay2 = dev.airplay2();
            target.password = settings_.ap.password;
            target.storedCreds = store_.credsFor(dev.id).value_or(std::string());
            raopTarget = target;
        }

        auto session = std::make_unique<PlayerSession>(
            dev.id, dev.name, mac, settings_.lmsHost, settings_.lmsPort,
            settings_.paceRealtime, sinkPath, raopTarget,
            [this, devId = dev.id](const std::string& deviceId, const std::string& creds) {
                (void)deviceId;
                store_.saveCreds(devId, creds);
            },
            settings_.volPct);
        session->start();
        sessions_[dev.id] = std::move(session);
    }

private:
    const Settings& settings_;
    StateStore& store_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<PlayerSession>> sessions_;
};

} // namespace

void runBridge(const Settings& settings) {
    // Signals are registered here so SIGINT/SIGTERM flip the g_run flag the
    // run loops actually poll (main.cpp's handler used to set a separate
    // anonymous-namespace flag nobody read, so the process ignored SIGTERM).
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    StateStore store;
    std::string error;
    if (!store.open(settings.statePath, error)) {
        log::error("state store: {}", error);
        return;
    }

    DeviceRegistry registry;
    SessionManager manager(settings, store);
    registry.setCallback([&manager](DeviceRegistry::Event ev, const AirplayDevice& dev) {
        manager.onRegistryEvent(ev, dev);
    });

    for (const auto& [id, name] : settings.staticDevices) {
        AirplayDevice dev;
        dev.id = id;
        dev.name = name;
        manager.onRegistryEvent(DeviceRegistry::Event::Added, dev);
    }

    const PlayerSettings& mainPlayer = settings.players.front();
    if (settings.staticDevices.empty() &&
        (mainPlayer.explicitName || mainPlayer.explicitMac || settings.sinkPath)) {
        AirplayDevice dev;
        dev.id = mainPlayer.deviceId;
        dev.name = mainPlayer.name;
        manager.onRegistryEvent(DeviceRegistry::Event::Added, dev);
    }

    if (settings.mdnsDebug) {
        MdnsBrowser browser;
        MdnsBrowser::RecordCallback cb = [&registry](const MdnsRecord& rec,
                                                     MdnsBrowser::RecordEvent ev) {
            const char* what = (ev == MdnsBrowser::RecordEvent::Added) ? "added" : "removed";
            log::info("mdns[{}] {} {} -> {}\n  txt:", what, rec.type, rec.instance,
                      rec.port ? rec.host + ":" + std::to_string(rec.port) : std::string());
            for (const auto& [k, v] : rec.txt) log::info("   {}={}", k, v);
        };
        if (!browser.start(settings.mdnsIface, cb, error)) {
            log::error("mdns: {}", error);
            return;
        }
        log::info("mdns debug mode: browsing (ctrl-c to exit)");
        while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        browser.stop();
        return;
    }

    if (!settings.discovery) {
        log::info("discovery disabled; running static devices only");
    }
    MdnsBrowser browser;
    MdnsBrowser::RecordCallback cb = [&registry](const MdnsRecord& rec,
                                                MdnsBrowser::RecordEvent ev) {
        if (rec.type == "_raop._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onRaopV4(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onRaopGone(rec.instance);
        } else if (rec.type == "_airplay._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onAirplayV4(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onAirplayGone(rec.instance);
        }
    };
    if (settings.discovery) {
        if (!browser.start(settings.mdnsIface, cb, error)) {
            log::warn("mdns: {} (continuing without discovery)", error);
        } else {
            log::info("discovering airplay devices");
        }
    }

    log::info("discovering airplay devices (ctrl-c to exit)");
    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    browser.stop();
    log::info("bye");
} // namespace squeeze2raop2

}
