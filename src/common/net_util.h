#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <string>

namespace squeeze2raop2 {

// IPv4 -> dotted-quad text; empty string when inet_ntop fails.
std::string ipv4ToString(const in_addr& addr);

// Same for an address carried as a host-order uint32 (the value readInt()
// produces from a wire field): the integer's most significant octet is the
// first dotted-quad component.
std::string ipv4ToString(uint32_t hostOrder);

// Sole-owner RAII wrapper for a POSIX file descriptor. Movable, non-copyable.
//
// `shutdown()` exists so an owner can unblock a peer thread parked in
// send()/recv()/poll() without closing the descriptor. POSIX permits
// shutdown() concurrently with I/O on the same socket; close() does not have
// that guarantee, which is why a socket shared across threads is held by
// std::shared_ptr (see SlimProtoClient) and never reset() in place.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    // Relinquish ownership; the caller becomes responsible for the fd.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // Adopt `fd`, closing the current one if present. Closing is attempted
    // exactly once: on Linux a close() that reports EINTR has still closed the
    // descriptor, and retrying could close a descriptor another thread has
    // already reused.
    void reset(int fd = -1) noexcept;

    // SHUT_RDWR, retrying only on EINTR. Safe to call while another thread is
    // blocked in send()/recv() on the same socket.
    void shutdown() noexcept;

private:
    int fd_ = -1;
};

// Resolve (literal or via getaddrinfo) + connect an AF_INET SOCK_STREAM
// socket with SO_KEEPALIVE. Returns -1 with errorOut set on failure.
int connectTcp(const std::string& host, uint16_t port, std::string& errorOut);

// Best-effort keepalive tuning for a connected stream socket: SO_KEEPALIVE plus
// tightened idle/interval/count, so a peer that has vanished without an RST is
// detected instead of leaving a read parked forever. No-op on fd < 0; safe to
// call after connectTcp (which already sets SO_KEEPALIVE).
void enableTcpKeepalive(int fd);

// Writes all bytes, retrying transient shortfalls (EAGAIN/EINTR/ENOBUFS);
// returns false on a real send error. MSG_NOSIGNAL so a dead peer cannot
// kill the process with SIGPIPE.
bool sendAll(int fd, const void* data, size_t len);

}  // namespace squeeze2raop2
