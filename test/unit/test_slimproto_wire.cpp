#include "lms/slimproto.h"

#include "check.h"
#include "common/util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace squeeze2raop2::test;
using squeeze2raop2::PcmParams;
using squeeze2raop2::SlimProtoClient;
using squeeze2raop2::StreamFormat;
using squeeze2raop2::StreamStats;

namespace {

// Loopback listener so SlimProtoClient::start() exercises the real connect
// path (connectOnce + run thread) without any external dependency.
class LoopbackServer {
public:
    LoopbackServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd_ >= 0, "server socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        require(::listen(fd_, 1) == 0, "listen");
        socklen_t len = sizeof(addr);
        require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        port_ = ntohs(addr.sin_port);
    }
    ~LoopbackServer() {
        if (conn_ >= 0) ::close(conn_);
        if (fd_ >= 0) ::close(fd_);
    }

    uint16_t port() const { return port_; }

    void acceptConnection() {
        require(conn_ < 0, "one connection at a time");
        // client connects immediately after start()
        pollfd pfd{fd_, POLLIN, 0};
        require(::poll(&pfd, 1, 5000) == 1, "client connects within 5s");
        conn_ = ::accept(fd_, nullptr, nullptr);
        require(conn_ >= 0, "accept");
    }

    // Framed server-side receive bounded by timeoutMs; nullopt on timeout,
    // closed connection or a malformed frame. Non-fatal, so a test can assert
    // that no further packet arrives.
    std::optional<std::vector<unsigned char>> readPacketFor(int timeoutMs) {
        if (conn_ < 0) return std::nullopt;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        auto readExact = [&](unsigned char* dst, size_t n) {
            size_t got = 0;
            while (got < n) {
                pollfd pfd{conn_, POLLIN, 0};
                const int remaining =
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         deadline - std::chrono::steady_clock::now())
                                         .count());
                if (remaining <= 0 || ::poll(&pfd, 1, remaining) != 1) return false;
                const ssize_t r = ::recv(conn_, dst + got, n - got, 0);
                if (r <= 0) return false;
                got += static_cast<size_t>(r);
            }
            return true;
        };
        constexpr size_t kHeaderBytes = 8;
        std::vector<unsigned char> pkt(kHeaderBytes);
        if (!readExact(pkt.data(), kHeaderBytes)) return std::nullopt;
        const uint32_t len = (static_cast<uint32_t>(pkt[4]) << 24) |
                             (static_cast<uint32_t>(pkt[5]) << 16) |
                             (static_cast<uint32_t>(pkt[6]) << 8) | pkt[7];
        if (len > 4096) return std::nullopt;
        pkt.resize(kHeaderBytes + len);
        if (!readExact(pkt.data() + kHeaderBytes, len)) return std::nullopt;
        return pkt;
    }

    // Framed server-side receive with a hard deadline (keeps failures
    // graceful instead of hanging the test).
    std::vector<unsigned char> readPacket() {
        auto pkt = readPacketFor(5000);
        require(pkt.has_value(), "timed out waiting for packet bytes");
        return *pkt;
    }

    // LMS -> client framing: [2b BE length = opcode+payload][opcode][payload]
    // (SlimProtoClient::run reads the 2-byte length prefix itself).
    void sendPacket(std::string_view opcode, std::span<const unsigned char> payload) {
        require(conn_ >= 0, "connection open for sends");
        require(opcode.size() == 4, "4-byte opcode");
        require(payload.size() <= 32000, "payload fits the 2-byte length prefix");
        const uint16_t len = static_cast<uint16_t>(4 + payload.size());
        const unsigned char hdr[2] = {static_cast<unsigned char>(len >> 8),
                                      static_cast<unsigned char>(len & 0xFF)};
        std::vector<unsigned char> pkt(4 + payload.size());
        std::memcpy(pkt.data(), opcode.data(), 4);
        std::memcpy(pkt.data() + 4, payload.data(), payload.size());
        auto sendAll = [&](const unsigned char* data, size_t n) {
            size_t done = 0;
            while (done < n) {
                ssize_t r = ::send(conn_, data + done, n - done, 0);
                require(r > 0, "send packet to client");
                done += static_cast<size_t>(r);
            }
        };
        sendAll(hdr, sizeof(hdr));
        sendAll(pkt.data(), pkt.size());
    }

    void expectClosedByClient() {
        // after stop(): BYE! arrives, then EOF
        auto bye = readPacket();
        expect(std::string_view(reinterpret_cast<const char*>(bye.data()), 4) == "BYE!",
               "BYE! opcode on stop");
        pollfd pfd{conn_, POLLIN, 0};
        expect(::poll(&pfd, 1, 5000) == 1, "eof pending");
        char c = 0;
        expect(::recv(conn_, &c, 1, 0) == 0, "connection closed by client");
    }

    // Drop the current connection and accept the client's reconnect (the
    // watchdog closes the socket and dials again).
    void acceptReconnect() {
        if (conn_ >= 0) {
            ::close(conn_);
            conn_ = -1;
        }
        pollfd pfd{fd_, POLLIN, 0};
        require(::poll(&pfd, 1, 5000) == 1, "client reconnects within 5s");
        conn_ = ::accept(fd_, nullptr, nullptr);
        require(conn_ >= 0, "accept reconnect");
    }

