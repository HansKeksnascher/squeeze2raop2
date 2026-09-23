#include "app/config.h"
#include "app/persistence.h"
#include "app/version.h"
#include "common/log.h"

#include <signal.h>

#include <cstdio>

namespace squeeze2raop2 {
void runBridge(const Settings& settings, Persistence& persistence);
}  // namespace squeeze2raop2

int main(int argc, char** argv) {
    using namespace squeeze2raop2;
    int exitCode = 0;
    auto args = parseArgs(argc, argv, exitCode);
    if (!args) return exitCode;

    Persistence persistence;
    Settings settings;
    std::string error;
    if (!persistence.open(args->configPath, settings, error)) {
        std::fprintf(stderr, "squeeze2raop2: %s\n", error.c_str());
        return 1;
    }

    log::setLevel(settings.global.logLevel);
    log::info(log::Area::App, "squeeze2raop2 starting {} (config {})", SQUEEZE2RAOP2_VERSION,
              persistence.path());

    // SA_RESTART matches glibc's signal() default; poll/select still return
    // EINTR (they are never restarted), which the read loops handle.
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGPIPE, &sa, nullptr);

    runBridge(settings, persistence);
    log::info(log::Area::App, "bye");
    return 0;
}