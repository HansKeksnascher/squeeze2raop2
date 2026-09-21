#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace squeeze2raop2 {

struct MdnsRecord {
    std::string type;
    std::string instance;
    std::string host;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

class MdnsBrowser {
public:
    enum class RecordEvent : std::uint8_t { Added, Removed };

    using RecordCallback = std::function<void(const MdnsRecord&, RecordEvent)>;

    // ctor/dtor are out-of-line: with unique_ptr<Impl>, a defaulted ctor in
    // the header would instantiate ~unique_ptr<Impl> on an incomplete type.
    MdnsBrowser();
    ~MdnsBrowser();
    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;

    bool start(const std::string& ifaceName, RecordCallback cb, std::string& errorOut);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace squeeze2raop2
