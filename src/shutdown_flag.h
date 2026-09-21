#pragma once

#include <atomic>
#include <csignal>

namespace squeeze2raop2 {

// Flip-flops to false on SIGINT/SIGTERM; polled by every run/stream loop so
// shutdown propagates without dedicated teardown signaling. Namespace-scope
// inline constinit variable: constant-initialized (and guaranteed so), the
// signal handler touches no runtime-initialized state and stays
// async-signal-safe.
inline constinit std::atomic<bool> g_run{true};
static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler must be async-signal-safe");

namespace detail {
inline void onShutdownSignal(int) { g_run.store(false); }
}  // namespace detail

// SIGINT/SIGTERM flip g_run, the flag the run loops actually poll (a
// main.cpp handler used to set a separate anonymous-namespace flag nobody
// read, so the process ignored SIGTERM). SA_RESTART matches glibc's
// signal() default; poll/select still return EINTR (they are never
// restarted), which the read loops handle.
inline void installShutdownSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = detail::onShutdownSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace squeeze2raop2
