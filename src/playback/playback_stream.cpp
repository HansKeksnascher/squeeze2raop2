#include "playback/playback_stream.h"

#include "app/shutdown_flag.h"
#include "common/log.h"
#include "common/util.h"
#include "debug/debug_wav_sink.h"
#include "lms/icy_meta.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace squeeze2raop2 {

namespace {

// Map an HttpStreamReader open error onto a DSCO reason. The reader's error
// strings are the only signal available at this layer.
DisconnectCode mapOpenError(const std::string& error) {
    if (error.find("timed out") != std::string::npos) return DisconnectCode::Timeout;
    if (error.find("request send") != std::string::npos) return DisconnectCode::Local;
    if (error.find("connect") != std::string::npos) return DisconnectCode::Unreachable;
    return DisconnectCode::Remote;
}

}  // namespace

PlaybackStream::PlaybackStream(AirplayOutput& output, StreamCounters& counters, bool paceRealtime,
                               ResamplerQuality resamplerQuality,
                               std::optional<std::string> sinkPath, uint32_t sourceTimeoutMs)
    : output_(output),
      counters_(counters),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)),
      resamplerQuality_(resamplerQuality),
      sourceTimeoutMs_(sourceTimeoutMs) {}

PlaybackStream::~PlaybackStream() { close(); }

std::optional<std::string> PlaybackStream::openSource(const StrmStart& st, const std::string& host,
                                                      uint16_t port, std::string& error) {
    const PcmFormat input = pcmFormat(st.pcm, kDefaultSampleRate);
    counters_.reset(input.sampleRate);

    // Per-stream audio parameters. Crossfade (mode 1) is unsupported: the
    // bridge never overlaps two tracks in the ring, so map it to no fade.
    replayGain_ = static_cast<int32_t>(st.replayGain);
    fadeMode_ = (st.transitionType == kFadeModeCross) ? kFadeModeNone : st.transitionType;
    fadeSecs_ = st.transitionPeriodS;
    fadeIn_ = false;
    fadeInDone_ = false;
    fadeOut_ = false;
    fadeDone_ = false;
    fadeOutRequested_.store(false);
    skipFrames_.store(0);
    decoderReadyFired_ = false;
    underrunFired_ = false;
    ringEmptySinceMs_ = 0;
    disconnect_ = DisconnectCode::None;

    // Ask for in-band ICY metadata on LMS-proxied streams (the embedded
    // request is bare): LMS's /stream.mp3 only interleaves StreamTitle blocks
    // when the client sends Icy-MetaData: 1.
    std::string request = withIcyRequestHeader(st.request);
    if (!reader_.openBlocking(host, port, request, error, st.ssl)) {
        disconnect_ = mapOpenError(error);
        return std::nullopt;
    }
    reader_.setMetaCallback([this](std::string_view block) { onMeta(block); });
    return reader_.headers();
}

bool PlaybackStream::attachDecoder(StreamFormat format, const PcmParams& pcm, std::string& error) {
    const PcmFormat input = pcmFormat(pcm, kDefaultSampleRate);
    // One decoder per stream format, one feed pipeline for both. Decoders emit
    // at their native rate; the resampler converts to the AirPlay output clock.
    decoder_ = Decoder::create(format, input, pcm.sampleSizeCode);
    if (!decoder_) {
        error = "unsupported stream format";
        return false;
    }
    // The STAT clock and the pacer run on the output clock, not the source rate.
    counters_.setOutputRate(outputRate_);
    log::info(log::Area::Pb, "decoder: {} stream", decoder_->name());
    if (sinkPath_) {
        sink_ = std::make_unique<DebugWavSink>(*sinkPath_);
        // The sink captures the wire signal: the pipeline output format.
        if (!sink_->open(outputFormat(), error)) {
            sink_.reset();
            return false;
        }
    }
    return true;
}

void PlaybackStream::pause(uint32_t ms) {
    // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a timed pause
    // (transition gaps). LMS's stop for a remote stream is a fade-down
    // followed by 'p 0'.
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        pauseUntilMs_.store(ms ? nowMs() + ms : std::numeric_limits<uint64_t>::max(),
                            std::memory_order_relaxed);
    }
    pauseCv_.notify_all();
}

