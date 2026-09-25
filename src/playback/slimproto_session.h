#pragma once

#include "airplay/airplay_output.h"
#include "app/config.h"
#include "lms/slimproto.h"
#include "playback/playback_stream.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

// The LMS control connection for one player: owns the SlimProtoClient, builds
// the HELO caps, owns the player display name, and turns the client's reader
// thread events into Delegate calls. It carries no playback policy — the
// Delegate (a PlayerSession) decides what each event means and calls back into
// stat()/disco()/resp()/meta() to report.
class SlimProtoSession {
public:
    struct Delegate {
        virtual void onStreamStart(const StrmStart& st) = 0;
        virtual void onCont() = 0;
        virtual void onStop() = 0;
        virtual void onFlush() = 0;
        virtual void onPause(uint32_t ms) = 0;
        virtual void onUnpause(uint32_t ms) = 0;
        virtual void onSkipAhead(uint32_t ms) = 0;
        virtual void onCodc(StreamFormat format, const PcmParams& pcm) = 0;
        virtual void onAude(bool enable) = 0;
        virtual void onLmsVolume(double l, double r) = 0;
        virtual ~Delegate() = default;
    };

    SlimProtoSession(const ResolvedPlayerConfig& cfg, const GlobalConfig& global,
                     AirplayOutput& output, NameSink nameSink, Delegate& delegate);
    ~SlimProtoSession();
    SlimProtoSession(const SlimProtoSession&) = delete;
    SlimProtoSession& operator=(const SlimProtoSession&) = delete;

    // The STAT snapshot provider (used for periodic STMt and every stat()).
    void setStatsProvider(std::function<StreamStats()> provider);
    void start();
    void stop();

    // LMS sends.
    void stat(const char (&event)[5]);
    void resp(std::string_view header);
    void disco(DisconnectCode code);
    void meta(std::string_view block);
    void button(uint32_t code);
    const std::string& serverHost() const;
    const std::string& name() const { return name_; }
    // True once the client exists (start() called): the chaser's linkAlive.
    bool alive() const { return client_ != nullptr; }

private:
    std::string buildCaps() const;

    const std::array<uint8_t, 6> mac_;
    const std::optional<std::string> lmsHost_;
    const uint16_t lmsPort_;
    const uint32_t serverTimeoutMs_;
    std::string name_;
    const NameSink nameSink_;
    AirplayOutput& output_;
    Delegate& delegate_;
    std::function<StreamStats()> statsProvider_;
    std::unique_ptr<SlimProtoClient> client_;
};

}  // namespace squeeze2raop2