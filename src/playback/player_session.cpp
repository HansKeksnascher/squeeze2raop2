#include "playback/player_session.h"

#include "app/shutdown_flag.h"
#include "app/version.h"
#include "common/log.h"
#include "common/net_util.h"
#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace squeeze2raop2 {

PlayerSession::PlayerSession(const ResolvedPlayerConfig& cfg, const GlobalConfig& global,
                             VolumeAnchors anchors, std::optional<std::string> sinkPath,
                             std::optional<RaopTarget> raopTarget, CredentialSink credSink,
                             NameSink nameSink)
    : name_(cfg.name),
      mac_(cfg.mac),
      lmsHost_(global.lmsHost),
      lmsPort_(global.lmsPort),
      paceRealtime_(cfg.paceRealtime),
      sinkPath_(std::move(sinkPath)),
      serverTimeoutMs_(global.serverTimeoutMs),
      nameSink_(std::move(nameSink)),
      anchors_(std::move(anchors)),
      volumeMode_(cfg.volumeMode),
      fixedVolumePct_(cfg.volPct) {
    // Compute the receiver identity before any thread exists (the sender reads
    // it): uppercase hex MAC with the separators removed.
    std::string identity = macToString(mac_);
    std::ranges::transform(identity, identity.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::erase(identity, ':');
    output_ = std::make_unique<AirplayOutput>(name_, std::move(identity), std::move(raopTarget),
                                              std::move(credSink), cfg.latencyMs);
}

PlayerSession::~PlayerSession() { stop(); }

void PlayerSession::start() {
    SlimProtoClient::Events events;
    events.onStart = [this](const StrmStart& st) { startStream(st); };
    events.onCont = [this](uint32_t) {
        // autostart >= 2 waits for cont before the pump starts (squeezelite
        // parity); the pump's first decoded chunk then drives STMl/STMs.
        haveCont_.store(true);
        maybeStartPump();
    };
    events.onStop = [this]() {
        client_->sendStat("STMf", currentStats());
        // A configured fade-out is rendered by the pump; streamLoop tears the
        // session down when it returns FadedOut.
        if (streamActive_.load() && track_ && track_->requestFadeOut()) {
            log::info(log::Area::Ap, "stop: fading out before teardown");
            return;
        }
        stopPlayback();
        output_->stop(true);
    };
    events.onFlush = [this](bool) {
        // Track transitions send 'strm f': drop buffered audio but KEEP the
        // AirPlay session — the next track's audio keeps streaming through it
        // (no ~1 s re-pairing gap). The streamLoop's exit path honors flushed_
        // by skipping the session teardown.
        flushed_.store(true);
        stopPlayback();
        output_->silence();
        client_->sendStat("STMf", currentStats());
    };
    events.onPause = [this](uint32_t ms) {
        // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a timed pause
        // (transition gaps). LMS's stop for a remote stream is a fade-down
        // followed by 'p 0'. The deadline lives in the track's pump loop.
        if (track_) track_->pause(ms);
        output_->silence();
        log::debug(log::Area::Ses, "pause requested: interval={} ms", ms);
        // STMp is sent by the slimproto 'p' handler (squeezelite parity);
        // sending it here too duplicates the event.
    };
    events.onUnpause = [this](uint32_t) {
        if (track_) track_->unpause();
    };
    events.onSkipAhead = [this](uint32_t ms) {
        // squeezelite drops the skipped frames from the output buffer; the
        // bridge drops them from the decoded stream after flushing the ring.
        if (!track_) return;
        output_->silence();
        track_->skipAhead(ms);
    };
    events.onCodc = [this](StreamFormat format, const PcmParams& pcm) {
        if (!awaitCodc_.load() || !track_) return;
        haveCodc_.store(true);
        std::string error;
        if (!track_->attachDecoder(format, pcm, error)) {
            log::error(log::Area::Ses, "codc: unsupported codec '{}': {}",
                       static_cast<char>(format), error);
            client_->sendStat("STMn", currentStats());
            track_.reset();
            awaitCodc_.store(false);
            return;
        }
        awaitCodc_.store(false);  // a duplicate codc is ignored
        maybeStartPump();
    };
    events.onAude = [this](bool enable) {
        if (enable) return;  // lazy session recreation covers power-on
        log::info(log::Area::Ap, "aude power off; tearing down the session");
        stopPlayback();
        output_->stop(true);
    };
    events.onSetName = [this](const std::string& name) {
        name_ = name;
        client_->setPlayerName(name);
        if (nameSink_) nameSink_(name);
    };
    events.onVolume = [this](double l, double r) {
        // The recovered LMS slider percent goes through the --vol-map dB
        // anchors before it reaches the AirPlay sender's 0..100 % domain
        // (0 % = -144 mute sentinel, 100 % = 0 dB).
        double pct = (l == r) ? r : (l + r) / 2.0;
        if (volumeMode_ == VolumeMode::Fixed) {
            log::info(log::Area::Ses, "volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})", l, r,
                      pct, fixedVolumePct_);
            return;
        }
        pct = anchors_.airplayPctFromLms(pct);
        // Remember the slider, not mute pushes: LMS's stop-fade ends at gain 0
        // and fresh players get a 0-gain push on registration, so a stored 0
        // would mute the next session until the first AUDG.
        if (pct > 0.0) lastLmsPct_.store(pct, std::memory_order_relaxed);
        if (output_->setVolume(pct)) {
            log::info(log::Area::Ap, "volume {:.1f} pct applied (lms)", pct);
        } else {
            log::info(log::Area::Ses, "volume -> {:.1f} pct ({})", pct,
                      pct > 0.0 ? "remembered for next session" : "mute, not remembered");
        }
    };

    // squeezelite-style caps: Model/ModelName drive the LMS web UI (player
    // lists, settings); the player's display name is sent separately via
    // SETD name. Firmware= is the free-text version LMS shows in player
    // settings. ModelName carries the AirPlay transport in use so the LMS UI
    // distinguishes the classic RAOP/AP1 path from the native AirPlay 2 path.
    const std::string modelName =
        output_->hasTarget() ? (output_->airplay2() ? "squeeze2raop2@ap2" : "squeeze2raop2@raop")
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
            "Firmware=squeeze2raop2 " SQUEEZE2RAOP2_VERSION ",pcm,mp3",
        std::move(events));
    client_->setPlayerName(name_);
    client_->setServerTimeout(serverTimeoutMs_);
    client_->setStatsProvider([this] { return currentStats(); });
    client_->start(lmsHost_.value_or(""), lmsPort_);
    // the AirPlay session is prepared lazily when the first audio arrives;
    // connecting eagerly hits receivers that immediately drop idle sessions
    // (HomePod/Sonos)
}