void PlaybackStream::unpause() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        pauseUntilMs_.store(0, std::memory_order_relaxed);
    }
    pauseCv_.notify_all();
}

void PlaybackStream::skipAhead(uint32_t ms) {
    const uint32_t rate = format().sampleRate ? format().sampleRate : kDefaultSampleRate;
    const uint64_t frames = skipFramesFor(ms, rate);
    skipFrames_.fetch_add(frames, std::memory_order_relaxed);
    log::info(log::Area::Pb, "skip ahead {} ms ({} frames)", ms, frames);
}

bool PlaybackStream::requestFadeOut() {
    if (fadeSecs_ == 0 || (fadeMode_ != kFadeModeOut && fadeMode_ != kFadeModeInOut)) return false;
    fadeOutRequested_.store(true, std::memory_order_relaxed);
    return true;
}

void PlaybackStream::interrupt() { reader_.interrupt(); }

void PlaybackStream::close() {
    if (sink_) {
        sink_->close();
        sink_.reset();
    }
    reader_.close();
}

PcmFormat PlaybackStream::format() const { return decoder_ ? outputFormat() : PcmFormat{}; }

PcmFormat PlaybackStream::outputFormat() const {
    return PcmFormat{.sampleRate = outputRate_,
                     .bitsPerSample = kDefaultBitsPerSample,
                     .channels = static_cast<uint8_t>(kDefaultChannels),
                     .bigEndian = false};
}

void PlaybackStream::reapplyNowPlaying() {
    if (!lastTitle_.empty()) output_.setNowPlaying(lastTitle_, "", "");
}

// ICY in-band metadata block (squeezelite parity): forward the raw chunk to
// LMS and push the StreamTitle to the AirPlay receiver.
void PlaybackStream::onMeta(std::string_view block) {
    if (metaForward_) metaForward_(block);
    const std::optional<std::string> title = parseStreamTitle(block);
    if (!title) return;
    // Some stations repeat the identical block every meta interval (~5/s);
    // only log and push on an actual change.
    if (*title == lastTitle_) return;
    log::info(log::Area::Pb, "icy title: {}", *title);
    lastTitle_ = *title;
    output_.setNowPlaying(*title, "", "");
}

