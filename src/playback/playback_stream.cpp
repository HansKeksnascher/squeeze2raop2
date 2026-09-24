#include "playback/playback_stream.h"

#include "app/shutdown_flag.h"
#include "common/log.h"
#include "common/util.h"
#include "lms/icy_meta.h"
#include "playback/wav_sink.h"

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
                               std::optional<std::string> sinkPath, uint32_t sourceTimeoutMs)
    : output_(output),
      counters_(counters),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)),
      sourceTimeoutMs_(sourceTimeoutMs) {}

PlaybackStream::~PlaybackStream() { close(); }

std::optional<std::string> PlaybackStream::openSource(const StrmStart& st, const std::string& host,
                                                      uint16_t port, std::string& error) {
    const PcmFormat input = pcmFormat(st.pcm, 44100);
    counters_.reset(input.sampleRate);

    // Per-stream audio parameters. Crossfade (mode 1) is unsupported: the
    // bridge never overlaps two tracks in the ring, so map it to no fade.
    replayGain_ = static_cast<int32_t>(st.replayGain);
    fadeMode_ = (st.transitionType == 1) ? 0 : st.transitionType;
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
    const PcmFormat input = pcmFormat(pcm, 44100);
    // One decoder per stream format, one feed pipeline for both. PCM regulates
    // to the AirPlay output clock (44100) so a source that under-delivers
    // cannot drain the pipeline.
    decoder_ = Decoder::create(format, input, 44100, pcm.sampleSizeCode);
    if (!decoder_) {
        error = "unsupported stream format";
        return false;
    }
    log::info(log::Area::Pb, "decoder: {} stream", decoder_->name());
    if (sinkPath_) {
        sink_ = std::make_unique<PcmFileSink>(*sinkPath_);
        if (!sink_->open(decoder_->format(), error)) {
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
    const uint32_t rate = format().sampleRate ? format().sampleRate : 44100;
    const uint64_t frames = skipFramesFor(ms, rate);
    skipFrames_.fetch_add(frames, std::memory_order_relaxed);
    log::info(log::Area::Pb, "skip ahead {} ms ({} frames)", ms, frames);
}

bool PlaybackStream::requestFadeOut() {
    if (fadeSecs_ == 0 || (fadeMode_ != 3 && fadeMode_ != 4)) return false;
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

PcmFormat PlaybackStream::format() const { return decoder_ ? decoder_->format() : PcmFormat{}; }

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
                                        const std::function<void()>& onPrebufferReady) {
    // Startup fill gate: pump into the ring until it holds 50% of capacity
    // (~1.5 s of audio at 44.1 kHz stereo) before the first RTP packet leaves,
    // so playback launches from a deep reserve instead of a sender
    // silence-padding its way into the session. Re-armed each pass: a retry
    // creates a fresh ring.
    const bool haveTarget = output_.hasTarget();
    const size_t prebufferSamples = haveTarget ? output_.capacity() / 2 : 0;
    bool prebuffering = haveTarget && prebufferSamples != 0;
    const uint64_t prebufferStartMs = nowMs();
    if (prebuffering)
        log::info(log::Area::Pb, "prebuffering {} samples (50% of ring)", prebufferSamples);

    PcmFormat fmt = format();
    char buf[4096];
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
            const uint32_t rate = fmt.sampleRate ? fmt.sampleRate : 44100;
            fadeOutDur_ = static_cast<uint32_t>(static_cast<uint64_t>(fadeSecs_) * rate);
            prebuffering = false;
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
        if (!paused && !prebuffering) sampleRingTelemetry();
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
                if (until != std::numeric_limits<uint64_t>::max()) {
                    const uint64_t now = nowMs();
                    if (until <= now) break;  // timed pause elapsed
                    pauseCv_.wait_for(
                        lock, st, std::chrono::milliseconds(std::min<uint64_t>(until - now, 25)),
                        [this, until] {
                            return pauseUntilMs_.load(std::memory_order_relaxed) != until;
                        });
                } else {
                    pauseCv_.wait_for(lock, st, std::chrono::milliseconds(25), [this, until] {
                        return pauseUntilMs_.load(std::memory_order_relaxed) != until;
                    });
                }
                if (st.stop_requested()) break;
                lock.unlock();
                output_.pump(std::chrono::milliseconds(0));
                lock.lock();
            }
            continue;
        }

        uint64_t iterStart = nowMs();
        auto rr = reader_.read(std::span{buf}, 150);

        if (rr.result == HttpStreamReader::ReadResult::Data && rr.bytes > 0) {
            const auto audio = std::as_bytes(std::span{buf}).first(rr.bytes);
            counters_.onReceived(rr.bytes);
            ringEmptySinceMs_ = 0;  // data flowing: not an underrun window
            lastDataMs_ = nowMs();  // source is alive
            if (!feed(st, audio, fmt, sink_.get())) {
                decodeFailed = true;
                break;
            }
            if (fadeDone_) return End::FadedOut;
            if (prebuffering) {
                const size_t avail = output_.queued();
                if (avail >= prebufferSamples) {
                    log::info(log::Area::Pb, "prebuffered {} samples in {} ms; launching", avail,
                              nowMs() - prebufferStartMs);
                    prebuffering = false;
                    onPrebufferReady();
                }
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
                } else if (!underrunFired_ && nowMs() - ringEmptySinceMs_ >= 1000) {
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

        // Pacing must stand down while the rate stage regulates: the decoder
        // intentionally emits ahead of the source (stretching), so an
        // emitted-timeline pacer would throttle the reads, starve the
        // measurement, and spiral the step down. LMS paces the source anyway;
        // without regulation (step 1.0) the pacing keeps the baseline read
        // cadence.
        if (paceRealtime_ && !prebuffering && !fadeOut_ && !(decoder_ && decoder_->regulating())) {
            const uint64_t timeline = counters_.fedSamples() * 1000ULL / fmt.sampleRate;
            // Pace reads to playback time with a lead: keeps the sender's ring
            // fed without running far ahead of the wire. The lead is the
            // pass-through path's only jitter headroom (44.1 kHz PCM goes ring
            // -> RTP packet with no staging, unlike the resampler's 8192-frame
            // inBuf_); too small and any LMS proxy/transcode burst silence-pads
            // RTP packets = crackle. Skipped while prebuffering: the gate wants
            // the ring filled as fast as the source allows.
            if (timeline > activeMs_ + 60) {
                const uint64_t sleepMs = std::min<uint64_t>(timeline - (activeMs_ + 60), 120);
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

// Replay gain * active fade, applied in place into gainScratch_. Returns the
// original chunk when neither applies.
std::span<const int16_t> PlaybackStream::applyGainFade(std::span<const int16_t> chunk,
                                                       const PcmFormat& fmt) {
    const bool haveReplay = replayGain_ != 0 && replayGain_ != kFixedOne;
    const uint32_t rate = fmt.sampleRate ? fmt.sampleRate : 44100;
    const size_t channels = fmt.channels ? fmt.channels : 2;
    const uint64_t frames = chunk.size() / channels;

    // Start the initial fade-in on the first emitted chunk (once per stream).
    if (!fadeIn_ && !fadeInDone_ && !fadeOut_ && fadeSecs_ > 0 &&
        (fadeMode_ == 2 || fadeMode_ == 4)) {
        fadeIn_ = true;
        fadeInFrames_ = 0;
        fadeInDur_ = static_cast<uint32_t>(static_cast<uint64_t>(fadeSecs_) * rate);
    }

    int32_t fadeInGain = kFixedOne;
    if (fadeIn_) {
        fadeInGain =
            fadeGain16(static_cast<uint32_t>(std::min<uint64_t>(fadeInFrames_, fadeInDur_)),
                       fadeInDur_, /*up=*/true);
        fadeInFrames_ += frames;
        if (fadeInFrames_ >= fadeInDur_) {
            fadeIn_ = false;
            fadeInDone_ = true;
        }
    }
    int32_t fadeOutGain = kFixedOne;
    if (fadeOut_ && !fadeDone_) {
        fadeOutGain =
            fadeGain16(static_cast<uint32_t>(std::min<uint64_t>(fadeOutFrames_, fadeOutDur_)),
                       fadeOutDur_, /*up=*/false);
        fadeOutFrames_ += frames;
        if (fadeOutFrames_ >= fadeOutDur_) fadeDone_ = true;
    }

    const bool fadeActive =
        fadeIn_ || fadeOut_ || fadeInGain != kFixedOne || fadeOutGain != kFixedOne;
    if (!haveReplay && !fadeActive) return chunk;

    gainScratch_.resize(chunk.size());
    const int32_t fade =
        static_cast<int32_t>((static_cast<int64_t>(fadeInGain) * fadeOutGain) >> 16);
    const int32_t base = haveReplay ? replayGain_ : kFixedOne;
    const int32_t gain = static_cast<int32_t>((static_cast<int64_t>(base) * fade) >> 16);
    std::transform(chunk.begin(), chunk.end(), gainScratch_.begin(),
                   [gain](int16_t s) { return applyGain16(s, gain); });
    return std::span<const int16_t>(gainScratch_);
}

// One pipeline for every stream format: bytes go through the stream's Decoder
// (mp3 decode / pcm header-skip + s16 stereo normalization) and are drained in
// the same 1152-frame chunks. Returns false when the decoder failed.
bool PlaybackStream::feed(std::stop_token st, std::span<const std::byte> data, PcmFormat& fmt,
                          PcmFileSink* sink, bool toOutput) {
    if (!decoder_) return true;
    const AirplayOutput::Abort abort = [&] {
        return st.stop_requested() || !g_run.load() || output_.lost();
    };
    decoder_->feed(data);
    for (;;) {
        std::span<const int16_t> chunk = decoder_->nextChunk();
        if (chunk.empty()) {
            if (decoder_->hasError()) {
                log::error(log::Area::Pb, "{} decode failed; dropping stream", decoder_->name());
                return false;
            }
            break;
        }
        const PcmFormat norm = decoder_->format();
        if (norm.sampleRate != 0 && fmt != norm) {
            fmt = norm;
            counters_.setOutputRate(fmt.sampleRate);
            output_.setInputRate(fmt.sampleRate);
            log::info(log::Area::Pb, "{} audio: {} Hz, {} ch", decoder_->name(), fmt.sampleRate,
                      fmt.channels);
        }
        if (!decoderReadyFired_) {
            decoderReadyFired_ = true;
            if (decoderReady_) decoderReady_();
        }

        const size_t channels = fmt.channels ? fmt.channels : 2;

        // Skip-ahead (strm a): drop whole frames, counting them as played so
        // the elapsed clock advances as in squeezelite.
        const size_t droppedFrames = consumeSkip(chunk.size() / channels);
        if (droppedFrames) {
            const size_t droppedSamples = droppedFrames * channels;
            counters_.onFed(droppedSamples, channels, decoder_->pendingBytes());
            chunk = chunk.subspan(droppedSamples);
        }
        if (chunk.empty()) continue;
        if (!toOutput) continue;  // paused drain: decode, discard

        const std::span<const int16_t> out = applyGainFade(chunk, fmt);
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
    if (!decoder_) return;
    // Ring-health summary every 10 s; on the boundary, let the decoder
    // regulate its source rate to the output clock.
    if (const auto window = ringTelemetry_.observe(avail, nowMs()))
        decoder_->regulateRate(counters_.bytesReceived(), avail, *window);
}

}  // namespace squeeze2raop2
