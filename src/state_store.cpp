#include "state_store.h"

#include "log.h"
#include "util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace squeeze2raop2 {

namespace {

std::array<uint8_t, 6> uniqueMacFor(std::map<std::string, StateStoreEntry>& entries,
                                    const std::string& deviceId) {
    std::array<uint8_t, 6> candidate = fakeMacFor(deviceId);
    for (int tries = 0; tries < 256; ++tries) {
        bool conflict = false;
        for (const auto& kv : entries) {
            if (kv.first != deviceId && kv.second.hasMac && kv.second.mac == candidate) {
                conflict = true;
                break;
            }
        }
        if (!conflict) return candidate;
        candidate[5] = static_cast<uint8_t>((candidate[5] + 1) & 0xFF);
    }
    return fakeMacFor(deviceId);
}

}  // namespace

std::string encodeId(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (c == ' ')
            out.append("%20");
        else if (c == '\n' || c == '\r')
            out.push_back(' ');
        else
            out.push_back(c);
    }
    return out;
}

bool StateStore::open(const std::string& path, std::string& errorOut) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t pos = path.find_last_of('/');
        if (pos != std::string::npos) (void)mkdir(path.substr(0, pos).c_str(), 0755);
        path_ = path;
    }
    return load(errorOut);
}

bool StateStore::load(std::string& errorOut) {
    (void)errorOut;  // load failures are logged, never fatal
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(path_);
    if (!in) {
        save();
        return true;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty() || line.starts_with('#')) continue;
        std::istringstream iss(line);
        std::string tag, id;
        if (!(iss >> tag >> id)) {
            log::warn("state file {}: odd line {}", path_, lineNo);
            continue;
        }
        StateStoreEntry& entry = entries_[urlDecode(id)];
        if (tag == "mac") {
            std::string macText;
            iss >> macText;
            std::array<uint8_t, 6> parsed{};
            if (macFromString(macText, parsed)) {
                entry.mac = parsed;
                entry.hasMac = true;
            } else {
                log::warn("state file {}: bad mac for {}", path_, id);
            }
        } else if (tag == "creds") {
            std::string rest;
            std::getline(iss, rest);
            size_t start = rest.find_first_not_of(' ');
            if (start != std::string::npos) entry.creds = rest.substr(start);
        } else {
            log::warn("state file {}: unknown tag {}", path_, tag);
        }
    }
    return true;
}

std::array<uint8_t, 6> StateStore::macFor(const std::string& deviceId, bool& newlyAssigned) {
    std::lock_guard<std::mutex> lock(mutex_);
    newlyAssigned = false;
    StateStoreEntry& entry = entries_[deviceId];
    if (!entry.hasMac) {
        entry.mac = uniqueMacFor(entries_, deviceId);
        entry.hasMac = true;
        newlyAssigned = true;
        save();
    }
    return entry.mac;
}

std::optional<std::string> StateStore::credsFor(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(deviceId);
    if (it == entries_.end() || it->second.creds.empty()) return std::nullopt;
    return it->second.creds;
}

void StateStore::saveCreds(const std::string& deviceId, const std::string& credsJson) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[deviceId].creds = credsJson;
    save();
}

bool StateStore::save() {
    // Write-then-rename: a crash mid-save must never truncate the real
    // state file (lost MACs would re-register the players on LMS).
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            log::error("cannot write state file {}", tmp);
            return false;
        }
        out << "# squeeze2raop2 state: stable virtual MACs, AirPlay pairing credentials\n";
        for (const auto& kv : entries_) {
            if (kv.second.hasMac)
                out << "mac  " << encodeId(kv.first) << " " << macToString(kv.second.mac) << "\n";
            if (!kv.second.creds.empty())
                out << "creds  " << encodeId(kv.first) << " " << kv.second.creds << "\n";
        }
        out.flush();
        if (!out) {
            log::error("cannot write state file {}", tmp);
            out.close();
            ::remove(tmp.c_str());
            return false;
        }
    }
    // Durability: push the temp file's data to disk before the rename so a
    // crash cannot leave a truncated state file. The directory entry itself is
    // not fsync'd; losing the rename is acceptable (worst case: the entry is
    // gone, never corrupt).
    if (const int fd = ::open(tmp.c_str(), O_RDONLY); fd >= 0) {
        (void)::fsync(fd);
        (void)::close(fd);
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        log::error("cannot replace state file {}: {}", path_, errnoMessage(errno));
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace squeeze2raop2