PlaybackStream::End PlaybackStream::run(std::stop_token st,
                                        const std::function<void()>& onOutputReady) {
    // Launch the receiver as soon as the first decoded audio is queued: the
    // sender's handshake then runs while the ring fills, so no deep startup
    // fill is needed. A retry creates a fresh ring, so this re-arms per pass.
    const bool haveTarget = output_.hasTarget();
    bool outputReady = !haveTarget;  // no target => nothing to launch

    PcmFormat fmt = format();
    char buf[kReadBufferBytes];
    bool reachedEof = false;
    bool decodeFailed = false;
    lastDataMs_ = nowMs();  // fresh deadline for a (re-entered) pass

    // output_.lost() doubles as the "receiver died" exit signal, set from a
    // sender pump callback (stop requests arrive via st).
    while (!st.stop_requested() && g_run.load() && !output_.lost()) {
        // A stop-path fade request arrives from the reader thread; adopt it.
        if (fadeOutRequested_.exchange(false, std::memory_order_relaxed)) {
            fadeOut_ = true;
            fadeOutFrames_ = 0;
            const uint32_t rate = fmt.sampleRate ? fmt.sampleRate : kDefaultSampleRate;
            fadeOutDur_ = static_cast<uint32_t>(static_cast<uint64_t>(fadeSecs_) * rate);
        }
        if (fadeDone_) return End::FadedOut;

        bool paused = false;
        {
            const uint64_t until = pauseUntilMs_.load(std::memory_order_relaxed);
            if (until && nowMs() < until)
                paused = true;
            else
                pauseUntilMs_.store(0, std::memory_order_relaxed);
        }
        if (!paused && outputReady) sampleRingTelemetry();
        if (paused && !fadeOut_) {
            // Do NOT read while paused. LMS streams local tracks as fast as the
            // client reads, so draining a paused stream consumes the whole
            // track in seconds and hits EOF (then STMd -> LMS skips it); live
            // streams are realtime so they never hit this. Squeezelite behaves
            // the same way: it stops reading and lets the socket backpressure
            // keep the connection open until the resume.
            lastDataMs_ = nowMs();  // a pause is not a source stall
            // Block until unpause(), the timed deadline, or a stop request, in
            // short slices: resume and teardown stay prompt, and each slice is
            // also the sender's service window, so it keeps padding silence on
            // the RTP timeline while no audio is being pushed.
            std::unique_lock<std::mutex> lock(pauseMutex_);
            for (;;) {
                const uint64_t until = pauseUntilMs_.load(std::memory_order_relaxed);
                if (until == 0) break;  // unpaused
                const bool timed = until != std::numeric_limits<uint64_t>::max();
                const uint64_t now = timed ? nowMs() : 0;
                if (timed && until <= now) break;  // timed pause elapsed

                // Wake on unpause (pauseUntilMs_ changed) or after a <=25 ms
                // slice, so a stop request and the sender's keep-alive stay
                // responsive.
                const auto pauseChanged = [this, until] {
                    return pauseUntilMs_.load(std::memory_order_relaxed) != until;
                };
                const uint64_t sliceMs =
                    timed ? std::min<uint64_t>(until - now, kPauseSliceMs) : kPauseSliceMs;
                pauseCv_.wait_for(lock, st, std::chrono::milliseconds(sliceMs), pauseChanged);

                if (st.stop_requested()) break;
                lock.unlock();
                output_.pump(std::chrono::milliseconds(0));
                lock.lock();
            }
            continue;
        }

        uint64_t iterStart = nowMs();
        auto rr = reader_.read(std::span{buf}, kReadPollTimeoutMs);

        if (rr.result == HttpStreamReader::ReadResult::Data && rr.bytes > 0) {
            const auto audio = std::as_bytes(std::span{buf}).first(rr.bytes);
            counters_.onReceived(rr.bytes);
            ringEmptySinceMs_ = 0;
            lastDataMs_ = nowMs();
            if (!feed(st, audio, fmt, sink_.get())) {
                decodeFailed = true;
                break;
            }
            if (fadeDone_) return End::FadedOut;
            if (!outputReady && output_.queued() > 0) {
                outputReady = true;
                onOutputReady();
            }
            // The pacing clock must include the feed cost, not just the read:
            // the ring push can block on backpressure and an under-counted
            // clock makes the pacer over-sleep relative to real elapsed time
            // (ring dips -> receiver silence pads).
            activeMs_ += nowMs() - iterStart;
        } else if (rr.result == HttpStreamReader::ReadResult::Timeout) {
            activeMs_ += nowMs() - iterStart;  // ~= the read timeout
            // A source that has delivered nothing for longer than the watchdog
            // is dead, not merely quiet: a half-open socket (peer gone, or a
            // live peer that stopped sending) never yields Eof/Error, so
            // without this the pump would read timeouts forever with the ring
            // drained. End the track so the session reports DSCO(Timeout) and
            // LMS re-issues the stream.
            const uint64_t now = nowMs();
            if (sourceTimeoutMs_ != 0 && now - lastDataMs_ >= sourceTimeoutMs_) {
                log::warn(log::Area::Pb, "source silent for {} ms; ending stream",
                          now - lastDataMs_);
                disconnect_ = DisconnectCode::Timeout;
                break;
            }
            // Output starved while the source is still active: STMo, but only
            // after the ring has stayed empty for ~1 s. A brief refill gap
            // (e.g. right after a pause/resume dropped the ring) must not make
            // LMS rebuffer; and a paused player has no meaningful underrun.
            const bool empty = !paused && outputUnderrun(output_.running(), output_.queued());
            if (empty) {
                if (ringEmptySinceMs_ == 0) {
                    ringEmptySinceMs_ = nowMs();
                } else if (!underrunFired_ && nowMs() - ringEmptySinceMs_ >= kUnderrunWindowMs) {
                    underrunFired_ = true;
                    log::info(log::Area::Pb, "output underrun while stream active (STMo)");
                    if (underrun_) underrun_();
                }
            } else {
                ringEmptySinceMs_ = 0;
            }
        } else if (rr.result == HttpStreamReader::ReadResult::Closed) {
            // Socket error, not a mere no-data timeout: without this branch the
            // loop used to spin hot on a dead socket forever.
            log::warn(log::Area::Pb, "stream socket error; ending stream");
            disconnect_ = DisconnectCode::Remote;
            break;
        }

        if (rr.result == HttpStreamReader::ReadResult::AtEof) {
            if (decoder_) {
                // Decode + emit the remaining tail frames of the stream (MP3);
                // PCM's finish() is the base no-op.
                decoder_->finish();
                feed(st, {}, fmt, sink_.get());
            }
            disconnect_ = DisconnectCode::Ok;
            reachedEof = true;
            break;
        }

        // Pace reads to playback time: keeps the sender's ring fed without
        // running ahead of the wire.
        if (paceRealtime_ && !fadeOut_) {
            const uint64_t timeline = counters_.fedSamples() * kMsPerSecond / fmt.sampleRate;
            // Pace reads to playback time with a lead: keeps the sender's ring fed
            // without running ahead of the wire. The lead is the pass-through
            // path's only jitter headroom; too small and a proxy burst
            // silence-pads RTP packets.
            if (timeline > activeMs_ + kPacerLeadMs) {
                const uint64_t sleepMs =
                    std::min<uint64_t>(timeline - (activeMs_ + kPacerLeadMs), kPacerMaxSleepMs);
                // The pace wait doubles as the sender's service window: pump
                // until the deadline instead of sleeping, so RTP/sync/retransmit
                // stay on time even though there is no dedicated pump thread.
                output_.pumpUntil(std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(sleepMs));
                activeMs_ += sleepMs;
            }
        }
    }

    if (fadeDone_) return End::FadedOut;
    if (st.stop_requested() || !g_run.load()) return End::Stopped;
    if (output_.lost()) return End::Lost;
    if (reachedEof) return End::Eof;
    if (decodeFailed) return End::DecodeError;
    return End::SocketError;
}

