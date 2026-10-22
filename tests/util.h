#pragma once

#include "blockwright/ansi.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace bwtest {

inline std::filesystem::path dataDir() { return BW_TEST_DATA; }

inline std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parses raw ANSI with no SAUCE at the given width.
inline bw::AnsiDocument parse(std::string_view ansi, int width = 80) {
    bw::ParseOptions o;
    o.defaultWidth = width;
    return bw::parseAnsi(ansi, o);
}

} // namespace bwtest
