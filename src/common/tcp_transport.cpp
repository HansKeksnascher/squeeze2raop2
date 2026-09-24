#include "common/tcp_transport.h"

#include "common/util.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace squeeze2raop2 {

TcpTransport::~TcpTransport() { close(); }

int TcpTransport::fd() const {
    std::lock_guard<std::mutex> lock(fdMutex_);
    return fd_.get();
}

void TcpTransport::setError(std::string message) { error_ = std::move(message); }

bool TcpTransport::connect(const std::string& host, uint16_t port, std::string& error) {
    close();
    const int raw = connectSocketTuned(host, port, error);
    if (raw < 0) return false;
    {
        std::lock_guard<std::mutex> lock(fdMutex_);
        fd_.reset(raw);
    }
    error_.clear();
    return true;
}

bool TcpTransport::writeAll(std::span<const char> data, std::string& error) {
    const int sock = fd();
    if (sock < 0) {
        error = "not connected";
        return false;
    }
    if (!sendAll(sock, data.data(), data.size())) {
        error = "send failed";
        return false;
    }
    return true;
}

TcpTransport::Read TcpTransport::read(std::span<char> buffer, uint32_t timeoutMs) {
    const int sock = fd();
    if (sock < 0) {
        setError("not connected");
        return {Result::Error, 0};
    }
    pollfd pfd{sock, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (pr < 0) {
        if (errno == EINTR) return {Result::Timeout, 0};
        setError(std::string("poll: ") + errnoMessage(errno));
        return {Result::Error, 0};
    }
    if (pr == 0) return {Result::Timeout, 0};

    const ssize_t n = ::recv(sock, buffer.data(), buffer.size(), 0);
    if (n > 0) return {Result::Data, static_cast<size_t>(n)};
    if (n == 0) return {Result::Eof, 0};
    if (errno == EAGAIN || errno == EINTR) return {Result::Timeout, 0};
    setError(std::string("recv: ") + errnoMessage(errno));
    return {Result::Error, 0};
}

void TcpTransport::interrupt() {
    std::lock_guard<std::mutex> lock(fdMutex_);
    fd_.shutdown();
}

void TcpTransport::close() {
    std::lock_guard<std::mutex> lock(fdMutex_);
    fd_.reset();
}

}  // namespace squeeze2raop2