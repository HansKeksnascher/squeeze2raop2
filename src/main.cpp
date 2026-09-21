#include "config.h"
#include "log.h"

#include <csignal>

namespace squeeze2raop2 {
void runBridge(const Settings& settings);
}  // namespace squeeze2raop2

int main(int argc, char** argv) {
    using namespace squeeze2raop2;
    int exitCode = 0;
    auto settings = parseCommandLine(argc, argv, exitCode);
    if (!settings) return exitCode;

    log::setLevel(settings->logLevel);
    log::info("squeeze2raop2 starting v0.1.0-m1");

    // SA_RESTART matches glibc's signal() default; poll/select still return
    // EINTR (they are never restarted), which the read loops handle.
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGPIPE, &sa, nullptr);

    runBridge(*settings);
    log::info("bye");
    return 0;
}
