#include "player_session.h"

#include "log.h"
#include "net_util.h"
#include "shutdown_flag.h"
#include "util.h"
#include "wav_sink.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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
        streamThread_ = std::jthread([this](std::stop_token st) { streamLoop(st); });
    };
    events.onStop = [this]() {
        stopPlayback();
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            teardownReceiverAudio(true);
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
            teardownReceiverAudio(false);
        }
        client_->sendStat("STMf", currentStats());
    };
    events.onPause = [this](uint32_t ms) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a
            // timed pause (transition gaps). LMS's stop for a remote
            // stream is a fade-down followed by 'p 0'.
            pauseUntilMs_ = ms ? nowMs() + ms : std::numeric_limits<uint64_t>::max();
        }
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            teardownReceiverAudio(false);
        }
        log::debug("pause {}", ms);
        // STMp is sent by the slimproto 'p' handler (squeezelite parity);
        // sending it here too duplicates the event.
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
            log::info("volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})", l, r, pct,
                      fixedVolumePct_);
            return;
        }
        pct = anchors_.airplayPctFromLms(pct);
        // NB: targetMutex_ also guards raop_ mutation in prepareAirplaySession() and
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
                      pct > 0.0 ? "remembered for next session" : "mute, not remembered");
        }
    };

    // Compute every immutable value a callback or thread reads BEFORE any
    // thread is created: thread creation provides the happens-before edge, so
    // setting raopIdentity_ after client_->start() would race the reader/stream
    // thread that reads it in prepareAirplaySession().
    std::string identity = macToString(mac_);
    std::ranges::transform(identity, identity.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::erase(identity, ':');
    raopIdentity_ = identity;

    // squeezelite-style caps: Model/ModelName drive the LMS web UI (player
    // lists, settings); the player's display name is sent separately via
    // SETD name. Firmware= is the free-text version LMS shows in player
    // settings. ModelName carries the AirPlay transport in use so the LMS UI
    // distinguishes the classic RAOP/AP1 path from the native AirPlay 2 path.
    const std::string modelName =
        raopTarget_ ? (raopTarget_->airplay2 ? "squeeze2raop2@ap2" : "squeeze2raop2@raop")
                    : "squeeze2raop2";
    client_ = std::make_unique<SlimProtoClient>(
        mac_,
        "Model=squeezelite,ModelName=" + modelName +
            ",AccuratePlayPoints=1,HasDigitalOut=1,MaxSampleRate=96000,"
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
    // the AirPlay session is prepared lazily in prepareAirplaySession()
    // when the first audio arrives; connecting eagerly hits receivers that
    // immediately drop idle sessions (HomePod/Sonos)
}

std::shared_ptr<RaopPlayer> PlayerSession::raopSnapshot() const {
    std::lock_guard<std::mutex> lock(targetMutex_);
    return raop_;
}

// Creates or reuses the AirPlay player WITHOUT starting it: the streamLoop
// prebuffers 50% of the ring capacity first and only calls
// launchAirplaySession() once the reserve is in hand (a sender that starts
// on an empty ring silence-pads its way into the session).
bool PlayerSession::prepareAirplaySession(uint32_t sampleRate) {
    // raop_/raopTarget_ are also read (locked) from SessionManager's
    // thread via updateTarget(); everything here runs on the stream
    // thread and must therefore hold targetMutex_ while mutating them.
    std::lock_guard<std::mutex> lock(targetMutex_);
    if (raop_ && (raop_->active() || !raop_->launched())) return true;
    if (raop_) {  // dead session (e.g. receiver teardown): recreate
        raop_->stop();
        raop_.reset();
    }
    if (!raopTarget_) return false;
    RaopPlayer::CredentialSink sink = credSink_;
    raop_ = std::make_shared<RaopPlayer>(name_, raopIdentity_, *raopTarget_);
    raop_->setCredentialSink(sink);
    raop_->setClosedCallback([this] { onRaopDeviceClosed(); });
    raop_->setInputRate(sampleRate);
    // Scheduled stream latency: must be set BEFORE start() (it is part
    // of the RTP timeline the receiver schedules against).
    raop_->setLatencyMs(latencyMs_);
    return true;
}

// Start the AirPlay sender (fresh session) or leave a live one running.
// Volume must be applied AFTER start(): RaopSender::start() wipes
// pendingVolumeDb_ ("never carry volume between devices"), so a pre-start
// setVolume is lost and startStreaming_ falls back to 0 dB = full blast.
// Post-start it only stores until the handshake finishes; startStreaming_
// sends the stored value before the audio pacer starts. In lms mode the
// last AUDG slider value wins; without one yet the fixed --vol-pct level
// covers the first seconds until LMS pushes the slider.
void PlayerSession::launchAirplaySession() {
    std::lock_guard<std::mutex> lock(targetMutex_);
    if (!raop_) return;
    if (!raop_->active()) {
        log::info("[ap] session launching for {} ({})", name_,
                  raopTarget_ && raopTarget_->airplay2 ? "ap2" : "ap1");
        raop_->start();
    }
    if (volumeMode_ == VolumeMode::Lms && lastLmsPct_ > 0.0) {
        raop_->setVolume(lastLmsPct_);
        log::info("[ap] volume {:.1f} pct applied (remembered lms slider)", lastLmsPct_);
    } else {
        raop_->setVolume(static_cast<double>(fixedVolumePct_));
        log::info("[ap] fixed volume {} pct applied (post-start)", fixedVolumePct_);
    }
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
    const uint32_t rate = elapsedRate_.load(std::memory_order_relaxed);
    // Report played time, not decoded time: the pipeline decodes ahead into
    // the sender ring, so fedSamples_ leads the receiver by up to the ring
    // occupancy (~1.5 s). LMS derives the progress display from this value,
    // and a decoded-ahead figure makes it run ahead and jump at track end
    // (squeezelite reports frames_played the same way). queuedSamples_ is
    // refreshed by the stream thread's telemetry; subtract the frames still
    // queued to approximate the played position.
    const uint64_t queuedFrames = queuedSamples_.load(std::memory_order_relaxed) / 2;
    const uint64_t playedFrames = fedSamples_ > queuedFrames ? fedSamples_ - queuedFrames : 0;
    st.elapsedMs = static_cast<uint32_t>(playedFrames * 1000ULL / rate);
    return st;
}

void PlayerSession::startStream(const StrmStart& st) {
    stopPlayback();
    flushed_.store(false);
    deviceLost_.store(false);
    retryUsed_.store(false);
    lastTitle_.clear();

    std::string host =
        st.serverIp ? ipv4ToString(st.serverIp) : (client_ ? client_->serverHost() : std::string());
    uint16_t port = st.serverPort ? st.serverPort : 9000;
    if (host.empty() || st.request.empty()) {
        log::error("strm-s missing stream target or request header");
        client_->sendStat("STMn", currentStats());
        return;
    }
    // Only formats the streamLoop actually consumes. LMS should honor the
    // HELO caps (pcm,mp3); a stray direct format would otherwise be pushed
    // into the ring as raw PCM = noise.
    if (st.format != StreamFormat::Pcm && st.format != StreamFormat::Mp3) {
        log::error("strm s: unsupported stream format '{}'", static_cast<char>(st.format));
        client_->sendStat("STMn", currentStats());
        return;
    }

    log::info("stream GET {}:{} icy={}", host, port,
              st.request.find("Icy-MetaData") != std::string::npos ? "req" : "none");
    std::string error;
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
            request += (request.size() >= 2 && request.compare(request.size() - 2, 2, "\r\n") == 0)
                           ? hdr
                           : "\r\n" + hdr;
    }
    if (!reader_.openBlocking(host, port, request, error)) {
        log::error("stream connect {}:{} failed: {}", host, port, error);
        client_->sendStat("STMn", currentStats());
        return;
    }
    reader_.setMetaCallback([this](std::string_view block) { onIcyMeta(block); });
    client_->sendResp(reader_.headers());
    client_->sendStat("STMc", currentStats());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        receivedBytes_ = 0;
        fedBytes_ = 0;
        fedSamples_ = 0;
        pauseUntilMs_ = 0;
        queuedSamples_.store(0, std::memory_order_relaxed);
        const PcmFormat input = pcmFormat(st.pcm, 44100);
        elapsedRate_.store(input.sampleRate ? input.sampleRate : 44100, std::memory_order_relaxed);
        // One decode stage per stream format, one feed pipeline for both. PCM
        // regulates to the AirPlay output clock (44100) so a source that
        // under-delivers cannot drain the pipeline.
        stage_ = std::make_unique<DecodeStage>(st.format, input, 44100);
        log::info("strm s: {} stream via decoder pipeline", stage_->name());
    }

    if (st.autostart >= 2) {
        std::lock_guard<std::mutex> lock(mutex_);
        autostartPending_ = true;
        log::info("autostart={}, waiting for cont", st.autostart);
        return;
    }
    client_->sendStat("STMs", currentStats());
    streamThread_ = std::jthread([this](std::stop_token stopTok) { streamLoop(stopTok); });
}

