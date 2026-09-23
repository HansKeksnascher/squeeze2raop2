#pragma once

// Minimal support for the hand-rolled unit runner:
//   * register a case with SQ2_TEST(suite, name) { ... }
//   * assert with expect() (records, continues) or require() (records, aborts
//     only the current case)
//   * squeeze2raop2::test::runAll() drives every registered case from one process.
//
// No external framework: header-only, stdlib + POSIX only. Failures are
// reported all together instead of exiting on the first one.
//
// expect()/require() are real functions (not macros) so their arguments may
// contain template-argument commas; the call site is captured with C++20
// std::source_location.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace squeeze2raop2::test {

// Thrown by require() to abort just the current case without killing the run.
struct TestAbort {};

inline std::atomic<int>& failureCount() {
    static std::atomic<int> n{0};
    return n;
}

inline void reportFailure(std::string_view what, const std::source_location& loc) {
    failureCount().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "    %s:%u: %.*s\n", loc.file_name(), loc.line(),
                 static_cast<int>(what.size()), what.data());
}

// Non-fatal: records the failure, execution continues.
inline void expect(bool ok, std::string_view what,
                   const std::source_location& loc = std::source_location::current()) {
    if (!ok) reportFailure(what, loc);
}

// Fatal: records the failure and unwinds the current case.
inline void require(bool ok, std::string_view what,
                    const std::source_location& loc = std::source_location::current()) {
    if (!ok) {
        reportFailure(what, loc);
        throw TestAbort{};
    }
}

// A registered case is itself the registry node, so nothing is heap-allocated
// (keeps LeakSanitizer quiet). Static initializers populate the list per TU.
struct Registrar {
    const char* suite;
    const char* name;
    void (*fn)();
    Registrar* next;

    Registrar(const char* s, const char* n, void (*f)()) : suite(s), name(n), fn(f) {
        next = registryHead();
        registryHead() = this;
    }

    static Registrar*& registryHead() {
        static Registrar* head = nullptr;
        return head;
    }
};

namespace detail {

inline bool matches(const Registrar& r, std::string_view filter) {
    if (filter.empty()) return true;
    if (filter == r.suite) return true;
    std::string full = r.suite;
    full += '.';
    full += r.name;
    return full.find(filter) != std::string::npos;
}

}  // namespace detail

// Entry point used by test/unit/test_main.cpp.
inline int runAll(int argc, char** argv) {
    std::string_view filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        } else {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(arg.size()),
                         arg.data());
            return 2;
        }
    }
    if (filter.empty()) {
        if (const char* env = std::getenv("SQ2_FILTER")) filter = env;
    }

    std::vector<Registrar*> cases;
    for (Registrar* r = Registrar::registryHead(); r != nullptr; r = r->next)
        if (detail::matches(*r, filter)) cases.push_back(r);
    std::sort(cases.begin(), cases.end(), [](const Registrar* a, const Registrar* b) {
        const int bySuite = std::strcmp(a->suite, b->suite);
        return bySuite != 0 ? bySuite < 0 : std::strcmp(a->name, b->name) < 0;
    });

    if (list) {
        for (const Registrar* r : cases) std::fprintf(stderr, "%s.%s\n", r->suite, r->name);
        return 0;
    }

    int passed = 0;
    int failed = 0;
    for (const Registrar* r : cases) {
        const int before = failureCount().load(std::memory_order_relaxed);
        try {
            r->fn();
        } catch (const TestAbort&) {
            // require() already recorded the failure.
        } catch (const std::exception& e) {
            reportFailure(e.what(), std::source_location::current());
        } catch (...) {
            reportFailure("unknown exception", std::source_location::current());
        }
        if (failureCount().load(std::memory_order_relaxed) == before) {
            ++passed;
            std::fprintf(stderr, "ok   %s.%s\n", r->suite, r->name);
        } else {
            ++failed;
            std::fprintf(stderr, "FAIL %s.%s\n", r->suite, r->name);
        }
    }
    std::fprintf(stderr, "%d passed, %d failed\n", passed, failed);
    return failed != 0 ? 1 : 0;
}

}  // namespace squeeze2raop2::test

#define SQ2_TEST(suite, name)                                               \
    static void sq2_test_##suite##_##name();                                \
    static const ::squeeze2raop2::test::Registrar sq2_reg_##suite##_##name( \
        #suite, #name, &sq2_test_##suite##_##name);                         \
    static void sq2_test_##suite##_##name()
