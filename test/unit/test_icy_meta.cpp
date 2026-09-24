// Pins the ICY request rewriting (header insertion positions) and the
// StreamTitle extraction.

#include "lms/icy_meta.h"

#include "check.h"

#include <string>

using namespace squeeze2raop2::test;
using squeeze2raop2::parseIcyMetaint;
using squeeze2raop2::parseStreamTitle;
using squeeze2raop2::withIcyRequestHeader;

SQ2_TEST(icy, request_header) {
    // Already carries the header: unchanged.
    {
        const std::string req = "GET /x HTTP/1.0\r\nIcy-MetaData: 1\r\n\r\n";
        expect(withIcyRequestHeader(req) == req, "already has icy header");
    }
    // Full header block: inserted before the blank line.
    {
        const std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n\r\n";
        const std::string out = withIcyRequestHeader(req);
        expect(out.find("Host: h\r\nIcy-MetaData: 1\r\n\r\n") != std::string::npos,
               "icy inserted before the blank line");
    }
    // No blank line but a trailing CRLF: append.
    {
        const std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n";
        expect(withIcyRequestHeader(req) == req + "Icy-MetaData: 1\r\n",
               "appended after a trailing CRLF");
    }
    // No blank line, no CRLF: terminate the line, then append.
    {
        const std::string req = "GET /x HTTP/1.0";
        expect(withIcyRequestHeader(req) == req + "\r\nIcy-MetaData: 1\r\n",
               "terminated then appended");
    }
}

SQ2_TEST(icy, metaint) {
    expect(parseIcyMetaint("icy-metaint:8192\r\n") == 8192, "plain value");
    expect(parseIcyMetaint("ICY-METAINT: 4096\r\n") == 4096, "case-insensitive name");
    expect(parseIcyMetaint("Icy-MetaInt:\t 16000\r\n") == 16000, "leading whitespace");
    expect(parseIcyMetaint("Host: h\r\n\r\n") == 0, "absent header");
    expect(parseIcyMetaint("icy-metaint: 0\r\n") == 0, "zero interval");
    expect(parseIcyMetaint("icy-metaint: 99999999\r\n") == 0, "absurd interval");
    expect(parseIcyMetaint("icy-metaint: abc\r\n") == 0, "non-numeric value");
}

SQ2_TEST(icy, stream_title) {
    expect(parseStreamTitle("StreamTitle='Hello';") == std::string("Hello"), "title parsed");
    expect(parseStreamTitle("x StreamTitle='A B' y") == std::string("A B"), "title with spaces");
    expect(!parseStreamTitle("StreamTitle='';").has_value(), "empty title");
    expect(!parseStreamTitle("no title here").has_value(), "missing key");
    expect(!parseStreamTitle("StreamTitle='unterminated").has_value(), "unterminated title");
}