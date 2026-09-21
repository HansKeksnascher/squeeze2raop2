#pragma once

#include "slimproto.h"

#include <cstdio>
#include <string>

namespace sq2 {

class PcmFileSink {
public:
    explicit PcmFileSink(std::string path);
    ~PcmFileSink();

    bool open(const PcmFormat& format, std::string& errorOut);
    void feed(const char* data, size_t len, const PcmFormat& format);
    void close();

    uint64_t bytesTotal() const { return total_; }

private:
    std::string path_;
    FILE* fp_ = nullptr;
    PcmFormat format_;
    uint64_t total_ = 0;
    bool headerWritten_ = false;
};

}