private:
    int fd_ = -1;
    int conn_ = -1;
    uint16_t port_ = 0;
};

std::string opcodeOf(const std::vector<unsigned char>& pkt) {
    return std::string(reinterpret_cast<const char*>(pkt.data()), 4);
}

}  // namespace

SQ2_TEST(wire, helo_framing_and_stat_round_trip) {
    LoopbackServer server;

    std::atomic<bool> volumeSeen{false};
    std::atomic<double> volumePct{-1.0};
    SlimProtoClient::Events events;
    events.onVolume = [&](double l, double r) {
        volumePct.store((l == r) ? r : (l + r) / 2.0);
        volumeSeen.store(true);
    };

    const std::string caps =
        "Model=squeezelite,ModelName=test,AccuratePlayPoints=1,HasDigitalOut=1,"
        "MaxSampleRate=96000,Firmware=wire-test,mp3,pcm";
    const std::array<unsigned char, 6> mac{0xaa, 0x01, 0x02, 0x03, 0x04, 0x05};
    SlimProtoClient client(mac, caps, std::move(events));
    client.start("127.0.0.1", server.port());
    server.acceptConnection();

    // --- HELO ---
    const auto helo = server.readPacket();
    expect(opcodeOf(helo) == "HELO", "first packet is HELO");
    const uint32_t heloLen = (static_cast<uint32_t>(helo[4]) << 24) |
                             (static_cast<uint32_t>(helo[5]) << 16) |
                             (static_cast<uint32_t>(helo[6]) << 8) | helo[7];
    expect(heloLen == 36 + caps.size(), "HELO body length = 36 + caps");
    expect(helo.size() == 8 + heloLen, "HELO packet fully framed");
    expect(helo[8] == 12, "HELO deviceid byte");
    expect(helo[9] == 1, "HELO revision byte");
    for (size_t i = 0; i < mac.size(); ++i)
        expect(helo[10 + i] == mac[i], "HELO carries the client mac");
    expect(
        std::string_view(reinterpret_cast<const char*>(helo.data() + 8 + 36), caps.size()) == caps,
        "HELO carries the caps string");

    // --- 'strm t' heartbeat request -> expect STMt STAT with len 53 ---
    std::vector<unsigned char> strmT(18);
    strmT[0] = 't';
    const uint32_t ts = 0x11223344;
    strmT[14] = static_cast<unsigned char>(ts >> 24);
    strmT[15] = static_cast<unsigned char>(ts >> 16);
    strmT[16] = static_cast<unsigned char>(ts >> 8);
    strmT[17] = static_cast<unsigned char>(ts & 0xFF);
    server.sendPacket("strm", strmT);
    const auto stat = server.readPacket();
    expect(opcodeOf(stat) == "STAT", "strm t answered with STAT");
    expect(std::string_view(reinterpret_cast<const char*>(stat.data() + 8), 4) == "STMt",
           "STAT carries STMt event");
    const uint32_t statLen = (static_cast<uint32_t>(stat[4]) << 24) |
                             (static_cast<uint32_t>(stat[5]) << 16) |
                             (static_cast<uint32_t>(stat[6]) << 8) | stat[7];
    expect(statLen == 53, "STAT body is 53 bytes");
    expect(stat[8 + 47] == (ts >> 24) && stat[8 + 50] == (ts & 0xFF),
           "server timestamp echoed in STAT");

    // --- audg -> onVolume (16.16 fixed gain 65536 => 100 %) ---
    std::vector<unsigned char> audg(18);
    audg[8] = 1;  // dvc flag: adjust
    // gainL at packet bytes 14..17, gainR at 18..21 -> payload offset 10..13 / 14..17
    const uint32_t gain = 65536;
    for (int i = 0; i < 4; ++i) {
        audg[10 + static_cast<size_t>(i)] = static_cast<unsigned char>(gain >> (24 - 8 * i));
        audg[14 + static_cast<size_t>(i)] = static_cast<unsigned char>(gain >> (24 - 8 * i));
    }
    server.sendPacket("audg", audg);
    for (int i = 0; i < 100 && !volumeSeen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(volumeSeen.load(), "audg fired onVolume");
    expect(volumePct.load() > 99.9 && volumePct.load() <= 100.0, "full-scale gain maps to 100%");

    // --- audg mid-range gain -> ~50 % (LMS slider 50 = -24.67 dB) ---
    // SqueezePlay curve: slider 50 -> -24.667 dB -> linear 0.058416 ->
    // LMS dBToFixed quantizes to 8 fractional bits: 15*256 = 3840.
    const uint32_t midGain = 3840;
    volumeSeen.store(false);
    for (int i = 0; i < 4; ++i) {
        audg[10 + static_cast<size_t>(i)] = static_cast<unsigned char>(midGain >> (24 - 8 * i));
        audg[14 + static_cast<size_t>(i)] = static_cast<unsigned char>(midGain >> (24 - 8 * i));
    }
    server.sendPacket("audg", audg);
    for (int i = 0; i < 100 && !volumeSeen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(volumeSeen.load(), "mid-range audg fired onVolume");
    expect(volumePct.load() > 49.9 && volumePct.load() < 50.1, "slider-50 gain decodes to ~50%");

    // --- audg mute (gain 0) -> pct 0 (the receiver's -144 mute sentinel) ---
    volumeSeen.store(false);
    for (int i = 10; i < 18; ++i) audg[static_cast<size_t>(i)] = 0;
    server.sendPacket("audg", audg);
    for (int i = 0; i < 100 && !volumeSeen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(volumeSeen.load(), "zero-gain audg fired onVolume");
    expect(volumePct.load() == 0.0, "LMS mute decodes to pct 0");

    // --- audg dvc=0 -> no onVolume (LMS fixed-output mode) ---
    volumeSeen.store(false);
    std::vector<unsigned char> audgNoAdj(18);
    audgNoAdj[10] = 0x40;  // nonzero gains: if the dvc=0 short-circuit were
    audgNoAdj[11] = 0x00;  // missing, this would decode to a very loud pct
    audgNoAdj[14] = 0x40;
    audgNoAdj[15] = 0x00;
    server.sendPacket("audg", audgNoAdj);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    expect(!volumeSeen.load(), "dvc=0 audg does not fire onVolume");

    // --- stop(): BYE! + EOF, clean join ---
    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, stream_start_event) {
    LoopbackServer server;
    std::atomic<bool> startSeen{false};
    std::atomic<uint32_t> startPort{0};
    SlimProtoClient::Events events;
    events.onStart = [&](const squeeze2raop2::StrmStart& st) {
        startPort.store(st.serverPort);
        startSeen.store(true);
    };
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x01};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    // 28-byte strm s body (autostart=0, no stream thread in the test);
    // body offsets: command 4, autostart 5, format 6, serverPort 22..23
    // (= payload offsets 0, 1, 2, 18..19 behind the 4-byte opcode)
    std::vector<unsigned char> strmS(24);
    strmS[0] = 's';
    strmS[1] = '0';  // autostart off
    strmS[2] = 'p';  // pcm
    strmS[18] = 9000 >> 8;
    strmS[19] = 9000 & 0xFF;
    server.sendPacket("strm", strmS);
    for (int i = 0; i < 100 && !startSeen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(startSeen.load(), "strm s fired onStart");
    expect(startPort.load() == 9000, "onStart carries serverPort");

    // the 's' handler ACKs with STMf before invoking onStart
    const auto statF = server.readPacket();
    expect(opcodeOf(statF) == "STAT", "strm s acknowledged with STAT");

    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, aude_power_event) {
    LoopbackServer server;
    std::atomic<int> lastEnable{-1};
    std::atomic<int> count{0};
    SlimProtoClient::Events events;
    events.onAude = [&](bool enable) {
        lastEnable.store(enable ? 1 : 0);
        count.fetch_add(1);
    };
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x02};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    // aude payload: enable_spdif, enable_dac.
    const unsigned char off[2] = {0, 0};
    server.sendPacket("aude", off);
    for (int i = 0; i < 100 && count.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(count.load() == 1 && lastEnable.load() == 0, "aude(0) reports power off");

    const unsigned char on[2] = {1, 1};
    server.sendPacket("aude", on);
    for (int i = 0; i < 100 && count.load() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(count.load() == 2 && lastEnable.load() == 1, "aude(1) reports power on");

    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, codc_event) {
    LoopbackServer server;
    std::atomic<int> fmt{-1};
    std::atomic<int> rateCode{-1};
    std::atomic<int> seen{0};
    SlimProtoClient::Events events;
    events.onCodc = [&](StreamFormat f, const PcmParams& pcm) {
        fmt.store(static_cast<int>(f));
        rateCode.store(pcm.sampleRateCode);
        seen.store(1);
    };
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x03};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    // codc payload: format, size, rate, channels, endianness.
    const unsigned char codc[5] = {'m', '1', '3', '2', '0'};
    server.sendPacket("codc", codc);
    for (int i = 0; i < 100 && !seen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(seen.load() == 1, "codc fired onCodc");
    expect(fmt.load() == static_cast<int>(StreamFormat::Mp3), "codc format decoded");
    expect(rateCode.load() == '3', "codc pcm params decoded");

    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, setd_rename_and_query) {
    LoopbackServer server;
    SlimProtoClient::Events events;
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x04};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    client.setPlayerName("Original");
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    // setd rename: id 0 + "Renamed" + NUL.
    const std::string rename = std::string("\0", 1) + "Renamed";
    std::vector<unsigned char> setd(rename.begin(), rename.end());
    setd.push_back('\0');
    server.sendPacket("setd", setd);
    const auto echo = server.readPacket();
    expect(opcodeOf(echo) == "SETD", "rename echoed with SETD");
    expect(std::string_view(reinterpret_cast<const char*>(echo.data() + 9)) == "Renamed",
           "echo carries the new name");

    // 5-byte query (id 0 only): the client must return the remembered name.
    const unsigned char query[1] = {0};
    server.sendPacket("setd", query);
    const auto replied = server.readPacket();
    expect(opcodeOf(replied) == "SETD", "query answered with SETD");
    expect(std::string_view(reinterpret_cast<const char*>(replied.data() + 9)) == "Renamed",
           "query returns the renamed value");

    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, dsco_framing) {
    LoopbackServer server;
    SlimProtoClient::Events events;
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x05};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    client.sendDisco(2);  // REMOTE_DISCONNECT
    const auto dsco = server.readPacket();
    expect(opcodeOf(dsco) == "DSCO", "sendDisco emits DSCO");
    const uint32_t dscoLen = (static_cast<uint32_t>(dsco[4]) << 24) |
                             (static_cast<uint32_t>(dsco[5]) << 16) |
                             (static_cast<uint32_t>(dsco[6]) << 8) | dsco[7];
    expect(dscoLen == 1, "DSCO body is one reason byte");
    expect(dsco[8] == 2, "DSCO carries the reason code");

    client.stop();
    server.expectClosedByClient();
}

SQ2_TEST(wire, flush_is_acknowledged_once) {
    LoopbackServer server;
    std::atomic<int> flushCalls{0};
    SlimProtoClient* clientPtr = nullptr;
    SlimProtoClient::Events events;
    // PlayerSession acks 'strm f' with one STMf from its onFlush handler; the
    // wire layer must not emit a second one (that was a duplicate STAT).
    events.onFlush = [&](bool) {
        flushCalls.fetch_add(1);
        clientPtr->sendStat("STMf", StreamStats{});
    };
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x07};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", std::move(events));
    clientPtr = &client;
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    server.readPacket();  // HELO

    const unsigned char flush = 'f';
    server.sendPacket("strm", std::span<const unsigned char>(&flush, 1));

    auto isStatf = [](const std::vector<unsigned char>& pkt) {
        return opcodeOf(pkt) == "STAT" &&
               std::string_view(reinterpret_cast<const char*>(pkt.data() + 8), 4) == "STMf";
    };
    // Drain for a fixed window: the duplicate (if any) follows the first STMf
    // immediately, while heartbeats (STMt) are ignored.
    int statf = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        auto pkt = server.readPacketFor(200);
        if (pkt && isStatf(*pkt)) ++statf;
    }
    expect(flushCalls.load() == 1, "onFlush fired once");
    expect(statf == 1, "strm f yields exactly one STMf");

    client.stop();
}

SQ2_TEST(wire, server_silence_watchdog_reconnects) {
    LoopbackServer server;
    const std::array<unsigned char, 6> mac{0xaa, 0, 0, 0, 0, 0x06};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", SlimProtoClient::Events{});
    client.setServerTimeout(300);
    client.start("127.0.0.1", server.port());
    server.acceptConnection();
    expect(opcodeOf(server.readPacket()) == "HELO", "first connection HELO");

    // Send nothing: the client must declare the connection dead and reconnect.
    server.acceptReconnect();
    expect(opcodeOf(server.readPacket()) == "HELO", "reconnect sends a fresh HELO");

    client.stop();
    server.expectClosedByClient();
}