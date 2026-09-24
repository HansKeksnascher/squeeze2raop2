#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "common/net_util.h"  // UniqueFd
#include "lms/wire_types.h"

namespace squeeze2raop2 {

bool discoverLms(std::string& hostOut, uint16_t port, uint32_t timeoutMs);

class SlimProtoClient {
public:
    struct Events {
        std::function<void(const StrmStart&)> onStart;
        std::function<void()> onStop;
        std::function<void(uint32_t intervalMs)> onPause;
        std::function<void(uint32_t resumeJiffies)> onUnpause;
        std::function<void(bool expectFlush)> onFlush;
        std::function<void(uint32_t skipMs)> onSkipAhead;
        std::function<void(uint32_t metaint)> onCont;
        std::function<void(StreamFormat format, const PcmParams& pcm)> onCodc;
        std::function<void(double leftPct, double rightPct)>
            onVolume;  // 0..100; LMS slider percent
        // 'aude': output enable/disable (keyed on enable_spdif, squeezelite
        // parity). false = power the player down.
        std::function<void(bool enable)> onAude;
        std::function<void(const std::string& name)> onSetName;
        std::function<void(uint32_t serverIp)> onServerSwitch;
    };

    SlimProtoClient(std::array<uint8_t, 6> mac, std::string caps, Events events);
    ~SlimProtoClient();

    void setPlayerName(const std::string& name);
    // Server-silence watchdog: reconnect when no packet arrives for this long
    // (squeezelite's 35 s default). Configured from [global] server-timeout-ms.
    void setServerTimeout(uint32_t ms) { serverTimeoutMs_ = ms; }
    void setStatsProvider(std::function<StreamStats()> provider) {
        statsProvider_ = std::move(provider);
    }
    const std::string& serverHost() const { return host_; }

    void start(const std::string& host, uint16_t port);
    void stop();

    // event must be a 4-character code (the array reference makes short/null
    // event strings unrepresentable at every call site).
    void sendStat(const char (&event)[5], StreamStats stats, uint32_t serverTimestamp = 0);
    void sendResp(const std::string& header);
    void sendSetdName(const std::string& name);
    void sendDisco(uint8_t reason);
    void sendMeta(std::string_view data);
    // A hard-button / IR code press (squeezelite parity). Used to nudge the
    // LMS volume from receiver-initiated volume changes: LMS maps the
    // volup/voldown codes to its own volume mixer and answers with AUDG.
    void sendButton(uint32_t code);

    const std::array<uint8_t, 6>& mac() const { return mac_; }

    // Exposed for the packet-bounds unit test: parses one framed LMS packet.
    void process(const std::string& packet);

private:
    void run(std::stop_token st);
    bool connectOnce(bool reconnect);
    // opcode must be a 4-character string literal (the array reference makes
    // null/short opcodes unrepresentable at every call site).
    [[nodiscard]] bool sendPacket(const char (&opcode)[5], std::span<const std::byte> payload);
    [[nodiscard]] bool sendRaw(std::span<const std::byte> data);
    void sendHelo(bool reconnect);
    void maybeHeartbeat();
    // Snapshot of the last stats passed to sendStat(); guarded by sendMutex_
    // because sendStat() also runs on stream threads (STMn/STMu/STMd paths).
    StreamStats lastStats();
    // Copy of the current socket, taken under sockMutex_. Callers hold the
    // shared_ptr for as long as they touch the fd, so a concurrent reconnect
    // or stop() can replace the socket without closing the fd under them.
    [[nodiscard]] std::shared_ptr<UniqueFd> currentSock() const;

    std::array<uint8_t, 6> mac_;
    std::string caps_;
    Events events_;
    std::string host_;
    uint16_t port_ = 3483;

    // Socket lifetime: guarded by sockMutex_, kept alive across sends by shared
    // ownership. Replaced (never reset() in place) on reconnect. A socket is
    // closed when its last shared_ptr owner drops it, so a send in flight on
    // the old connection is never cut short by connectOnce().
    std::shared_ptr<UniqueFd> sock_;
    mutable std::mutex sockMutex_;

    std::jthread thread_;
    StreamStats stats_{};
    std::mutex sendMutex_;  // serializes packet writes and guards stats_
    uint64_t lastHeartbeatMs_ = 0;
    uint32_t serverTimeoutMs_ = 35000;
    uint64_t lastServerMsgMs_ = 0;
    std::string playerName_;
    bool reconnect_ = false;
    std::function<StreamStats()> statsProvider_;
    // Strictly increasing 1 kHz tick for BUTN hard-button presses: LMS drops a
    // press whose timestamp equals the previous one, so fast receiver volume
    // nudges must never collide.
    std::atomic<uint32_t> buttonTick_{0};
};

}  // namespace squeeze2raop2
