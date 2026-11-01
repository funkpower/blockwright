// A generator for the Windows generator tests (test_generator_win.cpp), where
// there is no sh: one of
//
//   cat <file>              write the file to stdout
//   input <text> <file>     read stdin; without <text> in it, write it to
//                           stderr after "bad input: " and exit 3, otherwise
//                           cat <file>
//   touch <file> <text>     write <text> to <file>, then print "ok"
//   fail <status> [<text>]  write <text> to stderr, exit <status>
//   noise <lines> <status>  write "line N" lines to stderr, exit <status>
//   crash                   die of an access violation
//   sleep <ms>              sleep, then print "late"
//   flood <bytes>           write that many '0's to stdout
//   spawn <marker>          start "late-touch <marker>" detached from the
//                           console, print "ok" and exit at once
//   late-touch <marker>     sleep 1.5 s, then write <marker>
//
// Copied as NAME.exe beside a NAME.mode file, it stands in for a Python
// for the scene import tests (test_scene_import.cpp) and does what the
// file says:
//
//   python3   --version prints "Python 3.12.1"; -B <scene> --list-layers
//             prints a one-layer listing, -B <scene> --layer-ans=bg "ab"
//   py3       the same after a first argument -3, which it requires
//   alias     the WindowsApps alias with no Store Python: a message on
//             stderr, exit 9009
//   python2   "Python 2.7.18" on stderr, exit 0
//   hang      sleep 10 s, then exit 0 with nothing printed
//
// or for an ssh in the Send to WeeChat tests (test_app_publish.cpp):
//
//   ssh       write each argument, ended by a NUL, to NAME.args and stdin
//             to NAME.stdin, then write NAME.out to stdout and NAME.err to
//             stderr (when there are such files) and exit with the number
//             in NAME.exit (0 without one)

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <fcntl.h>
#include <io.h>

namespace {

std::string slurp(std::istream& in) { return {std::istreambuf_iterator<char>(in), {}}; }

int cat(const char* file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "no " << file << "\n";
        return 2;
    }
    std::cout << slurp(in);
    return 0;
}

void touch(const char* file, const char* text) { std::ofstream(file, std::ios::binary) << text; }

// The bytes of `p`, or nothing when there is no such file.
std::string contents(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return slurp(in);
}

// The ssh stand-in (above), its files named by `modeFile`.
int ssh(std::filesystem::path modeFile, int argc, char** argv) {
    std::string args;
    for (int i = 1; i < argc; ++i)
        (args += argv[i]) += '\0';
    std::ofstream(modeFile.replace_extension(".args"), std::ios::binary) << args;
    std::ofstream(modeFile.replace_extension(".stdin"), std::ios::binary) << slurp(std::cin);
    std::cout << contents(modeFile.replace_extension(".out")) << std::flush;
    std::cerr << contents(modeFile.replace_extension(".err")) << std::flush;
    const std::string status = contents(modeFile.replace_extension(".exit"));
    return status.empty() ? 0 : std::atoi(status.c_str());
}

// A Python or ssh stand-in (above) doing what `modeFile` says.
int standIn(const std::filesystem::path& modeFile, int argc, char** argv) {
    const std::string mode = contents(modeFile);
    if (mode == "ssh")
        return ssh(modeFile, argc, argv);
    if (mode == "alias") {
        std::cerr << "Python was not found; run without arguments to install from the Microsoft Store.\n";
        return 9009;
    }
    if (mode == "hang") {
        Sleep(10000);
        return 0;
    }
    if (mode == "python2") {
        std::cerr << "Python 2.7.18\n";
        return 0;
    }
    int i = 1;
    if (mode == "py3") {
        if (argc < 2 || std::string(argv[1]) != "-3") {
            std::cerr << "no -3\n";
            return 64;
        }
        i = 2;
    } else if (mode != "python3") {
        std::cerr << "no mode in " << modeFile.string() << "\n";
        return 64;
    }
    const int left = argc - i;
    if (left == 1 && std::string(argv[i]) == "--version") {
        std::cout << "Python 3.12.1\n";
        return 0;
    }
    if (left == 3 && std::string(argv[i]) == "-B") {
        const std::string what = argv[i + 2];
        if (what == "--list-layers") {
            std::cout << R"({"width": 2, "rows": 1, "title": "t", "layers": [{"name": "bg"}]})" << "\n";
            return 0;
        }
        if (what == "--layer-ans=bg") {
            std::cout << "ab";
            return 0;
        }
    }
    std::cerr << "unexpected arguments\n";
    return 64;
}

} // namespace

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    {
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::filesystem::path modeFile(std::wstring(self, n < MAX_PATH ? n : 0));
        modeFile.replace_extension(".mode");
        std::error_code ec;
        if (n > 0 && n < MAX_PATH && std::filesystem::is_regular_file(modeFile, ec))
            return standIn(modeFile, argc, argv);
    }
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "cat" && argc == 3)
        return cat(argv[2]);
    if (mode == "input" && argc == 4) {
        const std::string in = slurp(std::cin);
        if (in.find(argv[2]) == std::string::npos) {
            std::cerr << "bad input: " << in;
            return 3;
        }
        return cat(argv[3]);
    }
    if (mode == "touch" && argc == 4) {
        touch(argv[2], argv[3]);
        std::cout << "ok";
        return 0;
    }
    if (mode == "fail" && (argc == 3 || argc == 4)) {
        if (argc == 4)
            std::cerr << argv[3] << "\n";
        return std::atoi(argv[2]);
    }
    if (mode == "noise" && argc == 4) {
        for (int i = 0, n = std::atoi(argv[2]); i < n; ++i)
            std::cerr << "line " << i << "\n";
        return std::atoi(argv[3]);
    }
    if (mode == "crash") {
        std::fflush(nullptr);
        *static_cast<volatile int*>(nullptr) = 1;
        return 0;
    }
    if (mode == "sleep" && argc == 3) {
        Sleep(DWORD(std::atoi(argv[2])));
        std::cout << "late";
        return 0;
    }
    if (mode == "flood" && argc == 3) {
        const std::string block(65536, '0');
        for (long long left = std::atoll(argv[2]); left > 0; left -= (long long)block.size())
            std::cout.write(block.data(), std::streamsize(std::min<long long>(left, (long long)block.size())));
        return 0;
    }
    if (mode == "spawn" && argc == 3) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring marker;
        for (const char* c = argv[2]; *c; ++c)
            marker += wchar_t(static_cast<unsigned char>(*c));
        std::wstring line = L"\"" + std::wstring(self) + L"\" late-touch \"" + marker + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si,
                            &pi)) {
            std::cerr << "CreateProcessW failed: " << GetLastError() << "\n";
            return 2;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::cout << "ok";
        return 0;
    }
    if (mode == "late-touch" && argc == 3) {
        Sleep(1500);
        touch(argv[2], "late");
        return 0;
    }
    std::cerr << "usage: bw-gen-helper cat|input|touch|fail|noise|crash|sleep|flood|spawn|late-touch ...\n";
    return 64;
}
