// Pins the ICY request rewriting (header insertion positions) and the
// StreamTitle extraction.

#include "icy_meta.h"

#include "check.h"

#include <cstdio>
#include <string>

using namespace sq2t;
using squeeze2raop2::parseStreamTitle;
using squeeze2raop2::withIcyRequestHeader;

int main() {
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

    expect(parseStreamTitle("StreamTitle='Hello';") == std::string("Hello"), "title parsed");
    expect(parseStreamTitle("x StreamTitle='A B' y") == std::string("A B"), "title with spaces");
    expect(!parseStreamTitle("StreamTitle='';").has_value(), "empty title");
    expect(!parseStreamTitle("no title here").has_value(), "missing key");
    expect(!parseStreamTitle("StreamTitle='unterminated").has_value(), "unterminated title");

    std::printf("ok\n");
    return 0;
}
