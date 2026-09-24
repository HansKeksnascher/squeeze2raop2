// Pins the sender lifecycle without a dedicated pump thread: prepare/launch
// build and start the sender, the caller's thread drives it (pump/pumpUntil),
// a receiver failure surfaces as lost(), and the between-tracks keep-alive
// driver (park/unpark) starts, runs and joins cleanly on repeated rounds.

#include "airplay/airplay_output.h"

#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

namespace {

// A bound-but-not-listening TCP port: the kernel refuses every connect
// immediately, so the sender fails fast without needing a real receiver. The
// returned fd must stay open for the port to keep refusing.
int bindRefusedPort(uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

}  // namespace

SQ2_TEST(airplay_output, lifecycle_without_receiver) {
    uint16_t port = 0;
    const int listener = bindRefusedPort(port);
    require(listener >= 0, "could bind a refused loopback port");

    RaopTarget target;
    target.host = "127.0.0.1";
    target.port = port;
    target.airplay2 = false;  // classic RAOP: no HAP pairing needed to reach TCP

    AirplayOutput output("test", "AABBCCDDEEFF", target, CredentialSink{}, 50);

    expect(output.prepare(44100), "prepare builds a live sender");
    expect(output.hasPlayer(), "a player exists after prepare");
    expect(output.state() == AirplayOutput::State::Prepared, "prepared, not launched");

    output.launch(50.0);
    expect(output.state() == AirplayOutput::State::Running, "running after launch");

    // Drive the sender from this thread until the refused connect is reported.
    // The sender notifies from inside pump(), so one pass is enough, but loop
    // to stay robust to scheduling.
    for (int i = 0; i < 500 && !output.lost(); ++i) output.pump(std::chrono::milliseconds(5));
    expect(output.lost(), "a refused receiver is reported as lost");
    expect(output.consumeLost(), "lost() is a one-shot flag");
    expect(!output.consumeLost(), "the flag stays cleared");

    // The pumpUntil pace wait services the sender and returns by the deadline.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    output.pumpUntil(deadline);
    expect(std::chrono::steady_clock::now() >= deadline, "pumpUntil honours the deadline");

    // The between-tracks keep-alive driver must start and join cleanly, and be
    // safe to repeat and to start on a dead session.
    output.park();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    output.unpark();
    output.park();
    output.unpark();

    // Teardown joins the driver and drops the player; a fresh prepare is safe.
    output.stop(false);
    expect(!output.hasPlayer(), "stop drops the player");
    expect(output.state() == AirplayOutput::State::Absent, "absent after stop");
    expect(output.prepare(44100), "can prepare again after teardown");
    output.stop(false);

    ::close(listener);
}

SQ2_TEST(airplay_output, pump_without_target) {
    // No target: prepare() is a no-op, but the pacer's pumpUntil must still
    // block until the deadline (otherwise realtime pacing would stop working
    // for the WAV-sink path).
    AirplayOutput output("test", "AABBCCDDEEFF", std::nullopt, CredentialSink{}, 50);
    expect(!output.prepare(44100), "prepare fails without a target");
    expect(!output.hasPlayer(), "no player without a target");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
    output.pumpUntil(deadline);
    expect(std::chrono::steady_clock::now() >= deadline, "pumpUntil still blocks without a sender");

    output.pump(std::chrono::milliseconds(0));  // no-op, must not crash
    output.park();                              // no-op
    output.unpark();                            // no-op
    output.stop(false);
}