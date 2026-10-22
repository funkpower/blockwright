#include "read_file.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path writeTemp(const std::string& name, std::size_t bytes) {
    const fs::path dir = BW_TEST_TMP;
    fs::create_directories(dir);
    const fs::path p = dir / name;
    std::ofstream(p, std::ios::binary | std::ios::trunc) << std::string(bytes, 'x');
    return p;
}

// The message readFile throws, or "" if it returned.
std::string failure(const fs::path& p, std::size_t maxSize, std::string* out = nullptr) {
    try {
        const std::string data = bwcli::readFile(p.string(), maxSize);
        if (out)
            *out = data;
        return "";
    } catch (const bwcli::Failure& f) {
        return f.message;
    }
}

} // namespace

TEST_CASE("readFile refuses input over its size limit", "[cli]") {
    const fs::path p = writeTemp("hundred.ans", 100);
    std::string got = "untouched";
    CHECK(failure(p, 99, &got) == p.string() + ": input too large");
    CHECK(got == "untouched");

    CHECK(failure(p, 100, &got).empty());
    CHECK(got == std::string(100, 'x'));
}

TEST_CASE("readFile checks the limit on every chunk, not just the first", "[cli]") {
    // Three full 64 KiB reads and a short one
    const std::size_t size = 3 * 65536 + 5;
    const fs::path p = writeTemp("chunks.ans", size);
    std::string got = "untouched";
    CHECK(failure(p, 2 * 65536 + 10, &got) == p.string() + ": input too large");
    CHECK(failure(p, size - 1, &got) == p.string() + ": input too large");
    CHECK(got == "untouched");

    CHECK(failure(p, size, &got).empty());
    CHECK(got.size() == size);
}

TEST_CASE("readFile accepts an empty file under a zero limit", "[cli]") {
    const fs::path p = writeTemp("empty.ans", 0);
    std::string got = "untouched";
    CHECK(failure(p, 0, &got).empty());
    CHECK(got.empty());
}
