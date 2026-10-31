// Running a generator on Windows (generator.h): the program found on PATH as
// an .exe, in the generator's files or by a full path; its stdin, stdout,
// stderr and status; a timeout, a flood and a crash; and what it starts
// ending with the run. bw-gen-helper.exe (gen_helper_win.cpp), built beside
// this program, stands in for sh.

#include "blockwright/generator.h"
#include "blockwright/project.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QByteArray>
#include <QtGlobal>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>

using namespace bw;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;
namespace gen = bw::generator;

namespace {

using Files = std::map<std::string, std::string>;

const gen::RunOptions quick{std::chrono::milliseconds(10'000)};

project::Project dotsProject() {
    Canvas c(4, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            c.at(x, y) = Cell{U'.', Color::indexed(7), Color::indexed(0), 0, false};
    AnsiDocument doc;
    doc.canvas = c;
    doc.ice = false;
    return project::fromAnsi(doc);
}

fs::path helperDir() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    REQUIRE((n > 0 && n < MAX_PATH));
    return fs::path(self).parent_path();
}

fs::path helper() { return helperDir() / "bw-gen-helper.exe"; }

std::string bytesOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string json(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

// generator.json running `command` (each word a JSON string), plus `extra`
Files spec(const std::vector<std::string>& command, const std::string& extra = "") {
    std::string words;
    for (const std::string& w : command)
        words += (words.empty() ? "" : ", ") + json(w);
    return {{"generator.json", "{\"command\": [" + words + "]" + extra + "}"}};
}

// The helper by its full path, with these arguments.
Files helperRun(std::vector<std::string> args, const std::string& extra = "") {
    args.insert(args.begin(), helper().string());
    return spec(args, extra);
}

// PATH holds the helper's directory for as long as it lives.
struct HelperOnPath {
    QByteArray old = qgetenv("PATH");
    HelperOnPath() { qputenv("PATH", QByteArray::fromStdString(helperDir().string()) + ";" + old); }
    ~HelperOnPath() { qputenv("PATH", old); }
};

} // namespace

TEST_CASE("Windows: a generator's stdout makes the layer's cells, its stdin its input", "[generator][run][windows]") {
    project::Project p = dotsProject();
    Files files = helperRun({"cat", "out.ans"}, R"(, "key": 1)");
    files["out.ans"] = "\x1b[0;44m  \x1b[31mAB";
    const LayerId id = gen::add(p, files, "sky", quick);
    const Layer& l = *p.layers.find(id);
    CHECK(l.width() == 4);
    CHECK(l.cells.at(0, 0).transparent);
    CHECK(l.cells.at(2, 0).ch == U'A');

    // the canvas and params reach it; what it writes stays in the run's copy
    Files in = helperRun({"input", R"("word":"ok")", "data/word.txt"});
    in["params.json"] = R"({"word": "ok"})";
    in["data/word.txt"] = "WORD";
    const LayerId w = gen::add(p, in, "word", quick);
    CHECK(p.layers.find(w)->cells.at(3, 0).ch == U'D');
    p.files[gen::directory(w) + "params.json"] = R"({"word": "no"})";
    CHECK_THROWS_WITH(gen::refresh(p, w, quick),
                      ContainsSubstring("exited with status 3") && ContainsSubstring("bad input:"));

    Files touch = helperRun({"touch", "data/word.txt", "changed"});
    touch["data/word.txt"] = "WORD";
    const LayerId t = gen::add(p, touch, "touch", quick);
    CHECK(p.files.at(gen::directory(t) + "data/word.txt") == "WORD");
}

TEST_CASE("Windows: a generator's program is found on PATH, in its files or by a full path",
          "[generator][run][windows]") {
    const project::Project p = dotsProject();
    {
        const HelperOnPath onPath;
        for (const char* name : {"bw-gen-helper", "bw-gen-helper.exe", "BW-GEN-HELPER"}) {
            INFO(name);
            project::Project q = p;
            const LayerId id = gen::add(q, spec({name, "touch", "x.txt", "ok"}), "g", quick);
            CHECK(q.layers.find(id)->cells.at(1, 0).ch == U'k');
        }
    }
    // not on PATH now
    {
        project::Project q = p;
        CHECK_THROWS_WITH(gen::add(q, spec({"bw-gen-helper", "touch", "x", "ok"}), "g", quick),
                          ContainsSubstring("could not run bw-gen-helper: no such program"));
        CHECK(q == p);
    }
    // the generator's own copy, by a relative path
    const std::string exe = bytesOf(helper());
    REQUIRE(exe.size() > 1000);
    for (const char* program : {"./bin/gen.exe", "bin/gen.exe"}) {
        INFO(program);
        project::Project q = p;
        Files files = spec({program, "touch", "x.txt", "ok"});
        files["bin/gen.exe"] = exe;
        const LayerId id = gen::add(q, files, "g", quick);
        CHECK(q.layers.find(id)->cells.at(0, 0).ch == U'o');
    }
    // a project's file names use '/' wherever it is opened
    for (const char* bad : {"../gen.exe", "..\\gen.exe", "bin\\gen.exe", ".\\bin\\gen.exe", "c:gen.exe",
                            "a/.hidden/x.exe"}) {
        INFO(bad);
        project::Project q = p;
        CHECK_THROWS_WITH(gen::add(q, spec({bad}), "g", quick), ContainsSubstring("is not a file in scripts/"));
        CHECK(q == p);
    }
}

TEST_CASE("Windows: a generator that fails, crashes, hangs or floods leaves the project as it was",
          "[generator][run][windows]") {
    const project::Project p = dotsProject();
    const auto addFails = [&](const std::vector<std::string>& args, const std::string& why,
                              gen::RunOptions opts = quick) {
        INFO(args.front());
        project::Project q = p;
        CHECK_THROWS_WITH(gen::add(q, helperRun(args), "g", opts), ContainsSubstring(why));
        CHECK(q == p);
    };
    addFails({"fail", "2", "oops"}, "exited with status 2:\noops");
    addFails({"fail", "1"}, "exited with status 1");
    addFails({"crash"}, "crashed");
    {
        project::Project q = p;
        try {
            gen::add(q, helperRun({"noise", "400", "5"}), "g", quick);
            FAIL("no throw");
        } catch (const gen::GeneratorError& e) {
            const std::string what = e.what();
            CHECK(what.find("status 5:\nline ") != std::string::npos);
            CHECK(what.ends_with("line 399"));
            CHECK(what.find("line 0\n") == std::string::npos);
            CHECK(what.size() < 2600);
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    addFails({"sleep", "30000"}, "took longer than 300 ms; stopped", gen::RunOptions{std::chrono::milliseconds(300)});
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    addFails({"flood", std::to_string(gen::kMaxOutputBytes + 100'000)},
             "wrote more than " + std::to_string(gen::kMaxOutputBytes) + " bytes");
}

TEST_CASE("Windows: what a generator starts ends with the run", "[generator][run][process][windows]") {
    const fs::path dir = fs::path(BW_TEST_TMP) / "generator-win" / "spawn";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path marker = fs::absolute(dir / "leaked");
    project::Project p = dotsProject();
    const LayerId id = gen::add(p, helperRun({"spawn", marker.string()}), "g", quick);
    CHECK(p.layers.find(id)->cells.at(0, 0).ch == U'o');
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK_FALSE(fs::exists(marker));
}
