#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <string>

namespace squeeze2raop2 {

// IPv4 -> dotted-quad text; empty string when inet_ntop fails.
std::string ipv4ToString(const in_addr& addr);

// Same for an address carried as a host-order uint32 (the value unpackN()
// produces from a wire field): the integer's most significant octet is the
// first dotted-quad component.
std::string ipv4ToString(uint32_t hostOrder);

// Resolve (literal or via getaddrinfo) + connect an AF_INET SOCK_STREAM
// socket with SO_KEEPALIVE. Returns -1 with errorOut set on failure.
int connectTcp(const std::string& host, uint16_t port, std::string& errorOut);

// Writes all bytes, retrying transient shortfalls (EAGAIN/EINTR/ENOBUFS);
// returns false on a real send error. MSG_NOSIGNAL so a dead peer cannot
// kill the process with SIGPIPE.
bool sendAll(int fd, const void* data, size_t len);

} // namespace squeeze2raop2
