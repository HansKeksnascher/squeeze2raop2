// Receiver-originated AP2 event command decoding: a HomePod/Sonos volume
// change arrives as a `POST /command` binary plist on the session event
// channel. RaopSender::onEventReadyRead_ decrypts it and hands the body to
// fxchain::parseRemoteVolumeCommand; these cases pin the shapes it accepts.

#include "raop_sender.h"

#include "airplay_crypto.h"
#include "check.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>

using namespace squeeze2raop2::test;

namespace {

namespace bp = fxchain::airplay::bplist;

bp::Value object(std::initializer_list<std::pair<std::string, bp::Value>> items) {
    return bp::Value::object(bp::Dict(items));
}

std::optional<double> parse(const bp::Value& root) {
    const fxchain::airplay::Bytes body = bp::encode(root);
    return fxchain::parseRemoteVolumeCommand(std::span<const uint8_t>(body));
}

}  // namespace

SQ2_TEST(remote_event, top_level_volume) {
    // shairport-sync's receiver form (its "dvlc" notification).
    const auto root = object({{"type", bp::Value::str("sendMediaRemoteCommand")},
                              {"value", bp::Value::str("dvlc")},
                              {"volume", bp::Value::real(0.42)}});
    const auto v = parse(root);
    expect(v.has_value(), "top-level volume is recognised");
    expect(v && std::abs(*v - 0.42) < 1e-9, "volume value is preserved");
}

SQ2_TEST(remote_event, params_volume) {
    const auto root = object({{"type", bp::Value::str("sendMediaRemoteCommand")},
                              {"params", object({{"volume", bp::Value::real(0.75)}})}});
    const auto v = parse(root);
    expect(v.has_value(), "volume nested under params is recognised");
    expect(v && std::abs(*v - 0.75) < 1e-9, "nested volume value is preserved");
}

SQ2_TEST(remote_event, integer_volume) {
    const auto root = object(
        {{"type", bp::Value::str("sendMediaRemoteCommand")}, {"volume", bp::Value::integer(1)}});
    const auto v = parse(root);
    expect(v.has_value(), "integer volume is recognised");
    expect(v && std::abs(*v - 1.0) < 1e-9, "integer volume converts to double");
}

SQ2_TEST(remote_event, no_volume_is_null) {
    // A transport command (owntone's HomePod button vocabulary) carries no
    // volume and must not be mistaken for one.
    const auto button = object(
        {{"type", bp::Value::str("sendMediaRemoteCommand")}, {"value", bp::Value::str("paus")}});
    expect(!parse(button).has_value(), "transport command has no volume");

    const auto info = object({{"type", bp::Value::str("updateInfo")}});
    expect(!parse(info).has_value(), "updateInfo has no volume");
}

SQ2_TEST(remote_event, malformed_is_null) {
    const fxchain::airplay::Bytes empty;
    expect(!fxchain::parseRemoteVolumeCommand(std::span<const uint8_t>(empty)).has_value(),
           "empty body is rejected");

    const std::string garbage = "not a plist at all";
    expect(!fxchain::parseRemoteVolumeCommand(
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(garbage.data()),
                                         garbage.size()))
                .has_value(),
           "garbage is rejected");
}