// Start the AirPlay sender (fresh session) or leave a live one running.
// Volume must be applied AFTER start(): RaopSender::start() wipes
// pendingVolumeDb_ ("never carry volume between devices"), so a pre-start
// setVolume is lost and startStreaming_ falls back to 0 dB = full blast.
// Post-start it only stores until the handshake finishes; startStreaming_
// sends the stored value before the audio pacer starts. In lms mode the last
// AUDG slider value wins; without one yet the fixed --vol-pct level covers the
// first seconds until LMS pushes the slider.
void PlayerSession::launchAirplaySession() {
    double pct = fixedVolumePct_;
    if (volumeMode_ == VolumeMode::Lms) {
        const double remembered = lastLmsPct_.load(std::memory_order_relaxed);
        if (remembered > 0.0) pct = remembered;
    }
    log::info(log::Area::Ap, "volume {:.1f} pct applied (post-start)", pct);
    output_->launch(pct);
}

void PlayerSession::updateTarget(RaopTarget t) { output_->updateTarget(std::move(t)); }

void PlayerSession::stop() {
    // Join the stream thread first: it reads output_ throughout streamLoop, so
    // the session must outlive it.
    stopPlayback();
    output_->stop(false);
    if (client_) client_->stop();
}

StreamStats PlayerSession::currentStats() {
    StreamStats st = counters_.stats();
    // Real sender-ring occupancy, in bytes, for the STAT output buffer fields
    // (squeezelite reports the output buffer the same way).
    st.outputBufferSize = static_cast<uint32_t>(output_->capacityBytes());
    st.outputBufferFullness = static_cast<uint32_t>(output_->queuedBytes());
    return st;
}

