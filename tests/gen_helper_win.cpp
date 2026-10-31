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

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

} // namespace

int main(int argc, char** argv) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
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