// Drop queued skip frames from one chunk; returns the number of frames to drop
// (which the caller counts as played).
size_t PlaybackStream::consumeSkip(size_t frames) {
    const uint64_t want = skipFrames_.load(std::memory_order_relaxed);
    if (!want || !frames) return 0;
    const size_t drop = static_cast<size_t>(std::min<uint64_t>(want, frames));
    skipFrames_.store(want - drop, std::memory_order_relaxed);
    return drop;
}

// Replay gain * active fade, applied in float into gainScratch_. Returns the
// original chunk when neither applies. The envelope is generated in 16.16
// (squeezelite parity, pinned by test_volume_map) and converted to float.
std::span<const float> PlaybackStream::applyGainFade(std::span<const float> pcm,
                                                     const PcmFormat& fmt) {
    const bool haveReplay = replayGain_ != 0 && replayGain_ != kFixedOne;
    const uint32_t rate = fmt.sampleRate ? fmt.sampleRate : kDefaultSampleRate;
    const size_t channels = fmt.channels ? fmt.channels : kDefaultChannels;
    const uint64_t frames = pcm.size() / channels;

    // Start the initial fade-in on the first emitted chunk (once per stream).
    if (!fadeIn_ && !fadeInDone_ && !fadeOut_ && fadeSecs_ > 0 &&
        (fadeMode_ == kFadeModeIn || fadeMode_ == kFadeModeInOut)) {
        fadeIn_ = true;
        fadeInFrames_ = 0;
        fadeInDur_ = static_cast<uint32_t>(static_cast<uint64_t>(fadeSecs_) * rate);
    }

    float fadeInGain = 1.0F;
    if (fadeIn_) {
        fadeInGain = fixedToGain(fadeGain16(
            static_cast<uint32_t>(std::min<uint64_t>(fadeInFrames_, fadeInDur_)), fadeInDur_,
            /*up=*/true));
        fadeInFrames_ += frames;
        if (fadeInFrames_ >= fadeInDur_) {
            fadeIn_ = false;
            fadeInDone_ = true;
        }
    }
    float fadeOutGain = 1.0F;
    if (fadeOut_ && !fadeDone_) {
        fadeOutGain = fixedToGain(fadeGain16(
            static_cast<uint32_t>(std::min<uint64_t>(fadeOutFrames_, fadeOutDur_)), fadeOutDur_,
            /*up=*/false));
        fadeOutFrames_ += frames;
        if (fadeOutFrames_ >= fadeOutDur_) fadeDone_ = true;
    }

    const bool fadeActive = fadeIn_ || fadeOut_ || fadeInGain != 1.0F || fadeOutGain != 1.0F;
    if (!haveReplay && !fadeActive) return pcm;

    const float base = haveReplay ? fixedToGain(replayGain_) : 1.0F;
    const float gain = base * fadeInGain * fadeOutGain;
    gainScratch_.resize(pcm.size());
    std::transform(pcm.begin(), pcm.end(), gainScratch_.begin(),
                   [gain](float s) { return s * gain; });
    return std::span<const float>(gainScratch_);
}

