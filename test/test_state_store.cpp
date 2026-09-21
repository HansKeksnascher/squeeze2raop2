#include "state_store.h"

#include "check.h"
#include "util.h"

#include <unistd.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

using namespace sq2t;
using squeeze2raop2::StateStore;

namespace {

// Unique scratch directory per process so parallel/overlapping runs are safe.
class ScratchDir {
public:
    ScratchDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("squeeze2raop2_state_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() { std::filesystem::remove_all(path_); }

    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

}  // namespace

static void testFreshStoreAssignsMac(const ScratchDir& dir) {
    StateStore store;
    std::string error;
    expect(store.open(dir.file("state.conf"), error), "open fresh store");
    expect(error.empty(), "no error on fresh open");

    bool newlyAssigned = false;
    auto mac = store.macFor("dev1", newlyAssigned);
    expect(newlyAssigned, "first macFor assigns");
    expect(mac[0] == 0xaa && mac[5] == 0x01, "assigned mac uses aa:..:01 prefix");

    bool again = true;
    auto mac2 = store.macFor("dev1", again);
    expect(!again, "second macFor does not reassign");
    expect(mac2 == mac, "mac is stable within one store");

    auto other = store.macFor("dev2", newlyAssigned);
    expect(other != mac, "different ids get different macs");
}

static void testCredsRoundTrip(const ScratchDir& dir) {
    StateStore store;
    std::string error;
    expect(store.open(dir.file("state.conf"), error), "reopen for creds");

    const std::string creds = R"({"authToken":"abc","key":"<json>&\"chars\""})";
    store.saveCreds("dev1", creds);
    expect(store.credsFor("dev1").value_or("") == creds, "creds round-trip in memory");
    expect(!store.credsFor("unknown").has_value(), "unknown id has no creds");
}

static void testPersistenceAcrossInstances(const ScratchDir& dir) {
    StateStore first;
    std::string error;
    expect(first.open(dir.file("state.conf"), error), "open first instance");
    bool newlyAssigned = false;
    const auto mac = first.macFor("persist-dev", newlyAssigned);
    expect(newlyAssigned, "assign in first instance");
    first.saveCreds("persist-dev", "creds-json");

    StateStore second;
    expect(second.open(dir.file("state.conf"), error), "open second instance");
    bool reassigned = true;
    const auto mac2 = second.macFor("persist-dev", reassigned);
    expect(!reassigned, "mac not reassigned after reload");
    expect(mac2 == mac, "mac persists across instances");
    expect(second.credsFor("persist-dev").value_or("") == "creds-json",
           "creds persist across instances");

    // ids with spaces survive encode/decode (%20)
    bool spaceAssigned = false;
    second.macFor("dev with space", spaceAssigned);
    expect(spaceAssigned, "space id assigned");
    second.saveCreds("dev with space", "spaced");

    StateStore third;
    expect(third.open(dir.file("state.conf"), error), "open third instance");
    expect(third.credsFor("dev with space").value_or("") == "spaced",
           "space-containing id round-trips through the file");
    bool spaceReassigned = true;
    third.macFor("dev with space", spaceReassigned);
    expect(!spaceReassigned, "space id mac stable after reload");
}

static void testMalformedLinesTolerated(const ScratchDir& dir) {
    const std::string path = dir.file("broken.conf");
    {
        StateStore store;
        std::string error;
        expect(store.open(path, error), "create broken store");
        bool assigned = false;
        store.macFor("good-dev", assigned);
        expect(assigned, "assign mac in broken store");
    }
    {
        // append garbage: junk lines, unknown tags, truncated macs, bad hex
        std::FILE* f = std::fopen(path.c_str(), "a");
        expect(f != nullptr, "append to state file");
        std::fputs("\n", f);
        std::fputs("# comment line\n", f);
        std::fputs("junkline\n", f);
        std::fputs("unknown-tag some-id payload\n", f);
        std::fputs("mac\n", f);
        std::fputs("mac some-id zz:11:22\n", f);
        std::fputs("mac good-dev aa:01:02:03:04:05\n", f);
        std::fclose(f);
    }
    StateStore store;
    std::string error;
    expect(store.open(path, error), "reopen store with malformed lines");
    bool reassigned = true;
    const auto mac = store.macFor("good-dev", reassigned);
    expect(!reassigned, "valid mac entry still loads despite garbage");
    expect(mac[0] == 0xaa, "loaded mac matches the file entry");
}

int main() {
    ScratchDir dir;
    testFreshStoreAssignsMac(dir);
    testCredsRoundTrip(dir);
    testPersistenceAcrossInstances(dir);
    testMalformedLinesTolerated(dir);
    std::printf("ok\n");
    return 0;
}
