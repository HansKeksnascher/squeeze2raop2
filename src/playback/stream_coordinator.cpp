#include "playback/stream_coordinator.h"

#include "app/shutdown_flag.h"
#include "common/log.h"
#include "common/net_util.h"
#include "common/util.h"
#include "lms/lms_stream.h"
#include "playback/exit_policy.h"

#include <chrono>
#include <thread>
#include <utility>

namespace squeeze2raop2 {

StreamCoordinator::StreamCoordinator(AirplayOutput& output, StreamCounters& counters,
                                     SlimProtoSession& link, VolumeController& volume,
                                     uint32_t sourceTimeoutMs, bool paceRealtime,
                                     std::optional<std::string> sinkPath)
    : output_(output),
      counters_(counters),
      link_(link),
      volume_(volume),
      sourceTimeoutMs_(sourceTimeoutMs),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)) {}

StreamCoordinator::~StreamCoordinator() { shutdown(); }

StreamStats StreamCoordinator::currentStats() {
    StreamStats st = counters_.stats();
    // Real sender-ring occupancy, in bytes, for the STAT output buffer fields
    // (squeezelite reports the output buffer the same way).
    st.outputBufferSize = static_cast<uint32_t>(output_.capacityBytes());
    st.outputBufferFullness = static_cast<uint32_t>(output_.queuedBytes());
    return st;
}

void StreamCoordinator::onStreamStart(const StrmStart& st) {
    shutdown();
    flushed_.store(false);
    retryUsed_.store(false);
    autostart_.store(st.autostart);
    awaitCodc_.store(false);
    haveCodc_.store(false);
    haveCont_.store(false);
    sentStml_ = false;
    sentStms_ = false;

    std::string host = st.serverIp ? ipv4ToString(st.serverIp) : link_.serverHost();
    uint16_t port = st.serverPort ? st.serverPort : kDefaultStreamPort;
    if (host.empty() || st.request.empty()) {
        log::error(log::Area::Ses, "strm-s missing stream target or request header");
        link_.stat(kStatError);
        return;
    }
    // Only formats the track pump actually consumes, per Decoder::supportedCodecs()
    // (the same list the HELO caps advertise). LMS should honor those caps; a
    // stray direct format would otherwise be pushed into the ring as raw PCM =
    // noise. '?' (unknown) is allowed only with autostart>=2, where LMS learns
    // the codec from the response header and returns it in 'codc' (squeezelite
    // parity).
    const bool unknown = st.format == StreamFormat::Unknown;
    if (!unknown && !supportsFormat(st.format)) {
        log::error(log::Area::Ses, "strm s: unsupported stream format '{}'",
                   static_cast<char>(st.format));
        link_.stat(kStatError);
        return;
    }
    if (unknown && st.autostart < kAutostartRequireCont) {
        log::error(log::Area::Ses, "strm s: unknown codec requires autostart >= 2");
        link_.stat(kStatError);
        return;
    }

    // The request's Host header is the station hostname on a direct stream and
    // absent on an LMS-proxied one; log it when present so the direct URL is
    // readable next to the (DNS-resolved) IP LMS handed us.
    const std::string hostName = requestHost(st.request);
    const char* icy = st.request.find("Icy-MetaData") != std::string::npos ? "req" : "none";
    if (hostName.empty())
        log::info(log::Area::Ses, "stream GET {}:{} icy={}", host, port, icy);
    else
        log::info(log::Area::Ses, "stream GET {}:{} host={} icy={}", host, port, hostName, icy);
    track_ = std::make_unique<PlaybackStream>(output_, counters_, paceRealtime_, sinkPath_,
                                              sourceTimeoutMs_);
    track_->setMetaForward([this](std::string_view block) { link_.meta(block); });
    track_->setDecoderReady([this] { onDecoderReady(); });
    track_->setUnderrun([this] { link_.stat(kStatUnderrun); });

    std::string error;
    const std::optional<std::string> headers = track_->openSource(st, host, port, error);
    if (!headers) {
        log::error(log::Area::Ses, "stream connect {}:{} failed: {}", host, port, error);
        link_.disco(track_->disconnectCode());
        link_.stat(kStatError);
        track_.reset();
        return;
    }
    link_.resp(*headers);
    link_.stat(kStatConnect);

    if (unknown) {
        awaitCodc_.store(true);
        log::info(log::Area::Ses, "autostart={}, unknown codec: waiting for codc", st.autostart);
        return;  // maybeStartPump() runs from the codc handler
    }
    if (!track_->attachDecoder(st.format, st.pcm, error)) {
        log::error(log::Area::Ses, "strm s: cannot create decoder: {}", error);
        link_.stat(kStatError);
        track_.reset();
        return;
    }
    maybeStartPump();
}

