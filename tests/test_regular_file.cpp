// #453: every reader of a file a user names reads it through one handle
// checked by fstat (bw::readRegular), so a FIFO, device, socket or directory
// at the path is refused at once, naming what it is, and never waited on.

#include "blockwright/font_store.h"
#include "blockwright/library.h"
#include "blockwright/regular_file.h"
#include "blockwright/tdf.h"
#include "fifo_guard.h"
#include "read_file.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#endif

namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "regular-file" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

// "<reason> <kind>: <what>" for readRegular's refusal, or "read: <bytes>".
std::string coreRead(const fs::path& p, std::size_t limit = std::string().max_size()) {
    try {
        return "read: " + bw::readRegular(p, limit);
    } catch (const bw::RegularFileError& e) {
        using R = bw::RegularFileError::Reason;
        const char* reason = e.reason == R::Missing      ? "missing"
                             : e.reason == R::NotRegular ? "not regular"
                             : e.reason == R::TooLarge   ? "too large"
                                                         : "unreadable";
        return std::string(reason) + (e.kind.empty() ? "" : " " + e.kind) + ": " + e.what();
    }
}

std::string cliRead(const fs::path& p, std::size_t limit = std::string().max_size()) {
    try {
        return "read: " + bwcli::readFile(p.string(), limit);
    } catch (const bwcli::Failure& f) {
        return f.message;
    }
}

#if defined(__unix__) || defined(__APPLE__)
// A file holding `bytes` that this process can't open: false when it still
// can (running as root), so the test skips.
bool locked(const fs::path& p, const std::string& bytes) {
    put(p, bytes);
    fs::permissions(p, fs::perms::none);
    return ::access(p.c_str(), R_OK) != 0;
}
#endif

std::string tdfRead(const fs::path& p) {
    try {
        return "read " + std::to_string(bw::readTdfBytes(p).size()) + " bytes";
    } catch (const bw::TdfError& e) {
        return e.what();
    }
}

} // namespace

TEST_CASE("readRegular reads a regular file and refuses a directory, a missing file and one over its limit",
          "[io]") {
    const fs::path dir = scratch("plain");
    put(dir / "x.ans", "hello");
    CHECK(coreRead(dir / "x.ans") == "read: hello");
    CHECK(coreRead(dir / "x.ans", 5) == "read: hello");
    CHECK(coreRead(dir / "x.ans", 4) == "too large: is larger than 4 bytes");
    CHECK(coreRead(dir / "missing.ans") == "missing: doesn't exist");
    CHECK(coreRead(dir / "x.ans" / "under") == "missing: doesn't exist");
    fs::create_directory(dir / "d.ans");
    CHECK(coreRead(dir / "d.ans") == "not regular a directory: is a directory, not a regular file");
#if defined(__unix__) || defined(__APPLE__)
    // through a link, as before
    fs::create_symlink("x.ans", dir / "link.ans");
    CHECK(coreRead(dir / "link.ans") == "read: hello");
#endif
}

TEST_CASE("readRegular refuses a FIFO, a device and a socket without blocking", "[io]") {
#if defined(__unix__) || defined(__APPLE__)
    const fs::path dir = scratch("special");
    const fs::path fifo = dir / "pipe.ans";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(bwtest::within(fifo, [&] { return coreRead(fifo); }) == "not regular a FIFO: is a FIFO, not a regular file");
    CHECK(coreRead("/dev/null") == "not regular a device: is a device, not a regular file");

    const fs::path socket = dir / "sock.ans";
    sockaddr_un address{};
    if (socket.native().size() < sizeof address.sun_path) {
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket.c_str(), socket.native().size());
        const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(s >= 0);
        const int bound = ::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        ::close(s);
        REQUIRE(bound == 0);
        CHECK(coreRead(socket) == "not regular a socket: is a socket, not a regular file");
    } else {
        WARN("the scratch path is too long for a socket's address; not tried");
    }

    // the Library's wrapper keeps its own words
    CHECK(bwtest::within(fifo, [&] {
              try {
                  bw::library::readRegularFile(fifo);
                  return std::string("read");
              } catch (const bw::library::LibraryError& e) {
                  return std::string(e.what());
              }
          }) == "is not a regular file");
#else
    SKIP("no FIFOs on Windows");
#endif
}

TEST_CASE("the CLI's readFile refuses a FIFO or a directory promptly and still reads a file", "[cli][io]") {
    const fs::path dir = scratch("cli");
    put(dir / "x.ans", "hello");
    CHECK(cliRead(dir / "x.ans") == "read: hello");
    CHECK(cliRead(dir / "x.ans", 4) == (dir / "x.ans").string() + ": input too large");
    fs::create_directory(dir / "d.ans");
    CHECK(cliRead(dir / "d.ans") == (dir / "d.ans").string() + ": is a directory, not a regular file");
    CHECK(cliRead(dir / "nope.ans").rfind((dir / "nope.ans").string() + ": No such file", 0) == 0);
#if defined(__unix__) || defined(__APPLE__)
    const fs::path fifo = dir / "pipe.ans";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(bwtest::within(fifo, [&] { return cliRead(fifo); }) == fifo.string() + ": is a FIFO, not a regular file");
    // a steps file is read the same way
    const fs::path steps = dir / "steps.json";
    REQUIRE(::mkfifo(steps.c_str(), 0600) == 0);
    CHECK(bwtest::within(steps, [&] { return cliRead(steps, 16 << 20); }) ==
          steps.string() + ": is a FIFO, not a regular file");
#endif
}