void PlayerSession::streamLoop(std::stop_token st) {
    PcmFormat fmt;
    if (stage_) fmt = stage_->format();

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
    const bool haveTarget = target && target->port;
    if (haveTarget) {
        if (!prepareAirplaySession(fmt.sampleRate)) {
            log::error("[ap] cannot start airplay session for {}", name_);
            client_->sendStat("STMn", currentStats());
            if (sink) sink->close();
            return;
        }
        // (input rate is applied inside prepareAirplaySession; the only later
        // setInputRate is feedStream's mid-stream format-adoption update)
        if (fmt.channels != 2)
            log::warn("input is {}-channel; bridges Apple receivers expect stereo", fmt.channels);
    }

    uint64_t activeMs = 0;
    char buf[4096];

    // Streaming phase: entered once and re-entered after a single transparent
    // receiver-loss retry. Each entry re-arms the prebuffer gate because a
    // retry creates a fresh ring.
    for (;;) {
        // Startup fill gate: pump into the ring until it holds 50% of capacity
        // (~1.5 s of audio at 44.1 kHz stereo) before the first RTP packet
        // leaves, so playback launches from a deep reserve instead of a sender
        // silence-padding its way into the session.
        size_t prebufferSamples = 0;
        if (haveTarget) {
            std::lock_guard<std::mutex> lock(targetMutex_);
            prebufferSamples = raop_ ? raop_->bufferCapacity() / 2 : 0;
        }
        bool streaming = !haveTarget || prebufferSamples == 0;
        const uint64_t prebufferStartMs = nowMs();
        if (!streaming) log::info("[ap] prebuffering {} samples (50% of ring)", prebufferSamples);

        // Set when the HTTP source reaches EOF (natural end of the track), as
        // opposed to a stop request (new track / pause) or a socket error.
        bool reachedEof = false;

        // deviceLost_ doubles as the "receiver died" exit signal set from the
        // sender's io thread (streamActive_ was removed with the stop_token
        // conversion; stop requests arrive via st).
        while (!st.stop_requested() && g_run.load() && !deviceLost_.load()) {
            bool paused = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pauseUntilMs_ && nowMs() < pauseUntilMs_)
                    paused = true;
                else
                    pauseUntilMs_ = 0;
            }
            if (!paused && streaming) sampleRingTelemetry();
            if (paused) {
                // Drain instead of sleep: a paused player that stops reading
                // backpressures the LMS proxy, and LMS un-pauses the stream
                // itself within seconds (observed ~5 s, with a volume fade
                // up) when its writer stalls. Keep reading + decoding and
                // discard the PCM (toOutput=false also skips the elapsed
                // accounting, so progress stays frozen). Discarding is what
                // keeps a resumed *live* stream live: a retained backlog
                // would replay stale audio after a long pause. The sender's
                // timeline runs on silence, so unpause resumes seamlessly.
                // (This relies on the STAT heartbeat carrying the real byte
                // count; a zeroed reply makes LMS close the stream.)
                auto rr = reader_.read(std::span{buf}, 20);
                if (rr.result == HttpStreamReader::ReadResult::Data && rr.bytes > 0) {
                    if (!feedStream(st, std::as_bytes(std::span{buf}).first(rr.bytes), fmt, nullptr,
                                    /*toOutput=*/false))
                        break;
                } else if (rr.result == HttpStreamReader::ReadResult::Closed) {
                    log::warn("stream socket error while paused; ending stream");
                    break;
                } else if (rr.result == HttpStreamReader::ReadResult::AtEof) {
                    break;
                }
                continue;
            }

            uint64_t iterStart = nowMs();
            auto rr = reader_.read(std::span{buf}, 150);

            if (rr.result == HttpStreamReader::ReadResult::Data && rr.bytes > 0) {
                const auto audio = std::as_bytes(std::span{buf}).first(rr.bytes);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    receivedBytes_ += rr.bytes;
                }
                if (!feedStream(st, audio, fmt, sink.get())) break;
                if (!streaming) {
                    size_t avail = 0;
                    {
                        std::lock_guard<std::mutex> lock(targetMutex_);
                        avail = raop_ ? raop_->availableRead() : 0;
                    }
                    if (avail >= prebufferSamples) {
                        log::info("[ap] prebuffered {} samples in {} ms; launching", avail,
                                  nowMs() - prebufferStartMs);
                        streaming = true;
                        launchAirplaySession();
                    }
                }
                // The pacing clock must include the feed cost, not just the
                // read: pushToRaop/feedRing can block on ring backpressure and
                // an under-counted clock makes the pacer over-sleep relative
                // to real elapsed time (ring dips -> receiver silence pads).
                activeMs += nowMs() - iterStart;
            } else if (rr.result == HttpStreamReader::ReadResult::Timeout) {
                activeMs += nowMs() - iterStart;  // ~= the read timeout
            } else if (rr.result == HttpStreamReader::ReadResult::Closed) {
                // Socket error, not a mere no-data timeout: without this branch
                // the loop used to spin hot on a dead socket forever.
                log::warn("stream socket error; ending stream");
                break;
            }

            if (rr.result == HttpStreamReader::ReadResult::AtEof) {
                if (stage_) {
                    // Decode + emit the remaining tail frames of the stream
                    // (MP3); PCM's finish() is the base no-op.
                    stage_->finish();
                    feedStream(st, {}, fmt, sink.get());
                }
                reachedEof = true;
                break;
            }

            // Pacing must stand down while the rate stage regulates: the
            // decoder intentionally emits ahead of the source (stretching), so
            // an emitted-timeline pacer would throttle the reads, starve the
            // measurement, and spiral the step down. LMS paces the source
            // anyway; without regulation (step 1.0) the pacing keeps the
            // baseline read cadence.
            if (paceRealtime_ && streaming && !(stage_ && stage_->regulating())) {
                uint64_t timeline;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    timeline = fedSamples_ * 1000ULL / fmt.sampleRate;
                }
                // Pace reads to playback time with a lead: keeps the sender's
                // ring fed without running far ahead of the wire. The lead is
                // the pass-through path's only jitter headroom (44.1 kHz PCM
                // goes ring -> RTP packet with no staging, unlike the
                // resampler's 8192-frame inBuf_); too small and any LMS
                // proxy/transcode burst silence-pads RTP packets = crackle.
                // Skipped while prebuffering: the gate wants the ring filled
                // as fast as the source allows.
                if (timeline > activeMs + 60) {
                    uint64_t sleepMs = std::min<uint64_t>(timeline - (activeMs + 60), 120);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                    activeMs += sleepMs;
                }
            }
        }

        // Exit handling. A device-initiated session loss gets ONE transparent
        // retry (the receiver may have been transiently busy). A stop request
        // (new track via strm-s, user stop, shutdown) sends nothing here: the
        // flush/stop handlers already told LMS. A natural end (HTTP EOF with
        // the decoder drained) is reported with STMd — LMS advances the queue
        // on "decoder ready" (`playerReadyToStream`) — then STMu once the
        // sender ring has played out ("normal end of playback"), mirroring
        // squeezelite's STMd-at-DECODE_COMPLETE / STMu-at-output-underrun.
        const bool keepSession = flushed_.exchange(false);
        const bool lost = deviceLost_.exchange(false);
        const bool stopping = st.stop_requested() || !g_run.load();
        log::debug("stream exit: stopping={} lost={} eof={} flushed={}", stopping, lost, reachedEof,
                   keepSession);

        if (!stopping && lost) {
            if (retryUsed_.exchange(true)) {
                log::info("[ap] receiver session lost again; giving up (STMd)");
                client_->sendStat("STMd", currentStats());
            } else {
                log::info("[ap] receiver session lost; retrying in 2s");
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (prepareAirplaySession(fmt.sampleRate)) {
                    if (auto raop = raopSnapshot(); raop && !lastTitle_.empty())
                        raop->setNowPlaying(lastTitle_, "", "");
                    // The HTTP source stays open across a receiver restart, so
                    // loop back into the read phase (with a fresh prebuffer)
                    // instead of tearing down.
                    log::info("[ap] receiver session re-established; resuming stream");
                    continue;
                }
                log::error("[ap] cannot restart airplay session for {}", name_);
                client_->sendStat("STMn", currentStats());
            }
        } else if (!stopping && reachedEof) {
            // Decoder complete: tell LMS we are ready for the next track.
            client_->sendStat("STMd", currentStats());
            // Then let the receiver play out the buffered tail before the
            // output underrun (normal end of playback).
            waitForOutputDrain(st);
            if (!st.stop_requested() && g_run.load() && !deviceLost_.load())
                client_->sendStat("STMu", currentStats());
        } else if (!stopping) {
            // Socket or decode error: the stream is dead, not merely finished.
            client_->sendStat("STMu", currentStats());
        }

        // Tear down and leave. A flush transition (keepSession) leaves the
        // AirPlay session running for the next track.
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (!keepSession && raop_) {
                raop_->discardAudio();
                raop_->stop();
                raop_.reset();
            }
        }
        break;
    }

    if (sink) sink->close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log::info("stream ended, received={} bytes", receivedBytes_);
    }
}