void StreamCoordinator::onCont() {
    // autostart >= 2 waits for cont before the pump starts (squeezelite
    // parity); the pump's first decoded chunk then drives STMl/STMs.
    haveCont_.store(true);
    maybeStartPump();
}

void StreamCoordinator::onCodc(StreamFormat format, const PcmParams& pcm) {
    if (!awaitCodc_.load() || !track_) return;
    haveCodc_.store(true);
    std::string error;
    if (!track_->attachDecoder(format, pcm, error)) {
        log::error(log::Area::Ses, "codc: unsupported codec '{}': {}", static_cast<char>(format),
                   error);
        link_.stat(kStatError);
        track_.reset();
        awaitCodc_.store(false);
        return;
    }
    awaitCodc_.store(false);  // a duplicate codc is ignored
    maybeStartPump();
}

void StreamCoordinator::flush() {
    // Track transitions send 'strm f': drop buffered audio but KEEP the
    // AirPlay session — the next track's audio keeps streaming through it
    // (no ~1 s re-pairing gap). The streamLoop's exit path honors flushed_
    // by skipping the session teardown.
    flushed_.store(true);
    shutdown();
    output_.silence();
}

void StreamCoordinator::pause(uint32_t ms) {
    // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a timed pause
    // (transition gaps). LMS's stop for a remote stream is a fade-down
    // followed by 'p 0'. The deadline lives in the track's pump loop.
    if (track_) track_->pause(ms);
    output_.silence();
    log::debug(log::Area::Ses, "pause requested: interval={} ms", ms);
    // STMp is sent by the slimproto 'p' handler (squeezelite parity);
    // sending it here too duplicates the event.
}

void StreamCoordinator::unpause(uint32_t) {
    if (track_) track_->unpause();
}

void StreamCoordinator::skipAhead(uint32_t ms) {
    // squeezelite drops the skipped frames from the output buffer; the bridge
    // drops them from the decoded stream after flushing the ring.
    if (!track_) return;
    output_.silence();
    track_->skipAhead(ms);
}

void StreamCoordinator::requestStopOrFade() {
    // A configured fade-out is rendered by the pump; streamLoop tears the
    // session down when it returns FadedOut.
    if (streamActive_.load() && track_ && track_->requestFadeOut()) {
        log::info(log::Area::Ap, "stop: fading out before teardown");
        return;
    }
    shutdown();
    output_.stop(true);
}

void StreamCoordinator::audeOff() {
    log::info(log::Area::Ap, "aude power off; tearing down the session");
    shutdown();
    output_.stop(true);
}

// Start the pump once the codec is known and, for autostart >= 2, 'cont' has
// arrived. Safe to call repeatedly; a running thread short-circuits.
void StreamCoordinator::maybeStartPump() {
    if (!track_) return;
    if (streamThread_.joinable()) return;
    if (awaitCodc_.load() && !haveCodc_.load()) return;
    if (autostart_.load() >= kAutostartRequireCont && !haveCont_.load()) return;
    streamThread_ = std::jthread([this](std::stop_token st) { streamLoop(st); });
}

// First decoded chunk: announce decoder readiness (STMl for autostart 0) and,
// with no AirPlay target, the track start (STMs) since the prebuffer gate is
// disabled.
void StreamCoordinator::onDecoderReady() {
    if (autostart_.load() == kAutostartDecoderReady && !sentStml_) {
        sentStml_ = true;
        link_.stat(kStatAutostart);
    }
    if (!output_.hasTarget() && !sentStms_) {
        sentStms_ = true;
        link_.stat(kStatStart);
    }
}

// The prebuffer gate opened: the receiver output is about to start (STMs) and
// the sender is launched with the current volume.
void StreamCoordinator::onPrebufferReady() {
    if (!sentStms_) {
        sentStms_ = true;
        link_.stat(kStatStart);
    }
    volume_.launch();
}

