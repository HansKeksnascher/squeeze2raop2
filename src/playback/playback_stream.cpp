#include "playback/playback_stream.h"

#include "lms/icy_meta.h"
#include "common/log.h"
#include "app/shutdown_flag.h"
#include "common/util.h"
#include "playback/wav_sink.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace squeeze2raop2 {

PlaybackStream::PlaybackStream(AirplayOutput& output, StreamCounters& counters, bool paceRealtime,
                               std::optional<std::string> sinkPath)
    : output_(output),
      counters_(counters),
      paceRealtime_(paceRealtime),
      sinkPath_(std::move(sinkPath)) {}

PlaybackStream::~PlaybackStream() { close(); }

std::optional<std::string> PlaybackStream::open(const StrmStart& st, const std::string& host,
                                                uint16_t port, std::string& error) {
    const PcmFormat input = pcmFormat(st.pcm, 44100);
    counters_.reset(input.sampleRate);
    // One decoder per stream format, one feed pipeline for both. PCM regulates
    // to the AirPlay output clock (44100) so a source that under-delivers
    // cannot drain the pipeline.
    decoder_ = Decoder::create(st.format, input, 44100);
    if (!decoder_) {
        error = "unsupported stream format";
        return std::nullopt;
    }
    log::info("strm s: {} stream via decoder pipeline", decoder_->name());

    // Ask for in-band ICY metadata on LMS-proxied streams (the embedded
    // request is bare): LMS's /stream.mp3 only interleaves StreamTitle blocks
    // when the client sends Icy-MetaData: 1.
    std::string request = withIcyRequestHeader(st.request);
    if (!reader_.openBlocking(host, port, request, error)) return std::nullopt;
    reader_.setMetaCallback([this](std::string_view block) { onMeta(block); });

    if (sinkPath_) {
        sink_ = std::make_unique<PcmFileSink>(*sinkPath_);
        if (!sink_->open(decoder_->format(), error)) {
            sink_.reset();
            return std::nullopt;
        }
    }
    return reader_.headers();
}

void PlaybackStream::pause(uint32_t ms) {
    // squeezelite parity: 'p 0' pauses indefinitely, 'p N' is a timed pause
    // (transition gaps). LMS's stop for a remote stream is a fade-down
    // followed by 'p 0'.
    pauseUntilMs_.store(ms ? nowMs() + ms : std::numeric_limits<uint64_t>::max(),
                        std::memory_order_relaxed);
}

void PlaybackStream::unpause() { pauseUntilMs_.store(0, std::memory_order_relaxed); }

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
    log::info("icy title: {}", *title);
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
    if (prebuffering) log::info("[ap] prebuffering {} samples (50% of ring)", prebufferSamples);

    PcmFormat fmt = format();
    char buf[4096];
    bool reachedEof = false;
    bool decodeFailed = false;

    // output_.lost() doubles as the "receiver died" exit signal set from the
    // sender's io thread (stop requests arrive via st).
    while (!st.stop_requested() && g_run.load() && !output_.lost()) {
        bool paused = false;
        {
            const uint64_t until = pauseUntilMs_.load(std::memory_order_relaxed);
            if (until && nowMs() < until)
                paused = true;
            else
                pauseUntilMs_.store(0, std::memory_order_relaxed);
        }
        if (!paused && !prebuffering) sampleRingTelemetry();
        if (paused) {
            // Drain instead of sleep: a paused player that stops reading
            // backpressures the LMS proxy, and LMS un-pauses the stream itself
            // within seconds (observed ~5 s, with a volume fade up) when its
            // writer stalls. Keep reading + decoding and discard the PCM
            // (toOutput=false also skips the elapsed accounting, so progress
            // stays frozen). Discarding is what keeps a resumed *live* stream
            // live: a retained backlog would replay stale audio after a long
            // pause. The sender's timeline runs on silence, so unpause resumes
            // seamlessly. (This relies on the STAT heartbeat carrying the real
            // byte count; a zeroed reply makes LMS close the stream.)
            auto rr = reader_.read(std::span{buf}, 20);
            if (rr.result == HttpStreamReader::ReadResult::Data && rr.bytes > 0) {
                if (!feed(st, std::as_bytes(std::span{buf}).first(rr.bytes), fmt, nullptr,
                          /*toOutput=*/false)) {
                    decodeFailed = true;
                    break;
                }
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
            counters_.onReceived(rr.bytes);
            if (!feed(st, audio, fmt, sink_.get())) {
                decodeFailed = true;
                break;
            }
            if (prebuffering) {
                const size_t avail = output_.queued();
                if (avail >= prebufferSamples) {
                    log::info("[ap] prebuffered {} samples in {} ms; launching", avail,
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
        } else if (rr.result == HttpStreamReader::ReadResult::Closed) {
            // Socket error, not a mere no-data timeout: without this branch the
            // loop used to spin hot on a dead socket forever.
            log::warn("stream socket error; ending stream");
            break;
        }

        if (rr.result == HttpStreamReader::ReadResult::AtEof) {
            if (decoder_) {
                // Decode + emit the remaining tail frames of the stream (MP3);
                // PCM's finish() is the base no-op.
                decoder_->finish();
                feed(st, {}, fmt, sink_.get());
            }
            reachedEof = true;
            break;
        }

        // Pacing must stand down while the rate stage regulates: the decoder
        // intentionally emits ahead of the source (stretching), so an
        // emitted-timeline pacer would throttle the reads, starve the
        // measurement, and spiral the step down. LMS paces the source anyway;
        // without regulation (step 1.0) the pacing keeps the baseline read
        // cadence.
        if (paceRealtime_ && !prebuffering && !(decoder_ && decoder_->regulating())) {
            const uint64_t timeline = counters_.fedSamples() * 1000ULL / fmt.sampleRate;
            // Pace reads to playback time with a lead: keeps the sender's ring
            // fed without running far ahead of the wire. The lead is the
            // pass-through path's only jitter headroom (44.1 kHz PCM goes ring
            // -> RTP packet with no staging, unlike the resampler's 8192-frame
            // inBuf_); too small and any LMS proxy/transcode burst silence-pads
            // RTP packets = crackle. Skipped while prebuffering: the gate wants
            // the ring filled as fast as the source allows.
            if (timeline > activeMs_ + 60) {
                uint64_t sleepMs = std::min<uint64_t>(timeline - (activeMs_ + 60), 120);
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                activeMs_ += sleepMs;
            }
        }
    }

    if (st.stop_requested() || !g_run.load()) return End::Stopped;
    if (output_.lost()) return End::Lost;
    if (reachedEof) return End::Eof;
    if (decodeFailed) return End::DecodeError;
    return End::SocketError;
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
        const std::span<const int16_t> chunk = decoder_->nextChunk();
        if (chunk.empty()) {
            if (decoder_->hasError()) {
                log::error("{} decode failed; dropping stream", decoder_->name());
                return false;
            }
            break;
        }
        const PcmFormat norm = decoder_->format();
        if (norm.sampleRate != 0 && fmt != norm) {
            fmt = norm;
            counters_.setOutputRate(fmt.sampleRate);
            output_.setInputRate(fmt.sampleRate);
            log::info("[ap] {} audio: {} Hz, {} ch", decoder_->name(), fmt.sampleRate,
                      fmt.channels);
        }
        if (!toOutput) continue;  // paused drain: decode, discard
        if (sink) sink->feed(std::as_bytes(chunk), fmt);
        output_.push(chunk, fmt.channels, abort);
        counters_.onFed(chunk.size(), fmt.channels, decoder_->pendingBytes());
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
