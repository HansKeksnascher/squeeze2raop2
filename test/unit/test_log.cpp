// Pins the log line format: [LVL][local-time+offset][AREA]: message.

#include "common/log.h"

#include "check.h"

#include <ctime>
#include <string>

using namespace squeeze2raop2::test;
using squeeze2raop2::log::Area;
using squeeze2raop2::log::Level;

SQ2_TEST(log, level_tags) {
    expect(squeeze2raop2::log::levelTag(Level::Off) == "OFF", "off tag");
    expect(squeeze2raop2::log::levelTag(Level::Error) == "ERR", "error tag");
    expect(squeeze2raop2::log::levelTag(Level::Warn) == "WRN", "warn tag");
    expect(squeeze2raop2::log::levelTag(Level::Info) == "INF", "info tag");
    expect(squeeze2raop2::log::levelTag(Level::Debug) == "DBG", "debug tag");
}

SQ2_TEST(log, area_tags) {
    expect(squeeze2raop2::log::areaTag(Area::App) == "APP", "app area");
    expect(squeeze2raop2::log::areaTag(Area::Lms) == "LMS", "lms area");
    expect(squeeze2raop2::log::areaTag(Area::Mdns) == "MDNS", "mdns area");
    expect(squeeze2raop2::log::areaTag(Area::Ap) == "AP", "ap area");
    expect(squeeze2raop2::log::areaTag(Area::Dec) == "DEC", "dec area");
    expect(squeeze2raop2::log::areaTag(Area::Pb) == "PB", "pb area");
    expect(squeeze2raop2::log::areaTag(Area::Ses) == "SES", "ses area");
}

SQ2_TEST(log, stamp_and_line) {
    std::tm tm{};
    tm.tm_hour = 19;
    tm.tm_min = 4;
    tm.tm_sec = 6;
    expect(squeeze2raop2::log::formatStamp(tm, 162, 7200) == "[19:04:06.162+02:00]",
           "positive offset");
    expect(squeeze2raop2::log::formatStamp(tm, 5, -18000) == "[19:04:06.005-05:00]",
           "negative offset");
    expect(squeeze2raop2::log::formatStamp(tm, 0, 0) == "[19:04:06.000+00:00]", "utc offset");

    expect(squeeze2raop2::log::formatLine(Level::Info, Area::Ap, "[19:04:06.162+02:00]",
                                          "session launching: Kueche") ==
               "[INF][19:04:06.162+02:00][AP]: session launching: Kueche",
           "full line assembly");
    expect(squeeze2raop2::log::formatLine(Level::Error, Area::Ses, "[00:00:00.001+00:00]",
                                          "boom") == "[ERR][00:00:00.001+00:00][SES]: boom",
           "error line");
}