void StreamCoordinator::streamLoop(std::stop_token st) {
    streamActive_.store(true);
    if (!track_) {
        streamActive_.store(false);
        return;
    }
    // A stop request now wakes a read() parked in poll()/recv() by itself;
    // before, every teardown path had to remember to call track_->interrupt()
    // before joining. The callback lives for this stream thread only and
    // captures the track directly (track_ may be replaced by onStreamStart
    // after this thread is joined).
    PlaybackStream* track = track_.get();
    std::stop_callback wakeOnStop(st, [track] { track->interrupt(); });

    // A previous track may have left the session parked (keepSession) with the
    // coarse keep-alive driver running; stop it before this stream thread
    // takes over the sender pump.
    output_.unpark();

    const PcmFormat fmt = track_->format();
    const bool haveTarget = output_.hasTarget();
    if (haveTarget) {
        if (!output_.prepare(fmt.sampleRate)) {
            log::error(log::Area::Ap, "cannot start airplay session for {}", link_.name());
            link_.stat(kStatError);
            streamActive_.store(false);
            return;
        }
        // (input rate is applied inside prepare(); the only later setInputRate
        // is the track's mid-stream format-adoption update)
        if (fmt.channels != kDefaultChannels)
            log::warn(log::Area::Ses, "input is {}-channel; bridges Apple receivers expect stereo",
                      fmt.channels);
    }

    // Streaming phase: re-entered after a single transparent receiver-loss
    // retry. Each pass re-arms the prebuffer gate because a retry creates a
    // fresh ring.
    for (;;) {
        const PlaybackStream::End end = track_->run(st, [this] { onPrebufferReady(); });
        if (end == PlaybackStream::End::DecodeError) link_.stat(kStatError);

        const bool keepSession = flushed_.exchange(false);
        const bool lost = output_.consumeLost();
        const bool stopping = st.stop_requested() || !g_run.load();
        const bool reachedEof = end == PlaybackStream::End::Eof;

        // A stop-path fade already reached zero: the 'q' handler sent STMf.
        if (end == PlaybackStream::End::FadedOut) {
            log::debug(log::Area::Ses, "stream exit: fade-out complete");
            if (keepSession)
                output_.park();
            else
                output_.stop(false);
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
            log::info(log::Area::Ap, "receiver session lost; retrying in {}s",
                      kRetryDelayMs / kMsPerSecond);
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
            if (output_.prepare(track_->format().sampleRate)) {
                track_->reapplyNowPlaying();
                // The HTTP source stays open across a receiver restart, so loop
                // back into the read phase (with a fresh prebuffer).
                log::info(log::Area::Ap, "receiver session re-established; resuming stream");
                continue;
            }
            log::error(log::Area::Ap, "cannot restart airplay session for {}", link_.name());
            link_.stat(kStatError);
            break;
        case ExitAction::GaveUp:
            log::info(log::Area::Ap, "receiver session lost again; giving up (STMd)");
            link_.stat(kStatDone);
            break;
        case ExitAction::EndedEof:
            // Decoder complete: DSCO(OK) closes the stream, then STMd tells LMS
            // we are ready for the next track.
            link_.disco(track_->disconnectCode());
            link_.stat(kStatDone);
            // Then let the receiver play out the buffered tail before the
            // output underrun (normal end of playback).
            waitForOutputDrain(st);
            if (!st.stop_requested() && g_run.load() && !output_.lost()) link_.stat(kStatEnd);
            break;
        case ExitAction::EndedError:
            // Socket or decode error: the stream is dead, not merely finished.
            // A source-side failure carries a DSCO reason; report it as STMn
            // (an error) rather than STMu (a normal end), so LMS treats it as a
            // failure and can re-issue the stream. A decode error already sent
            // STMn above and has no disconnect reason.
            if (track_->disconnectCode() != DisconnectCode::None) {
                link_.disco(track_->disconnectCode());
                link_.stat(kStatError);
            } else {
                link_.stat(kStatEnd);
            }
            break;
        }

        // Tear down and leave. A flush transition (keepSession) leaves the AirPlay
        // session running for the next track, now driven by the keep-alive
        // driver until a new stream thread takes over.
        if (keepSession)
            output_.park();
        else
            output_.stop(false);
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
void StreamCoordinator::waitForOutputDrain(std::stop_token st) {
    const uint64_t deadline = nowMs() + kDrainTimeoutMs;
    while (!st.stop_requested() && g_run.load() && !output_.lost()) {
        const size_t avail = output_.queued();
        counters_.setQueued(avail);
        if (avail == 0) return;
        if (nowMs() >= deadline) return;
        // Keep the sender running so the buffered tail actually leaves the ring.
        output_.pump(std::chrono::milliseconds(kDrainPumpMs));
    }
}

void StreamCoordinator::shutdown() {
    if (streamThread_.joinable()) {
        // request_stop() fires streamLoop's stop_callback, which interrupts a
        // read() parked in poll()/recv(), so a stalled source cannot hold the
        // join (and thus shutdown) for the read timeout.
        streamThread_.request_stop();
        // Wake the pump out of a pause so it reaches the loop guard promptly.
        if (track_) track_->unpause();
        streamThread_.join();
    }
    if (track_) track_->close();
}

}  // namespace squeeze2raop2