void PlayerSession::startStream(const StrmStart& st) {
    stopPlayback();
    flushed_.store(false);
    retryUsed_.store(false);
    autostart_.store(st.autostart);
    awaitCodc_.store(false);
    haveCodc_.store(false);
    haveCont_.store(false);
    sentStml_ = false;
    sentStms_ = false;

    std::string host =
        st.serverIp ? ipv4ToString(st.serverIp) : (client_ ? client_->serverHost() : std::string());
    uint16_t port = st.serverPort ? st.serverPort : 9000;
    if (host.empty() || st.request.empty()) {
        log::error(log::Area::Ses, "strm-s missing stream target or request header");
        client_->sendStat("STMn", currentStats());
        return;
    }
    // Only formats the track pump actually consumes. LMS should honor the HELO
    // caps (pcm,mp3); a stray direct format would otherwise be pushed into the
    // ring as raw PCM = noise. '?' (unknown) is allowed only with autostart>=2,
    // where LMS learns the codec from the response header and returns it in
    // 'codc' (squeezelite parity).
    const bool unknown = st.format == StreamFormat::Unknown;
    if (!unknown && st.format != StreamFormat::Pcm && st.format != StreamFormat::Mp3) {
        log::error(log::Area::Ses, "strm s: unsupported stream format '{}'",
                   static_cast<char>(st.format));
        client_->sendStat("STMn", currentStats());
        return;
    }
    if (unknown && st.autostart < 2) {
        log::error(log::Area::Ses, "strm s: unknown codec requires autostart >= 2");
        client_->sendStat("STMn", currentStats());
        return;
    }

    log::info(log::Area::Ses, "stream GET {}:{} icy={}", host, port,
              st.request.find("Icy-MetaData") != std::string::npos ? "req" : "none");
    track_ = std::make_unique<PlaybackStream>(*output_, counters_, paceRealtime_, sinkPath_);
    track_->setMetaForward([this](std::string_view block) { client_->sendMeta(block); });
    track_->setDecoderReady([this] { onDecoderReady(); });
    track_->setUnderrun([this] { client_->sendStat("STMo", currentStats()); });

    std::string error;
    const std::optional<std::string> headers = track_->openSource(st, host, port, error);
    if (!headers) {
        log::error(log::Area::Ses, "stream connect {}:{} failed: {}", host, port, error);
        client_->sendDisco(static_cast<uint8_t>(track_->disconnectCode()));
        client_->sendStat("STMn", currentStats());
        track_.reset();
        return;
    }
    client_->sendResp(*headers);
    client_->sendStat("STMc", currentStats());

    if (unknown) {
        awaitCodc_.store(true);
        log::info(log::Area::Ses, "autostart={}, unknown codec: waiting for codc", st.autostart);
        return;  // maybeStartPump() runs from the codc handler
    }
    if (!track_->attachDecoder(st.format, st.pcm, error)) {
        log::error(log::Area::Ses, "strm s: cannot create decoder: {}", error);
        client_->sendStat("STMn", currentStats());
        track_.reset();
        return;
    }
    maybeStartPump();
}

// Start the pump once the codec is known and, for autostart >= 2, 'cont' has
// arrived. Safe to call repeatedly; a running thread short-circuits.
void PlayerSession::maybeStartPump() {
    if (!track_) return;
    if (streamThread_.joinable()) return;
    if (awaitCodc_.load() && !haveCodc_.load()) return;
    if (autostart_.load() >= 2 && !haveCont_.load()) return;
    streamThread_ = std::jthread([this](std::stop_token st) { streamLoop(st); });
}

// First decoded chunk: announce decoder readiness (STMl for autostart 0) and,
// with no AirPlay target, the track start (STMs) since the prebuffer gate is
// disabled.
void PlayerSession::onDecoderReady() {
    if (autostart_.load() == 0 && !sentStml_) {
        sentStml_ = true;
        client_->sendStat("STMl", currentStats());
    }
    if (!output_->hasTarget() && !sentStms_) {
        sentStms_ = true;
        client_->sendStat("STMs", currentStats());
    }
}

// The prebuffer gate opened: the receiver output is about to start (STMs) and
// the sender is launched with the current volume.
void PlayerSession::onPrebufferReady() {
    if (!sentStms_) {
        sentStms_ = true;
        client_->sendStat("STMs", currentStats());
    }
    launchAirplaySession();
}