// The single int16 boundary: round + saturate the float output.
std::span<const int16_t> PlaybackStream::clampToS16(std::span<const float> pcm) {
    outScratch_.resize(pcm.size());
    std::transform(pcm.begin(), pcm.end(), outScratch_.begin(),
                   [](float v) { return floatToS16(v); });
    return std::span<const int16_t>(outScratch_);
}

// One pipeline for every stream format: bytes go through the stream's Decoder
// (mp3 decode / pcm header-skip + s16 stereo normalization) and are drained in
// the same 1152-frame chunks, resampled to the 44.1 kHz AirPlay clock, then
// gain/fade + clamp to s16. Returns false when the decoder failed.
bool PlaybackStream::feed(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
                          DebugWavSink* sink, bool toOutput) {
    if (!decoder_) return true;
    const AirplayOutput::Abort abort = [&] {
        return st.stop_requested() || !g_run.load() || output_.lost();
    };
    decoder_->feed(data);
    for (;;) {
        const std::span<const int16_t> chunk = decoder_->nextChunk();
        if (chunk.empty()) {
            if (decoder_->hasError()) {
                log::error(log::Area::Pb, "{} decode failed; dropping stream", decoder_->name());
                return false;
            }
            break;
        }

        // Decoders emit at their native rate; the resampler converts to the
        // AirPlay output clock. Rebuild it whenever the native rate changes
        // (MP3 has no rate until its first frame).
        const PcmFormat native = decoder_->format();
        if (native.sampleRate != 0 && (native.sampleRate != sourceRate_ || !resampler_)) {
            sourceRate_ = native.sampleRate;
            resampler_ = std::make_unique<Resampler>(sourceRate_, outputRate_, resamplerQuality_);
            if (!resampler_->valid())
                log::warn(log::Area::Pb, "resampler unavailable; audio is not rate-corrected");
            log::info(log::Area::Pb, "{} audio: {} Hz, {} ch", decoder_->name(), native.sampleRate,
                      native.channels);
        }
        if (!decoderReadyFired_) {
            decoderReadyFired_ = true;
            if (decoderReady_) decoderReady_();
        }

        if (!resampler_) continue;  // no native rate yet
        std::span<const float> pcm = resampler_->process(chunk);
        if (pcm.empty()) continue;

        const size_t channels = fmt.channels ? fmt.channels : kDefaultChannels;

        // Skip-ahead (strm a): drop whole output frames, counting them as
        // played so the elapsed clock advances as in squeezelite.
        const size_t droppedFrames = consumeSkip(pcm.size() / channels);
        if (droppedFrames) {
            const size_t droppedSamples = droppedFrames * channels;
            counters_.onFed(droppedSamples, channels, decoder_->pendingBytes());
            pcm = pcm.subspan(droppedSamples);
        }
        if (pcm.empty()) continue;
        if (!toOutput) continue;  // paused drain: decode, discard

        const std::span<const float> gained = applyGainFade(pcm, fmt);
        const std::span<const int16_t> out = clampToS16(gained);
        if (sink) sink->feed(std::as_bytes(out), fmt);
        output_.push(out, channels, abort);
        counters_.onFed(out.size(), channels, decoder_->pendingBytes());
    }
    return true;
}

// One occupancy sample per stream-loop iteration while streaming. Gaps between
// samples are bounded by the read timeout + pacing sleep (~270 ms), so
// sub-iteration zero-crossings can be missed — the min/max summary still shows
// the trend and the warn/recover pair catches real starvation.
void PlaybackStream::sampleRingTelemetry() {
    if (!output_.hasPlayer()) return;
    const size_t avail = output_.queued();
    counters_.setQueued(avail);
    // Ring-health summary every 10 s and the starvation warn/recover pair.
    (void)ringTelemetry_.observe(avail, nowMs());
}

}  // namespace squeeze2raop2
