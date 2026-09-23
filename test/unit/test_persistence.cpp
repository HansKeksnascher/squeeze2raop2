#include "app/persistence.h"

#include "app/config.h"
#include "check.h"
#include "common/util.h"

#include <unistd.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace squeeze2raop2::test;
using squeeze2raop2::Persistence;
using squeeze2raop2::ResolvedPlayerConfig;
using squeeze2raop2::Settings;

namespace {

class ScratchDir {
public:
    explicit ScratchDir(const char* tag) {
        path_ = std::filesystem::temp_directory_path() /
                ("squeeze2raop2_persist_test_" + std::to_string(::getpid()) + "_" + tag);
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() { std::filesystem::remove_all(path_); }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

const ResolvedPlayerConfig* findByKey(const Settings& s, const std::string& key) {
    for (const auto& p : s.players)
        if (p.key == key) return &p;
    return nullptr;
}

}  // namespace

SQ2_TEST(persistence, fresh_creates_template) {
    ScratchDir dir("fresh");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(dir.file("fresh.conf"), s, error), "open fresh config");
    expect(error.empty(), "no error on fresh open");
    expect(s.global.discovery, "discovery defaults on");
    expect(s.global.autoRegister, "auto-register defaults on");
    expect(s.global.lmsPort == 3483, "lms port default");
    expect(s.players.empty(), "no players in template");
    const std::string text = readFile(dir.file("fresh.conf"));
    expect(text.find("[global]") != std::string::npos, "template has [global]");
    expect(text.find("[default]") != std::string::npos, "template has [default]");
}

SQ2_TEST(persistence, parse_and_inheritance) {
    ScratchDir dir("parsed");
    const std::string path = dir.file("parsed.conf");
    writeFile(path,
              "# top comment\n"
              "[global]\n"
              "lms = 192.168.1.10:3499\n"
              "discovery = off\n"
              "auto-register = off\n"
              "log = debug\n"
              "\n"
              "[default]\n"
              "volume = fixed\n"
              "volume-pct = 40\n"
              "latency-ms = 1000\n"
              "pace = fast\n"
              "\n"
              "[player \"Kueche15\"]\n"
              "id = 542a1b5cc9e2\n"
              "volume-pct = 55\n"
              "target = 192.168.1.157:7000\n");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open parsed config");
    expect(!s.global.discovery, "discovery off parsed");
    expect(!s.global.autoRegister, "auto-register off parsed");
    expect(s.global.lmsHost.value_or("") == "192.168.1.10", "lms host parsed");
    expect(s.global.lmsPort == 3499, "lms port parsed");
    expect(s.players.size() == 1, "one player");
    const auto& r = s.players.front();
    expect(r.id == "542a1b5cc9e2", "id parsed");
    expect(r.key == "542a1b5cc9e2", "key uses id");
    expect(r.volumeMode == squeeze2raop2::VolumeMode::Fixed, "inherits volume mode");
    expect(r.volPct == 55.0f, "overrides volume-pct");
    expect(r.latencyMs == 1000, "inherits latency");
    expect(!r.paceRealtime, "inherits pace fast");
    expect(r.target.has_value() && r.target->first == "192.168.1.157", "target parsed");
}

SQ2_TEST(persistence, user_edits_preserved) {
    ScratchDir dir("edits");
    const std::string path = dir.file("edits.conf");
    writeFile(path,
              "# keep me\n"
              "[global]\n"
              "log = info\n"
              "\n"
              "[player \"Kitchen\"]\n"
              "# player comment\n"
              "sink = /tmp/k.wav\n"
              "unknown-key = 42\n");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open edits config");
    auto resolved = p.resolve("", "Kitchen", false);
    expect(resolved.has_value(), "match by name");
    expect(resolved->sinkPath.value_or("") == "/tmp/k.wav", "sink parsed");

    const std::string text = readFile(path);
    expect(text.find("# keep me") != std::string::npos, "top comment preserved");
    expect(text.find("# player comment") != std::string::npos, "player comment preserved");
    expect(text.find("unknown-key = 42") != std::string::npos, "unknown key preserved");
    expect(text.find("sink = /tmp/k.wav") != std::string::npos, "user key preserved");
    expect(text.find("mac = ") != std::string::npos, "mac written back");
}

SQ2_TEST(persistence, auto_register_stable) {
    ScratchDir dir("auto");
    const std::string path = dir.file("auto.conf");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open auto config");

    auto first = p.resolve("542a1b5cc9e2", "Kueche15", true);
    expect(first.has_value(), "auto-registers discovered device");
    expect(first->autoRegistered, "marked auto");
    expect(first->key == "542a1b5cc9e2", "key is the device id");
    const auto mac = first->mac;

    auto second = p.resolve("542A1B5CC9E2", "Renamed", true);
    expect(second.has_value(), "re-resolves by id");
    expect(second->mac == mac, "mac stable");
    expect(second->name == "Kueche15", "keeps the original section name");

    const std::string text = readFile(path);
    expect(text.find("id = 542a1b5cc9e2") != std::string::npos, "id persisted");
    expect(text.find("auto = true") != std::string::npos, "auto marker persisted");

    auto disabled = p.resolve("deadbeef0000", "Other", false);
    expect(!disabled.has_value(), "no auto-register when disabled");
}

SQ2_TEST(persistence, creds_round_trip) {
    ScratchDir dir("creds");
    const std::string path = dir.file("creds.conf");
    {
        Persistence p;
        Settings s;
        std::string error;
        expect(p.open(path, s, error), "open creds config");
        auto r = p.resolve("542a1b5cc9e2", "Kueche15", true);
        expect(r.has_value(), "auto-register for creds");
        const std::string creds = R"({"authToken":"abc","key":"<json>&\"x\""})";
        p.saveCreds(r->key, creds);
        expect(p.credsFor(r->key).value_or("") == creds, "creds in memory");
    }
    Persistence p2;
    Settings s2;
    std::string error;
    expect(p2.open(path, s2, error), "reopen creds config");
    expect(p2.credsFor("542a1b5cc9e2").has_value(), "creds persisted");
}

