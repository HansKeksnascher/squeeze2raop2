#include "playback/player_session.h"

#include "common/log.h"
#include "common/net_util.h"
#include "app/shutdown_flag.h"
#include "common/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace squeeze2raop2 {

PlayerSession::PlayerSession(std::string deviceId, std::string name, std::array<uint8_t, 6> mac,
                             std::optional<std::string> lmsHost, uint16_t lmsPort,
                             bool paceRealtime, std::optional<std::string> sinkPath,
                             std::optional<RaopTarget> raopTarget,
                             CredentialSink credSink, VolumeMode volumeMode,
                             VolumeAnchors anchors, float volPct, int latencyMs)
    : deviceId_(std::move(deviceId)),
      name_(std::move(name)),
      mac_(mac),
      lmsHost_(std::move(lmsHost)),
      lmsPort_(lmsPort),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)),
      anchors_(std::move(anchors)),
      volumeMode_(volumeMode),
      fixedVolumePct_(volPct) {
    // Compute the receiver identity before any thread exists (the sender reads
    // it): uppercase hex MAC with the separators removed.
    std::string identity = macToString(mac_);
    std::ranges::transform(identity, identity.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::erase(identity, ':');
    output_ = std::make_unique<AirplayOutput>(name_, std::move(identity), std::move(raopTarget),
                                              std::move(credSink), latencyMs);
}

PlayerSession::~PlayerSession() { stop(); }

void PlayerSession::start() {
    SlimProtoClient::Events events;
    events.onStart = [this](const StrmStart& st) { startStream(st); };
    events.onCont = [this](uint32_t) {
        if (!autostartPending_.exchange(false)) return;
        client_->sendStat("STMs", currentStats());
        streamThread_ = std::jthread([this](std::stop_token st) { streamLoop(st); });
    };
    events.onStop = [this]() {
        stopPlayback();
        output_->stop(true);
        client_->sendStat("STMf", currentStats());
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
        log::debug("pause {}", ms);
        // STMp is sent by the slimproto 'p' handler (squeezelite parity);
        // sending it here too duplicates the event.
    };
    events.onUnpause = [this](uint32_t) {
        if (track_) track_->unpause();
    };
    events.onVolume = [this](double l, double r) {
        // The recovered LMS slider percent goes through the --vol-map dB
        // anchors before it reaches the AirPlay sender's 0..100 % domain
        // (0 % = -144 mute sentinel, 100 % = 0 dB).
        double pct = (l == r) ? r : (l + r) / 2.0;
        if (volumeMode_ == VolumeMode::Fixed) {
            log::info("volume l={:.0f} r={:.0f} -> {} (ignored, fixed at {})", l, r, pct,
                      fixedVolumePct_);
            return;
        }
        pct = anchors_.airplayPctFromLms(pct);
        // Remember the slider, not mute pushes: LMS's stop-fade ends at gain 0
        // and fresh players get a 0-gain push on registration, so a stored 0
        // would mute the next session until the first AUDG.
        if (pct > 0.0) lastLmsPct_.store(pct, std::memory_order_relaxed);
        if (output_->setVolume(pct)) {
            log::info("[ap] volume {:.1f} pct applied (lms)", pct);
        } else {
            log::info("volume -> {:.1f} pct ({})", pct,
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
            "Firmware=squeeze2raop2 v0.1.0 (m1),pcm,mp3",
        std::move(events));
    client_->setPlayerName(name_);
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
    log::info("[ap] volume {:.1f} pct applied (post-start)", pct);
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

StreamStats PlayerSession::currentStats() { return counters_.stats(); }

void PlayerSession::startStream(const StrmStart& st) {
    stopPlayback();
    flushed_.store(false);
    retryUsed_.store(false);

    std::string host =
        st.serverIp ? ipv4ToString(st.serverIp) : (client_ ? client_->serverHost() : std::string());
    uint16_t port = st.serverPort ? st.serverPort : 9000;
    if (host.empty() || st.request.empty()) {
        log::error("strm-s missing stream target or request header");
        client_->sendStat("STMn", currentStats());
        return;
    }
    // Only formats the track pump actually consumes. LMS should honor the HELO
    // caps (pcm,mp3); a stray direct format would otherwise be pushed into the
    // ring as raw PCM = noise.
    if (st.format != StreamFormat::Pcm && st.format != StreamFormat::Mp3) {
        log::error("strm s: unsupported stream format '{}'", static_cast<char>(st.format));
        client_->sendStat("STMn", currentStats());
        return;
    }

    log::info("stream GET {}:{} icy={}", host, port,
              st.request.find("Icy-MetaData") != std::string::npos ? "req" : "none");
    track_ = std::make_unique<PlaybackStream>(*output_, counters_, paceRealtime_, sinkPath_);
    track_->setMetaForward([this](std::string_view block) { client_->sendMeta(block); });

    std::string error;
    const std::optional<std::string> headers = track_->open(st, host, port, error);
    if (!headers) {
        log::error("stream connect {}:{} failed: {}", host, port, error);
        client_->sendStat("STMn", currentStats());
        return;
    }
    client_->sendResp(*headers);
    client_->sendStat("STMc", currentStats());

    if (st.autostart >= 2) {
        autostartPending_.store(true);
        log::info("autostart={}, waiting for cont", st.autostart);
        return;
    }
    client_->sendStat("STMs", currentStats());
    streamThread_ = std::jthread([this](std::stop_token stopTok) { streamLoop(stopTok); });
}

void PlayerSession::streamLoop(std::stop_token st) {
    if (!track_) return;
    const PcmFormat fmt = track_->format();
    const bool haveTarget = output_->hasTarget();
    if (haveTarget) {
        if (!output_->prepare(fmt.sampleRate)) {
            log::error("[ap] cannot start airplay session for {}", name_);
            client_->sendStat("STMn", currentStats());
            return;
        }
        // (input rate is applied inside prepare(); the only later setInputRate
        // is the track's mid-stream format-adoption update)
        if (fmt.channels != 2)
            log::warn("input is {}-channel; bridges Apple receivers expect stereo", fmt.channels);
    }

    // Streaming phase: re-entered after a single transparent receiver-loss
    // retry. Each pass re-arms the prebuffer gate because a retry creates a
    // fresh ring.
    for (;;) {
        const PlaybackStream::End end = track_->run(st, [this] { launchAirplaySession(); });
        if (end == PlaybackStream::End::DecodeError) client_->sendStat("STMn", currentStats());

        // Exit handling, decided by the pure decideExit() policy: a stop
        // request sends nothing (the flush/stop handlers already told LMS); a
        // receiver loss gets one transparent retry; a natural end (HTTP EOF
        // with the decoder drained) reports STMd — LMS advances the queue on
        // "decoder ready" — then STMu once the sender ring has played out.
        const bool keepSession = flushed_.exchange(false);
        const bool lost = output_->consumeLost();
        const bool stopping = st.stop_requested() || !g_run.load();
        const bool reachedEof = end == PlaybackStream::End::Eof;
        const ExitAction action = decideExit({stopping, lost, retryUsed_.load(), reachedEof});
        log::debug("stream exit: action={} stopping={} lost={} eof={} flushed={}",
                   static_cast<int>(action), stopping, lost, reachedEof, keepSession);

        switch (action) {
        case ExitAction::SilentStop:
            break;
        case ExitAction::Retry:
            retryUsed_.store(true);
            log::info("[ap] receiver session lost; retrying in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (output_->prepare(track_->format().sampleRate)) {
                track_->reapplyNowPlaying();
                // The HTTP source stays open across a receiver restart, so loop
                // back into the read phase (with a fresh prebuffer).
                log::info("[ap] receiver session re-established; resuming stream");
                continue;
            }
            log::error("[ap] cannot restart airplay session for {}", name_);
            client_->sendStat("STMn", currentStats());
            break;
        case ExitAction::GaveUp:
            log::info("[ap] receiver session lost again; giving up (STMd)");
            client_->sendStat("STMd", currentStats());
            break;
        case ExitAction::EndedEof:
            // Decoder complete: tell LMS we are ready for the next track.
            client_->sendStat("STMd", currentStats());
            // Then let the receiver play out the buffered tail before the
            // output underrun (normal end of playback).
            waitForOutputDrain(st);
            if (!st.stop_requested() && g_run.load() && !output_->lost())
                client_->sendStat("STMu", currentStats());
            break;
        case ExitAction::EndedError:
            // Socket or decode error: the stream is dead, not merely finished.
            client_->sendStat("STMu", currentStats());
            break;
        }

        // Tear down and leave. A flush transition (keepSession) leaves the
        // AirPlay session running for the next track.
        if (!keepSession) output_->stop(false);
        break;
    }

    track_->close();
    log::info("stream ended, received={} bytes", counters_.bytesReceived());
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
        autostartPending_.store(false);
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
