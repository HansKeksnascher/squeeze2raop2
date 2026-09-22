// Transport-level regression tests for the remediation in net_util/slimproto
// and lms_stream:
//   - UniqueFd / SlimProtoClient: a stream thread hammering sendStat() while
//     the client reconnects must be race-free (run under ThreadSanitizer).
//   - sendStat() after stop() must be a harmless no-op, not a crash.
//   - HttpStreamReader::interrupt() must unblock a header read in progress.
//
// These use loopback sockets only; no external services.

#include "lms_stream.h"
#include "slimproto.h"

#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace sq2t;
using squeeze2raop2::HttpStreamReader;
using squeeze2raop2::SlimProtoClient;
using squeeze2raop2::StreamStats;

namespace {

// Minimal loopback TCP listener that accepts repeatedly (each accepted socket
// is handed to the caller).
class LoopbackListener {
public:
    LoopbackListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        expect(fd_ >= 0, "listener socket");
        int one = 1;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        expect(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        expect(::listen(fd_, 8) == 0, "listen");
        socklen_t len = sizeof(addr);
        expect(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        port_ = ntohs(addr.sin_port);
    }
    ~LoopbackListener() {
        if (fd_ >= 0) ::close(fd_);
    }

    uint16_t port() const { return port_; }

    // Accept with a deadline; -1 on timeout.
    int acceptOne(int timeoutMs) {
        pollfd pfd{fd_, POLLIN, 0};
        if (::poll(&pfd, 1, timeoutMs) != 1) return -1;
        return ::accept(fd_, nullptr, nullptr);
    }

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

void testConcurrentSendDuringReconnect() {
    LoopbackListener listener;
    std::atomic<bool> run{true};

    // Accept and immediately drop each connection, forcing the client into a
    // reconnect loop while another thread keeps sending control packets.
    std::thread acceptor([&] {
        while (run.load()) {
            const int conn = listener.acceptOne(100);
            if (conn < 0) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ::shutdown(conn, SHUT_RDWR);
            ::close(conn);
        }
    });

    const std::array<uint8_t, 6> mac{0xaa, 0, 0, 0, 0, 0x02};
    SlimProtoClient client(mac, "Model=squeezelite,mp3,pcm", SlimProtoClient::Events{});
    client.start("127.0.0.1", listener.port());

    std::thread sender([&] {
        for (int i = 0; i < 300 && run.load(); ++i) {
            client.sendStat("STMt", StreamStats{});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    run.store(false);
    sender.join();
    client.stop();
    acceptor.join();

    // sendStat() after stop() must be a safe no-op (socket already released).
    client.sendStat("STMt", StreamStats{});
    expect(true, "concurrent send during reconnect completed");
}

void testHttpHeaderInterrupt() {
    LoopbackListener listener;
    // Server accepts but never replies, so openBlocking parks in poll().
    std::thread acceptor([&] {
        const int conn = listener.acceptOne(5000);
        expect(conn >= 0, "http server accepted");
        // Hold the connection open (do not send a response) until the reader
        // is interrupted.
        pollfd pfd{conn, POLLIN, 0};
        (void)::poll(&pfd, 1, 2000);
        ::close(conn);
    });

    HttpStreamReader reader;
    std::atomic<bool> ok{true};
    std::thread readerThread([&] {
        std::string error;
        const bool opened =
            reader.openBlocking("127.0.0.1", listener.port(), "GET / HTTP/1.0\r\n\r\n", error);
        ok.store(opened);
    });

    // Let openBlocking connect, send, and enter the header poll.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    reader.interrupt();
    readerThread.join();
    acceptor.join();

    expect(!ok.load(), "openBlocking returns false after interrupt");
}

}  // namespace

int main() {
    testConcurrentSendDuringReconnect();
    testHttpHeaderInterrupt();
    std::printf("ok\n");
    return 0;
}