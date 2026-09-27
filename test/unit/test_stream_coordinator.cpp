// Pins StreamCoordinator's null-track robustness and its played-time STAT
// snapshot (stream_coordinator.cpp). The full stream loop — decoder attach,
// prebuffer gating, EOF/error/retry exit paths — needs a live HTTP source and
// receiver and is exercised end-to-end by the integration scenarios
// (single_player_stream, queue_advance, stream_stall, pause_resume, ...); the
// pure exit decision it applies is pinned in test_exit_policy.cpp.

#include "playback/stream_coordinator.h"

#include "airplay/airplay_output.h"
#include "check.h"

#include <memory>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace {

struct StubDelegate : SlimProtoSession::Delegate {
    void onStreamStart(const StrmStart&) override {}
    void onCont() override {}
    void onStop() override {}
    void onFlush() override {}
    void onPause(uint32_t) override {}
    void onUnpause(uint32_t) override {}
    void onSkipAhead(uint32_t) override {}
    void onCodc(StreamFormat, const PcmParams&) override {}
    void onAude(bool) override {}
    void onLmsVolume(double, double) override {}
};

struct Harness {
    AirplayOutput output{"test", "AABBCCDDEEFF", std::nullopt, nullptr, CredentialSink{}, 50};
    StreamCounters counters;
    StubDelegate delegate;
    std::unique_ptr<SlimProtoSession> link;
    std::unique_ptr<VolumeController> volume;
    std::unique_ptr<StreamCoordinator> sc;

    Harness() {
        ResolvedPlayerConfig cfg;
        cfg.name = "Kitchen";
        cfg.mac = {0xaa, 0, 0, 0, 0, 0x01};
        GlobalConfig global;
        link = std::make_unique<SlimProtoSession>(cfg, global, output, NameSink{}, delegate);
        volume = std::make_unique<VolumeController>(
            output, *VolumeAnchors::parse(kDefaultVolumeMap), VolumeMode::Lms, 0.7f, true,
            [] { return true; }, [](uint32_t) {});
        sc = std::make_unique<StreamCoordinator>(output, counters, *link, *volume,
                                                 /*sourceTimeoutMs=*/15000, /*paceRealtime=*/true,
                                                 std::nullopt);
    }
};

// A strm s with a resolved server IP but no request line is rejected before a
// track is created.
StrmStart brokenStart() {
    StrmStart st;
    st.format = StreamFormat::Pcm;
    st.serverIp = 0x7f000001u;  // 127.0.0.1
    st.serverPort = 9000;
    st.request.clear();
    return st;
}

}  // namespace

SQ2_TEST(stream_coordinator, invalid_start_is_rejected) {
    Harness h;
    h.sc->onStreamStart(brokenStart());
    expect(!h.sc->active(), "a start without a request creates no active stream");
    // No track: the lifecycle handlers must stay safe no-ops.
    h.sc->pause(100);
    h.sc->unpause(100);
    h.sc->skipAhead(500);
    h.sc->flush();
    h.sc->requestStopOrFade();
    h.sc->audeOff();
    h.sc->shutdown();
    h.sc->shutdown();  // idempotent
    expect(!h.sc->active(), "still inactive after lifecycle calls");
}

SQ2_TEST(stream_coordinator, current_stats_uses_output_ring) {
    Harness h;
    // Reset the counters with a known input rate, then feed some data.
    h.counters.reset(44100);
    h.counters.onReceived(44100 * 4);
    // 88200 interleaved s16 samples = 44100 stereo frames = one second.
    h.counters.onFed(/*samples=*/88200, /*channels=*/2, /*pendingBytes=*/0);

    const StreamStats st = h.sc->currentStats();
    expect(st.streamBufferSize == kStreamBufferBytes, "reports the fixed stream buffer size");
    expect(st.bytesReceived == 44100u * 4u, "reports received bytes");
    // The output fields come from the live ring occupancy, not the counters.
    expect(st.outputBufferSize == h.output.capacityBytes(), "output buffer size from the ring");
    expect(st.outputBufferFullness == h.output.queuedBytes(), "output fullness from the ring");
    expect(st.elapsedMs == 1000, "one second of played audio");
}

SQ2_TEST(stream_coordinator, aude_off_stops_output) {
    Harness h;
    h.sc->audeOff();
    expect(h.output.state() == AirplayOutput::State::Absent, "power off tears the output down");
}
