#include "config.h"
#include "log.h"

#include <csignal>

namespace sq2 {
void runBridge(const Settings& settings);
}

int main(int argc, char** argv) {
    using namespace sq2;
    int exitCode = 0;
    auto settings = parseCommandLine(argc, argv, exitCode);
    if (!settings) return exitCode;

    log::setLevel(settings->logLevel);
    log::info("sqraop2 starting v0.1.0-m1");

    ::signal(SIGPIPE, SIG_IGN);

    runBridge(*settings);
    log::info("bye");
    return 0;
}
