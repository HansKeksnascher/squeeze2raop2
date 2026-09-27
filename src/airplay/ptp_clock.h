#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace squeeze2raop2 {

// One gPTP clock service for the whole process. UDP 319/320 are process-global,
// so every concurrent AirPlay 2 session shares this object; it tracks one
// sample per receiver (keyed by source IP) and never assumes a single peer.
//
// The worker behaves like a Mac sender: it bursts Sync+Announce+Signaling at
// each peer to prompt it, then yields: for a receiver that starts sending its
// own Sync/Follow_Up (a standalone HomePod) it stays silent and follows that
// timeline. Only a peer that never sends Sync after a probe window is served as
// grandmaster (sync packets keep carrying our clock). HomePods never answer
// Delay_Req, so the offset is taken one-way from each Sync/Follow_Up pair; a
// Delay_Resp, if one arrives, refines it.
class PtpClock {
public:
    struct Config {
        // Network interface to advertise/bind (empty = all interfaces). The
        // interface's IPv4 becomes the timingPeerInfo address.
        std::string iface;
        uint16_t eventPort = 319;  // 0 = ephemeral (tests)
        uint16_t generalPort = 320;
        uint8_t priority1 = 250;    // yield to a receiver with a lower value
        bool joinMulticast = true;  // best-effort join of 224.0.1.129
        // After the initial burst we stay silent this long, waiting for the
        // receiver's own Sync/Follow_Up. A receiver that wants to be master
        // (standalone HomePod) sends Sync well inside this window; one that
        // slaves to us never does, so we announce as grandmaster only after it
        // elapses.
        std::chrono::milliseconds probeTimeout{5000};
        // Re-assert interval once we are acting as grandmaster for a peer.
        std::chrono::milliseconds masterInterval{2000};
    };

    struct Sample {
        uint64_t peerClockId = 0;   // receiver's master clock identity
        uint64_t deviceTimeNs = 0;  // receiver PTP time at localTimeNs
        uint64_t localTimeNs = 0;   // CLOCK_MONOTONIC at capture
        int64_t offsetNs = 0;       // receiver - local
        uint64_t rttNs = 0;         // round-trip, 0 for a one-way sample
    };

    PtpClock();
    ~PtpClock();
    PtpClock(const PtpClock&) = delete;
    PtpClock& operator=(const PtpClock&) = delete;

    // Bind the PTP ports and start the worker. Returns false (and leaves the
    // clock unavailable) when the interface cannot be resolved or a port cannot
    // be bound -- the caller then keeps NTP timing. Idempotent.
    bool start(const Config& config);
    void stop();

    bool available() const;
    const std::string& error() const;
    // Our shared 8-byte gPTP identity (timingPeerInfo ClockID).
    uint64_t clockId() const;
    // Actual bound ports (== Config ports, or the kernel's when 0).
    uint16_t eventPort() const;
    uint16_t generalPort() const;
    // The interface IPv4 to advertise, or empty to fall back to the RTSP
    // session's local address.
    std::string localAddress() const;

    // Per-peer lifecycle: one peer per active AirPlay 2 session. addPeer
    // resolves `host` to IPv4, sends the initial BMCA burst and returns the
    // canonical peer IP to use with the accessors; removePeer forgets it.
    std::string addPeer(const std::string& host);
    void removePeer(const std::string& peerIp);

    bool synchronized(const std::string& peerIp) const;
    // Receiver PTP time now, extrapolated from the last sample; falls back to
    // local monotonic time until the first sample converges.
    uint64_t nowNs(const std::string& peerIp) const;
    // The receiver's master clock identity, or our own before one is learned.
    uint64_t peerClockId(const std::string& peerIp) const;
    std::optional<Sample> sample(const std::string& peerIp) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace squeeze2raop2
