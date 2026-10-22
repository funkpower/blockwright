#include "read_file.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace bwcli {

std::string readFile(const std::string& path, std::size_t maxSize) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        throw Failure{path + ": is a directory"};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Failure{path + ": " + std::strerror(errno)};
    // Not `ss << in.rdbuf()`: that swallows a failed allocation and returns
    // the part read so far.
    std::string data;
    char buf[1 << 16];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n > maxSize - data.size())
            throw Failure{path + ": input too large"};
        data.append(buf, n);
    }
    if (in.bad())
        throw Failure{path + ": read failed"};
    return data;
}

} // namespace bwcli
