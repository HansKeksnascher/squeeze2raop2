#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace sq2 {

struct MdnsRecord {
    std::string type;
    std::string instance;
    std::string host;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

class MdnsBrowser {
public:
    enum class RecordEvent { Added, Removed };

    using RecordCallback = std::function<void(const MdnsRecord&, RecordEvent)>;

    MdnsBrowser() = default;
    ~MdnsBrowser();
    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;

    bool start(const std::string& ifaceName, RecordCallback cb, std::string& errorOut);
    void stop();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}