TEST_CASE("a TheDraw font at a FIFO or a directory is refused promptly, and a font file still reads",
          "[tdf][io]") {
    const fs::path dir = scratch("tdf");
    const std::string font = bwtest::readFile(bwtest::dataDir() / "tdf" / "bw-block.tdf");
    put(dir / "block.tdf", font);
    CHECK(tdfRead(dir / "block.tdf") == "read " + std::to_string(font.size()) + " bytes");
    fs::create_directory(dir / "d.tdf");
    CHECK(tdfRead(dir / "d.tdf") == (dir / "d.tdf").string() + " is a directory, not a regular file");
    CHECK(tdfRead(dir / "nope.tdf") == "can't read " + (dir / "nope.tdf").string() + ": No such file or directory");
    put(dir / "big.tdf", std::string(std::size_t(bw::kMaxTdfBytes) + 1, '\0'));
    CHECK(tdfRead(dir / "big.tdf") == (dir / "big.tdf").string() + " is over 4 MiB");
#if defined(__unix__) || defined(__APPLE__)
    const fs::path fifo = dir / "pipe.tdf";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(bwtest::within(fifo, [&] { return tdfRead(fifo); }) == fifo.string() + " is a FIFO, not a regular file");
    CHECK(bwtest::within(fifo, [&] {
              try {
                  bw::readTdfFile(fifo);
                  return std::string("read");
              } catch (const bw::TdfError& e) {
                  return std::string(e.what());
              }
          }) == fifo.string() + " is a FIFO, not a regular file");
#endif
}

// A guard for the walk's regular-file filter (font_store.cpp), which drops
// the FIFO before it is read; readTdfBytes's own refusal of one is the test
// above.
TEST_CASE("a fonts folder holding a FIFO named like a font is scanned without blocking", "[tdf][fonts][io]") {
#if defined(__unix__) || defined(__APPLE__)
    const fs::path dir = scratch("folder");
    put(dir / "block.tdf", bwtest::readFile(bwtest::dataDir() / "tdf" / "bw-block.tdf"));
    const fs::path fifo = dir / "pipe.tdf";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(bwtest::within(fifo, [&] {
              const bw::FontCatalogue c = bw::scanFontsFolder(dir);
              std::string out;
              for (const auto& f : c.fonts)
                  out += f.displayName + ";";
              return out;
          }) == "BW Block (block.tdf);");
#else
    SKIP("no FIFOs on Windows");
#endif
}

TEST_CASE("a file that can't be opened is refused in the system's words, as before #453", "[cli][tdf][io]") {
#if defined(__unix__) || defined(__APPLE__)
    const fs::path dir = scratch("locked");
    if (!locked(dir / "x.ans", "hello"))
        SKIP("this process can read a file with no permissions (root)");
    CHECK(coreRead(dir / "x.ans") == "unreadable: can't be read: Permission denied");
    CHECK(cliRead(dir / "x.ans") == (dir / "x.ans").string() + ": Permission denied");
    CHECK(cliRead(dir / "nope.ans") == (dir / "nope.ans").string() + ": No such file or directory");
    REQUIRE(locked(dir / "x.tdf", "font"));
    CHECK(tdfRead(dir / "x.tdf") == "can't read " + (dir / "x.tdf").string() + ": Permission denied");
    try {
        bw::readRegular(dir / "x.ans");
        FAIL("read");
    } catch (const bw::RegularFileError& e) {
        CHECK(e.systemMessage() == "Permission denied");
    }
#else
    SKIP("no permission bits to take away on Windows");
#endif
}

TEST_CASE("readRegularHead reads at most the first bytes of a regular file and refuses what readRegular refuses",
          "[io]") {
    const fs::path dir = scratch("head");
    put(dir / "z.zip", std::string("PK\x03\x04", 4) + std::string(100000, 'x'));
    CHECK(bw::readRegularHead(dir / "z.zip", 4) == std::string("PK\x03\x04", 4));
    put(dir / "short", "PK");
    CHECK(bw::readRegularHead(dir / "short", 4) == "PK");
    CHECK(bw::readRegularHead(dir / "short", 0).empty());
    fs::create_directory(dir / "d");
    CHECK_THROWS_AS(bw::readRegularHead(dir / "d", 4), bw::RegularFileError);
    CHECK_THROWS_AS(bw::readRegularHead(dir / "nope", 4), bw::RegularFileError);
#if defined(__unix__) || defined(__APPLE__)
    const fs::path fifo = dir / "pipe";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(bwtest::within(fifo, [&] {
              try {
                  return "read: " + bw::readRegularHead(fifo, 4);
              } catch (const bw::RegularFileError& e) {
                  return std::string(e.what());
              }
          }) == "is a FIFO, not a regular file");
#endif
}