SQ2_TEST(persistence, resolve_precedence) {
    ScratchDir dir("precedence");
    const std::string path = dir.file("precedence.conf");
    const auto macForId = squeeze2raop2::fakeMacFor("abcdefabcdef");
    writeFile(path, "[global]\nlog = info\n\n[player \"Pinned\"]\nmac = " +
                        squeeze2raop2::macToString(macForId) + "\n");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open precedence config");

    // No id on the section: the virtual-MAC rung binds the device id.
    auto byMac = p.resolve("abcdefabcdef", "Something Else", false);
    expect(byMac.has_value(), "matched by mac");
    expect(byMac->key == "Pinned", "mac match keeps the section key");
    expect(byMac->mac == macForId, "explicit mac retained");
    expect(byMac->explicitMac, "mac is explicit");
}

SQ2_TEST(persistence, static_players) {
    ScratchDir dir("static");
    const std::string path = dir.file("static.conf");
    writeFile(path,
              "[global]\ndiscovery = off\n\n"
              "[player \"Kitchen\"]\nsink = /tmp/k.wav\n\n"
              "[player \"Other\"]\nauto = true\n");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open static config");
    auto players = p.staticPlayers();
    expect(players.size() == 1, "auto sections are not static");
    expect(players.front().name == "Kitchen", "static player returned");
    expect(players.front().sinkPath.value_or("") == "/tmp/k.wav", "static config resolved");
}

SQ2_TEST(persistence, legacy_import) {
    ScratchDir dir("migrated");
    const std::string statePath = dir.file("migrated.state");
    writeFile(statePath,
              "mac  Kitchen aa:ba:87:2b:cf:01\n"
              "creds  Kitchen {\"tok\":\"x\"}\n"
              "mac  542a1b5cc9e2 aa:4b:9a:4f:dd:01\n");
    const std::string configPath = dir.file("migrated.conf");

    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(configPath, s, error), "import legacy state");
    expect(s.players.size() == 2, "two entries imported");
    const auto* named = findByKey(s, "Kitchen");
    expect(named != nullptr, "name-keyed entry imported");
    expect(named && !named->autoRegistered, "named entry is static");
    const std::array<uint8_t, 6> want{0xaa, 0xba, 0x87, 0x2b, 0xcf, 0x01};
    expect(named && named->mac == want, "named mac imported");
    const auto* hex = findByKey(s, "542a1b5cc9e2");
    expect(hex != nullptr, "hex entry imported");
    expect(hex && hex->autoRegistered, "hex entry is auto");
    expect(p.credsFor("Kitchen").value_or("") == "{\"tok\":\"x\"}", "creds imported");

    const std::string text = readFile(configPath);
    expect(text.find("[player \"Kitchen\"]") != std::string::npos,
           "legacy import wrote a player section");
}

SQ2_TEST(persistence, server_timeout_global) {
    ScratchDir dir("timeout");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(dir.file("default.conf"), s, error), "fresh open");
    expect(s.global.serverTimeoutMs == 35000, "server-timeout-ms defaults to 35000");

    const std::string path = dir.file("timeout.conf");
    writeFile(path, "[global]\nserver-timeout-ms = 1500\nlog = info\n");
    Persistence p2;
    Settings s2;
    expect(p2.open(path, s2, error), "open timeout config");
    expect(s2.global.serverTimeoutMs == 1500, "server-timeout-ms parsed");

    const std::string bad = dir.file("bad.conf");
    writeFile(bad, "[global]\nserver-timeout-ms = 5\n");
    Persistence p3;
    Settings s3;
    expect(!p3.open(bad, s3, error), "out-of-range server-timeout-ms rejected");
    expect(!error.empty(), "rejection carries an error message");
}

SQ2_TEST(persistence, rename_persisted) {
    ScratchDir dir("rename");
    const std::string path = dir.file("rename.conf");
    writeFile(path,
              "[global]\nlog = info\n\n"
              "[player \"Kitchen\"]\n"
              "id = 542a1b5cc9e2\n"
              "mac = aa:ba:87:2b:cf:01\n");
    Persistence p;
    Settings s;
    std::string error;
    expect(p.open(path, s, error), "open rename config");
    auto before = p.resolve("542a1b5cc9e2", "Kitchen", false);
    expect(before.has_value() && before->name == "Kitchen", "initial name");
    const auto mac = before->mac;

    p.savePlayerName(before->key, "Kueche15");

    auto after = p.resolve("542a1b5cc9e2", "Kitchen", false);
    expect(after.has_value() && after->name == "Kueche15", "renamed in memory");
    expect(after->key == "542a1b5cc9e2", "key stays the id");
    expect(after->mac == mac, "mac unchanged");

    // Reopen from disk: the rename must survive, header/key/mac intact.
    Persistence p2;
    Settings s2;
    expect(p2.open(path, s2, error), "reopen rename config");
    const auto* byKey = findByKey(s2, "542a1b5cc9e2");
    expect(byKey != nullptr, "section still resolved by id");
    expect(byKey && byKey->name == "Kueche15", "rename persisted");
    expect(byKey && byKey->mac == mac, "mac survived the rewrite");

    const std::string text = readFile(path);
    expect(text.find("name = Kueche15") != std::string::npos, "name key written");
    expect(text.find("[player \"Kitchen\"]") != std::string::npos, "header left unchanged");
}