// HttpStreamReader regression tests (lms/lms_stream.cpp): interrupt() must
// unblock a header read parked in poll(), so a stalled source cannot hold the
// stream thread (and thus shutdown) for the read timeout.
//
// Loopback sockets only; no external services.

#include "lms/lms_stream.h"

#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace squeeze2raop2::test;
using squeeze2raop2::HttpStreamReader;
using squeeze2raop2::requestHost;

namespace {

// Minimal loopback TCP listener that accepts one connection.
class LoopbackListener {
public:
    LoopbackListener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd_ >= 0, "listener socket");
        int one = 1;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        require(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        require(::listen(fd_, 8) == 0, "listen");
        socklen_t len = sizeof(addr);
        require(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
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

}  // namespace

// requestHost feeds the TLS SNI/verification path, so the port/IPv6/whitespace
// handling must be exact.
SQ2_TEST(lms_stream, request_host) {
    expect(requestHost("GET / HTTP/1.0\r\nHost: example.com:8000\r\n\r\n") == "example.com",
           "strips the port");
    expect(requestHost("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n") == "example.com", "no port");
    expect(requestHost("GET / HTTP/1.0\r\nhost:  Station.NET \r\n\r\n") == "Station.NET",
           "case-insensitive header, leading spaces trimmed, host case preserved");
    expect(requestHost("GET / HTTP/1.0\r\nHost: [2001:db8::1]:9000\r\n\r\n") == "2001:db8::1",
           "IPv6 bracket literal strips brackets and port");
    expect(requestHost("GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n") == "127.0.0.1", "IPv4");
    expect(requestHost("GET / HTTP/1.0\r\nAccept: */*\r\n\r\n").empty(),
           "absent Host header yields empty");
}

SQ2_TEST(lms_stream, http_header_interrupt) {
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

// The ICY de-interleaver strips in-band metadata (length byte + N*16 bytes)
// from the audio at the icy-metaint interval and forwards each block to the
// callback. A bug here (wrong *16, off-by-one countdown, mishandled boundary)
// corrupts or leaks metadata into the decoded audio; only the pure helpers in
// icy_meta.h were previously tested.
SQ2_TEST(lms_stream, icy_deinterleave) {
    LoopbackListener listener;
    std::thread server([&] {
        const int conn = listener.acceptOne(5000);
        if (conn < 0) return;
        std::string meta(16, '\0');
        const std::string_view title = "StreamTitle='x'";
        for (size_t i = 0; i < title.size(); ++i) meta[i] = title[i];

        std::string resp = "HTTP/1.0 200 OK\r\nicy-metaint: 4\r\n\r\n";
        resp += "AAAA";
        resp += static_cast<char>(1);  // metadata length = 1 * 16 bytes
        resp += meta;
        resp += "BBBB";
        resp += static_cast<char>(0);  // empty block: no callback
        resp += "CCCC";
        (void)::send(conn, resp.data(), resp.size(), 0);
        ::shutdown(conn, SHUT_WR);
        ::close(conn);
    });

    HttpStreamReader reader;
    std::vector<std::string> metas;
    reader.setMetaCallback([&](std::string_view block) { metas.emplace_back(block); });

    std::string error;
    require(reader.openBlocking("127.0.0.1", listener.port(),
                                "GET / HTTP/1.0\r\nHost: station\r\n\r\n", error),
            "icy stream opened");

    // A deliberately tiny buffer forces reads to split at the 4-byte audio
    // boundaries and across the metadata block.
    std::string audio;
    char buf[3];
    for (int i = 0; i < 1000; ++i) {
        const auto r = reader.read(std::span{buf, sizeof buf}, 200);
        if (r.result == HttpStreamReader::ReadResult::Data) {
            audio.append(buf, r.bytes);
        } else if (r.result == HttpStreamReader::ReadResult::Timeout) {
            continue;
        } else {
            break;  // AtEof / Closed
        }
    }
    reader.close();
    server.join();

    expect(audio == "AAAABBBBCCCC", "audio extracted with metadata stripped");
    require(metas.size() == 1, "the non-empty block is delivered exactly once");
    expect(metas[0].substr(0, 11) == "StreamTitle", "block forwarded verbatim");
}