// Play out the sender ring before reporting the end of playback: the decoder
// can complete well before the receiver has consumed the buffered tail, so
// STMu must wait for the ring to empty (bounded, and abortable by a stop
// request from a new track or shutdown). Keeps queuedSamples_ current so the
// final STMu reports the played position.
void PlayerSession::waitForOutputDrain(std::stop_token st) {
    const uint64_t deadline = nowMs() + 5000;
    while (!st.stop_requested() && g_run.load() && !deviceLost_.load()) {
        size_t avail = 0;
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (!raop_) return;
            avail = raop_->availableRead();
            queuedSamples_.store(avail, std::memory_order_relaxed);
        }
        if (avail == 0) return;
        if (nowMs() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// The receiver ended the session on its own (e.g. the phone took over the
// HomePod). Flag it and let the streamLoop's exit path tell LMS (STMd, then
// STMu) instead of streaming into a dead session. Runs on the sender's
// io thread: only flag here — the streamLoop does the discard/stop/reset.
void PlayerSession::onRaopDeviceClosed() {
    // Runs on the sender's io thread: flag only. The streamLoop decides
    // between a transparent session retry and reporting to LMS.
    deviceLost_.store(true);
}

// ICY in-band metadata block (squeezelite parity): forward the raw
// chunk to LMS and push the StreamTitle to the AirPlay receiver.
void PlayerSession::onIcyMeta(std::string_view block) {
    client_->sendMeta(block);
    constexpr std::string_view key = "StreamTitle='";
    const auto p = block.find(key);
    if (p == std::string_view::npos) return;
    const auto e = block.find('\'', p + key.size());
    if (e == std::string_view::npos) return;
    const std::string title(block.substr(p + key.size(), e - p - key.size()));
    if (title.empty()) return;
    // Some stations repeat the identical block every meta interval (~5/s);
    // only log and push on an actual change.
    if (title == lastTitle_) return;
    log::info("icy title: {}", title);
    lastTitle_ = title;
    if (auto raop = raopSnapshot()) raop->setNowPlaying(title, "", "");
}

// One pipeline for every stream format: bytes go through the stream's
// Decoder (mp3 decode / pcm header-skip + s16 stereo normalization) and are
// drained in the same 1152-frame chunks. Returns false when the decoder
// failed and the stream must abort.
bool PlayerSession::feedStream(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
                               PcmFileSink* sink, bool toOutput) {
    if (!stage_) return true;
    // Hold one reference for the whole call: teardown may reset raop_ on
    // another thread, but the snapshot keeps this player alive and any audio
    // pushed to a torn-down player is harmless (its ring is discarded).
    const std::shared_ptr<RaopPlayer> raop = raopSnapshot();
    stage_->feed(data);
    for (;;) {
        const std::span<const int16_t> chunk = stage_->nextChunk();
        if (chunk.empty()) {
            if (stage_->hasError()) {
                log::error("{} decode failed; dropping stream", stage_->name());
                client_->sendStat("STMn", currentStats());
                return false;
            }
            break;
        }
        const PcmFormat norm = stage_->format();
        if (norm.sampleRate != 0 && fmt != norm) {
            fmt = norm;
            elapsedRate_.store(fmt.sampleRate, std::memory_order_relaxed);
            if (raop) raop->setInputRate(fmt.sampleRate);
            log::info("[ap] {} audio: {} Hz, {} ch", stage_->name(), fmt.sampleRate, fmt.channels);
        }
        if (!toOutput) continue;  // paused drain: decode, discard
        if (sink) sink->feed(std::as_bytes(chunk), fmt);
        if (raop) {
            if (fmt.channels == 1) {
                // Duplicate each sample in place, walking backwards so the
                // unread lower-index samples are never overwritten.
                const size_t frames = chunk.size();
                pushScratch_.resize(frames * 2);
                for (size_t i = frames; i-- > 0;) {
                    const int16_t s = chunk[i];
                    pushScratch_[2 * i] = s;
                    pushScratch_[2 * i + 1] = s;
                }
                feedRing(*raop, st, pushScratch_);
            } else {
                feedRing(*raop, st, chunk);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fedSamples_ += chunk.size() / (fmt.channels ? fmt.channels : 2);
            // fedBytes is the consumed prefix; clamp so a decoder backlog
            // larger than what was received cannot underflow the counter.
            const uint64_t pending = stage_->pendingBytes();
            fedBytes_ = receivedBytes_ > pending ? receivedBytes_ - pending : 0;
        }
    }
    return true;
}

// One occupancy sample per stream-loop iteration while streaming. Gaps
// between samples are bounded by the read timeout + pacing sleep (~270 ms),
// so sub-iteration zero-crossings can be missed — the min/max summary still
// shows the trend and the warn/recover pair catches real starvation.
void PlayerSession::sampleRingTelemetry() {
    size_t avail = 0;
    {
        std::lock_guard<std::mutex> lock(targetMutex_);
        if (!raop_) return;
        avail = raop_->availableRead();
        queuedSamples_.store(avail, std::memory_order_relaxed);
    }

    if (stage_) stage_->observeOutput(avail, receivedBytes_, nowMs());
}

void PlayerSession::feedRing(RaopPlayer& raop, std::stop_token st,
                             std::span<const int16_t> samples) {
    size_t offset = 0;
    while (offset < samples.size() && !st.stop_requested() && g_run.load() && !deviceLost_.load()) {
        size_t freeSpace = raop.availableWrite();
        if (freeSpace == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        size_t take = std::min(freeSpace, samples.size() - offset);
        if (!raop.push(std::span<const int16_t>(samples.data() + offset, take))) {
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
        // Unblock a read() parked in poll()/recv() before joining, so a stalled
        // source cannot hold the join (and thus shutdown) for the read timeout.
        // The descriptor stays valid: close() runs only after the join below.
        reader_.interrupt();
        streamThread_.join();
    }
    reader_.close();
}

void PlayerSession::teardownReceiverAudio(bool fullStop) {
    if (!raop_) return;
    raop_->flush();
    raop_->discardAudio();
    if (fullStop) {
        raop_->stop();
        raop_.reset();
    }
}

}  // namespace squeeze2raop2