void PlayerSession::streamLoop(std::stop_token st) {
    streamActive_.store(true);
    if (!track_) {
        streamActive_.store(false);
        return;
    }
    const PcmFormat fmt = track_->format();
    const bool haveTarget = output_->hasTarget();
    if (haveTarget) {
        if (!output_->prepare(fmt.sampleRate)) {
            log::error(log::Area::Ap, "cannot start airplay session for {}", name_);
            client_->sendStat("STMn", currentStats());
            streamActive_.store(false);
            return;
        }
        // (input rate is applied inside prepare(); the only later setInputRate
        // is the track's mid-stream format-adoption update)
        if (fmt.channels != 2)
            log::warn(log::Area::Ses, "input is {}-channel; bridges Apple receivers expect stereo",
                      fmt.channels);
    }

    // Streaming phase: re-entered after a single transparent receiver-loss
    // retry. Each pass re-arms the prebuffer gate because a retry creates a
    // fresh ring.
    for (;;) {
        const PlaybackStream::End end = track_->run(st, [this] { onPrebufferReady(); });
        if (end == PlaybackStream::End::DecodeError) client_->sendStat("STMn", currentStats());

        const bool keepSession = flushed_.exchange(false);
        const bool lost = output_->consumeLost();
        const bool stopping = st.stop_requested() || !g_run.load();
        const bool reachedEof = end == PlaybackStream::End::Eof;

        // A stop-path fade already reached zero: the 'q' handler sent STMf.
        if (end == PlaybackStream::End::FadedOut) {
            log::debug(log::Area::Ses, "stream exit: fade-out complete");
            if (!keepSession) output_->stop(false);
            break;
        }

        // Exit handling, decided by the pure decideExit() policy: a stop
        // request sends nothing (the flush/stop handlers already told LMS); a
        // receiver loss gets one transparent retry; a natural end (HTTP EOF
        // with the decoder drained) reports STMd — LMS advances the queue on
        // "decoder ready" — then STMu once the sender ring has played out.
        const ExitAction action = decideExit({stopping, lost, retryUsed_.load(), reachedEof});
        log::debug(log::Area::Ses, "stream exit: action={} stopping={} lost={} eof={} flushed={}",
                   static_cast<int>(action), stopping, lost, reachedEof, keepSession);

        switch (action) {
        case ExitAction::SilentStop: break;
        case ExitAction::Retry:
            retryUsed_.store(true);
            log::info(log::Area::Ap, "receiver session lost; retrying in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (output_->prepare(track_->format().sampleRate)) {
                track_->reapplyNowPlaying();
                // The HTTP source stays open across a receiver restart, so loop
                // back into the read phase (with a fresh prebuffer).
                log::info(log::Area::Ap, "receiver session re-established; resuming stream");
                continue;
            }
            log::error(log::Area::Ap, "cannot restart airplay session for {}", name_);
            client_->sendStat("STMn", currentStats());
            break;
        case ExitAction::GaveUp:
            log::info(log::Area::Ap, "receiver session lost again; giving up (STMd)");
            client_->sendStat("STMd", currentStats());
            break;
        case ExitAction::EndedEof:
            // Decoder complete: DSCO(OK) closes the stream, then STMd tells LMS
            // we are ready for the next track.
            client_->sendDisco(static_cast<uint8_t>(track_->disconnectCode()));
            client_->sendStat("STMd", currentStats());
            // Then let the receiver play out the buffered tail before the
            // output underrun (normal end of playback).
            waitForOutputDrain(st);
            if (!st.stop_requested() && g_run.load() && !output_->lost())
                client_->sendStat("STMu", currentStats());
            break;
        case ExitAction::EndedError:
            // Socket or decode error: the stream is dead, not merely finished.
            // A source-side failure carries a DSCO reason; a decode error does
            // not (its code is None).
            if (track_->disconnectCode() != DisconnectCode::None)
                client_->sendDisco(static_cast<uint8_t>(track_->disconnectCode()));
            client_->sendStat("STMu", currentStats());
            break;
        }

        // Tear down and leave. A flush transition (keepSession) leaves the
        // AirPlay session running for the next track.
        if (!keepSession) output_->stop(false);
        break;
    }

    track_->close();
    streamActive_.store(false);
    log::info(log::Area::Ses, "stream ended, received={} bytes", counters_.bytesReceived());
}

// Play out the sender ring before reporting the end of playback: the decoder
// can complete well before the receiver has consumed the buffered tail, so
// STMu must wait for the ring to empty (bounded, and abortable by a stop
// request from a new track or shutdown). Keeps the ring occupancy current so
// the final STMu reports the played position.
void PlayerSession::waitForOutputDrain(std::stop_token st) {
    const uint64_t deadline = nowMs() + 5000;
    while (!st.stop_requested() && g_run.load() && !output_->lost()) {
        const size_t avail = output_->queued();
        counters_.setQueued(avail);
        if (avail == 0) return;
        if (nowMs() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void PlayerSession::stopPlayback() {
    if (streamThread_.joinable()) {
        streamThread_.request_stop();
        if (track_) {
            track_->unpause();
            // Unblock a read() parked in poll()/recv() before joining, so a
            // stalled source cannot hold the join (and thus shutdown) for the
            // read timeout. The descriptor stays valid: close() runs after the
            // join below.
            track_->interrupt();
        }
        streamThread_.join();
    }
    if (track_) track_->close();
}

}  // namespace squeeze2raop2