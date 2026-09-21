#include "player_session.h"

#include "log.h"
#include "net_util.h"
#include "shutdown_flag.h"
#include "util.h"
#include "wav_sink.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace squeeze2raop2 {

PlayerSession::PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                             std::optional<std::string> lmsHost, uint16_t lmsPort,
                             bool paceRealtime, std::optional<std::string> sinkPath,
                             std::optional<RaopTarget> raopTarget,
                             RaopPlayer::CredentialSink credSink, VolumeMode volumeMode,
                             VolumeAnchors anchors, float volPct, int latencyMs)
    : deviceId_(std::move(deviceId)),
      name_(std::move(name)),
      mac_(mac),
      lmsHost_(std::move(lmsHost)),
      lmsPort_(lmsPort),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)),
      raopTarget_(std::move(raopTarget)),
      credSink_(std::move(credSink)),
      anchors_(std::move(anchors)),
      volumeMode_(volumeMode),
      fixedVolumePct_(volPct),
      latencyMs_(latencyMs) {}

PlayerSession::~PlayerSession() { stop(); }

void PlayerSession::start() {
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
        streamThread_ = std::jthread(
            [this](std::stop_token st) { streamLoop(st); });
    };
    events.onStop = [this]() {
        stopPlayback();
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (raop_) {
                // Drop the receiver's buffered audio BEFORE tearing
                // down: a HomePod keeps playing its ~latency jitter
                // buffer otherwise (~2-3 s tail on stop).
                raop_->flush();
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
            if (raop_) {
                raop_->flush();
                raop_->discardAudio();
            }
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
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (raop_) {
                // Silence now: the receiver's jitter buffer would keep
                // the tail playing for ~latency after the feed stops.
                raop_->flush();
                raop_->discardAudio();   // ring residue would follow the flush
            }
        }
        log::debug("pause {}", ms);
        client_->sendStat("STMp", currentStats());
    };
    events.onUnpause = [this](uint32_t) {
        std::lock_guard<std::mutex> lock(mutex_);
        pauseUntilMs_ = 0;
    };
    events.onVolume = [this](double l, double r) {
        // The recovered LMS slider percent goes through the --vol-map
        // dB anchors before it reaches the AirPlay sender's 0..100 %
        // domain (0 % = -144 mute sentinel, 100 % = 0 dB).
        double pct = (l == r) ? r : (l + r) / 2.0;
        if (volumeMode_ == VolumeMode::Fixed) {
            log::info("volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})",
                      l, r, pct, fixedVolumePct_);
            return;
        }
        pct = anchors_.airplayPctFromLms(pct);
        // NB: targetMutex_ also guards raop_ mutation in ensureRaop() and
        // the streamLoop teardown paths; no mutex_ nesting here (see the
        // onCont comment).
        std::lock_guard<std::mutex> lock(targetMutex_);
        // Remember the slider, not mute pushes: LMS's stop-fade ends at
        // gain 0 and fresh players get a 0-gain push on registration, so
        // a stored 0 would mute the next session until the first AUDG.
        if (pct > 0.0) lastLmsPct_ = pct;
        if (raop_ && raop_->active()) {
            raop_->setVolume(pct);
            log::info("[ap] volume {:.1f} pct applied (lms)", pct);
        } else {
            log::info("volume -> {:.1f} pct ({})", pct,
                      pct > 0.0 ? "remembered for next session"
                                : "mute, not remembered");
        }
    };

    client_ = std::make_unique<SlimProtoClient>(
        mac_,
        // squeezelite-style caps: Model/ModelName drive the LMS web UI
        // (player lists, settings); the player's display name is sent
        // separately via SETD name. Firmware= is the free-text version
        // LMS shows in player settings.
        "Model=squeezelite,ModelName=squeeze2raop2,AccuratePlayPoints=1,"
        "HasDigitalOut=1,MaxSampleRate=96000,"
        // Only formats the bridge actually decodes: raw PCM (headerless,
        // LMS transcode profiles like flc-pcm) and native MP3 (minimp3).
        // pcm first: local FLAC etc. transcode losslessly on the LAN;
        // mp3 second: MP3 sources (radio) stream direct regardless.
        // Do NOT advertise wav/aif/aac/flc/alc: no decoder here, and
        // direct-streamed wav/aif would ship their container headers.
        "Firmware=squeeze2raop2 v0.1.0 (m1),pcm,mp3",
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

bool PlayerSession::ensureRaop(uint32_t sampleRate) {
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
    // Scheduled stream latency: must be set BEFORE start() (it is part
    // of the RTP timeline the receiver schedules against).
    raop_->setLatencyMs(latencyMs_);
    raop_->start();
    // Set volume AFTER start(): RaopSender::start() wipes pendingVolumeDb_
    // ("never carry volume between devices"), so a pre-start setVolume is
    // lost and startStreaming_ falls back to 0 dB = full blast. Post-start
    // it only stores until the handshake finishes; startStreaming_ sends
    // the stored value before the audio pacer starts. In lms mode the
    // last AUDG slider value wins; without one yet the fixed --vol-pct
    // level covers the first seconds until LMS pushes the slider.
    if (volumeMode_ == VolumeMode::Lms && lastLmsPct_ > 0.0) {
        raop_->setVolume(lastLmsPct_);
        log::info("[ap] volume {:.1f} pct applied (remembered lms slider)",
                  lastLmsPct_);
    } else {
        raop_->setVolume(static_cast<double>(fixedVolumePct_));
        log::info("[ap] fixed volume {} pct applied (post-start)",
                  fixedVolumePct_);
    }
    if (raop_) log::info("[ap] session launching for {} ({})", name_,
                         raopTarget_->airplay2 ? "ap2" : "ap1");
    return true;
}

void PlayerSession::updateTarget(RaopTarget t) {
    std::lock_guard<std::mutex> lock(targetMutex_);
    if (raop_ && raop_->active()) return;  // in-flight audio keeps its setup
    raopTarget_ = std::move(t);
}

void PlayerSession::stop() {
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

StreamStats PlayerSession::currentStats() {
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

void PlayerSession::startStream(const StrmStart& st) {
    stopPlayback();
    flushed_.store(false);
    deviceLost_.store(false);
    retryUsed_.store(false);
    lastTitle_.clear();

    std::string host = st.serverIp ? ipv4ToString(st.serverIp)
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
    streamThread_ = std::jthread(
        [this](std::stop_token stopTok) { streamLoop(stopTok); });
}

void PlayerSession::streamLoop(std::stop_token st) {
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
            if (sink) sink->close();
            return;
        }
        raop_->setInputRate(fmt.sampleRate);
        if (fmt.channels != 2)
            log::warn("input is {}-channel; bridges Apple receivers expect stereo", fmt.channels);
    }

    uint64_t activeMs = 0;
    char buf[4096];

    // deviceLost_ doubles as the "receiver died" exit signal set from the
    // sender's io thread (streamActive_ was removed with the stop_token
    // conversion; stop requests arrive via st).
    while (!st.stop_requested() && g_run.load() && !deviceLost_.load()) {
        bool paused = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pauseUntilMs_ && nowMs() < pauseUntilMs_) paused = true;
            else pauseUntilMs_ = 0;
        }
        if (paused) {
            // Drain instead of sleep: a paused player that stops reading
            // backpressures the LMS proxy, and LMS un-pauses the stream
            // itself within seconds (observed ~5 s, with a volume fade
            // up) when its writer stalls. Keep reading + decoding and
            // discard the PCM; the sender's timeline runs on silence,
            // so an unpause resumes with fresh live audio.
            size_t got = 0;
            auto rr = reader_.read(std::span{buf}, 20);
            got = rr.bytes;
            if (rr.result == HttpStreamReader::ReadResult::Data && got > 0) {
                if (isMp3_) {
                    if (!feedMp3(st, std::as_bytes(std::span{buf}).first(got), fmt,
                                 nullptr, /*toOutput=*/false))
                        break;
                }
                // raw-pcm pause: nothing to decode, just dropped
            }
            if (rr.result == HttpStreamReader::ReadResult::AtEof) break;
            continue;
        }

        uint64_t iterStart = nowMs();
        size_t got = 0;
        auto rr = reader_.read(std::span{buf}, 150);
        got = rr.bytes;
        uint64_t iterCost = nowMs() - iterStart;

        if (rr.result == HttpStreamReader::ReadResult::Data && got > 0) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                receivedBytes_ += got;
            }
            if (isMp3_) {
                if (!feedMp3(st, std::as_bytes(std::span{buf}).first(got), fmt, sink.get()))
                    break;
            } else {
                if (sink) sink->feed(std::as_bytes(std::span{buf}).first(got), fmt);
                if (raop_ && fmt.bitsPerSample == 16) {
                    pushToRaop(st, std::as_bytes(std::span{buf}).first(got), fmt);
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

        if (rr.result == HttpStreamReader::ReadResult::AtEof) {
            if (isMp3_ && mp3_) {
                // Decode + emit the remaining tail frames of the stream.
                mp3_->finish();
                feedMp3(st, {}, fmt, sink.get());
            }
            break;
        }

        if (paceRealtime_) {
            uint64_t timeline;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                timeline = fedSamples_ * 1000ULL / fmt.sampleRate;
            }
            // Pace reads to playback time with a small lead: keeps the
            // sender's ring fed without running far ahead of the wire.
            if (timeline > activeMs + 20) {
                uint64_t sleepMs = std::min<uint64_t>(timeline - (activeMs + 20), 120);
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
    }
}

// The receiver ended the session on its own (e.g. the phone took over the
// HomePod). Stop the local stream and tell LMS with STMu so it stops the
// track instead of streaming into a dead session. Runs on the sender's
// io thread: only flag here — the streamLoop's normal exit path does the
// raop discard/stop/reset.
void PlayerSession::onRaopDeviceClosed() {
    // Runs on the sender's io thread: flag only. The streamLoop decides
    // between a transparent session retry and reporting to LMS.
    deviceLost_.store(true);
}

// ICY in-band metadata block (squeezelite parity): forward the raw
// chunk to LMS and push the StreamTitle to the AirPlay receiver.
void PlayerSession::onIcyMeta(const char* data, size_t len) {
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

void PlayerSession::pushToRaop(std::stop_token st, std::span<const std::byte> data,
                               const PcmFormat& fmt) {
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
        feedRing(st, stereo);
    } else {
        feedRing(st, samples);
    }
}

// Returns false when the decoder failed and the stream must abort.
bool PlayerSession::feedMp3(std::stop_token st, std::span<const std::byte> data,
                            PcmFormat& fmt, PcmFileSink* sink, bool toOutput) {
    constexpr size_t kPcmChunk = size_t{1152} * 2;
    if (!mp3_) return true;
    mp3_->feed(data);
    std::array<int16_t, kPcmChunk> pcm{};
    for (;;) {
        size_t n = mp3_->drain(pcm);
        if (!n) {
            if (mp3_->hasError()) {
                log::error("mp3 decode failed; dropping stream");
                client_->sendStat("STMn", currentStats());
                return false;
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
        if (!toOutput) continue;   // paused drain: decode, discard
        const size_t byteLen = n * sizeof(int16_t);
        if (sink) sink->feed(std::as_bytes(std::span{pcm}).first(byteLen), fmt);
        if (raop_) pushToRaop(st, std::as_bytes(std::span{pcm}).first(byteLen), fmt);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fedSamples_ += n / (fmt.channels ? fmt.channels : 2);
            fedBytes_ = receivedBytes_ - mp3_->pendingBytes();
        }
    }
    return true;
}

void PlayerSession::feedRing(std::stop_token st, const std::vector<int16_t>& samples) {
    size_t offset = 0;
    while (offset < samples.size() && !st.stop_requested() && g_run.load() &&
           !deviceLost_.load()) {
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

void PlayerSession::stopPlayback() {
    if (streamThread_.joinable()) {
        streamThread_.request_stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pauseUntilMs_ = 0;
            autostartPending_ = false;
        }
        streamThread_.join();
    }
    reader_.close();
}

} // namespace squeeze2raop2
