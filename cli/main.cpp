// blockwright-cli: headless access to the core.
//
//   blockwright-cli --version
//   blockwright-cli info <file.ans>
//   blockwright-cli convert [--allow-lossy] <in.ans> <out.ans>
//
// Exit status: 0 ok, 1 bad input, an I/O failure or a lossy convert without
// --allow-lossy (nothing is written), 2 usage error.

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/version.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSaveFile>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr int kOk = 0, kBadInput = 1, kUsage = 2;

struct Failure {
    std::string message;
};

std::string readFile(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        throw Failure{path + ": is a directory"};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Failure{path + ": " + std::strerror(errno)};
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw Failure{path + ": read failed"};
    return ss.str();
}

bw::AnsiDocument load(const std::string& path) {
    const std::string data = readFile(path);
    if (data.empty())
        throw Failure{path + ": empty file"};
    try {
        return bw::parseAnsi(data);
    } catch (const bw::AnsiError& e) {
        throw Failure{path + ": " + e.what()};
    }
}

std::string text(const std::string& cp437) { return bw::cp437ToUtf8(cp437); }

const char* spacingName(int v) {
    switch (v) {
    case 1: return "8 px";
    case 2: return "9 px";
    case 0: return "not set";
    default: return "invalid (3)";
    }
}

const char* aspectName(int v) {
    switch (v) {
    case 1: return "legacy";
    case 2: return "square";
    case 0: return "not set";
    default: return "invalid (3)";
    }
}

int info(const std::string& path) {
    const bw::AnsiDocument doc = load(path);
    const std::string data = readFile(path);
    const auto content = bw::splitSauce(data).content;
    std::cout << "file:        " << path << "\n"
              << "size:        " << doc.canvas.width() << "x" << doc.canvas.height() << "\n"
              << "ice:         " << (doc.ice ? "on" : "off") << "\n";
    if (!doc.sauce) {
        std::cout << "sauce:       none\n";
        return kOk;
    }
    const bw::Sauce& s = *doc.sauce;
    std::cout << "sauce:       yes\n"
              << "title:       " << text(s.title) << "\n"
              << "author:      " << text(s.author) << "\n"
              << "group:       " << text(s.group) << "\n"
              << "date:        " << s.date << "\n"
              << "filesize:    " << s.fileSize;
    if (s.fileSize == content.size())
        std::cout << " (matches content)\n";
    else
        std::cout << " (content is " << content.size() << " bytes)\n";
    std::cout << "datatype:    " << int(s.dataType) << "\n"
              << "filetype:    " << int(s.fileType) << "\n"
              << "tinfo:       " << s.tinfo1 << " " << s.tinfo2 << " " << s.tinfo3 << " " << s.tinfo4 << "\n"
              << "flags:       0x" << std::hex << int(s.flags) << std::dec << " (iCE " << (s.ice() ? "on" : "off")
              << ", letter spacing " << spacingName(s.letterSpacing()) << ", aspect " << aspectName(s.aspectRatio())
              << ")\n"
              << "font:        " << s.font << "\n"
              << "comments:    " << s.comments.size() << "\n";
    for (const auto& c : s.comments)
        std::cout << "  " << text(c) << "\n";
    return kOk;
}

int convert(const std::string& in, const std::string& out, bool allowLossy) {
    const bw::AnsiDocument doc = load(in);
    const bw::AnsiOutput res = bw::writeAnsi(doc);
    // One warning per kind of loss, with a count.
    if (const size_t n = res.unwritable.size()) {
        const bw::Unwritable& u = res.unwritable.front();
        std::string glyph;
        bw::appendUtf8(glyph, u.ch);
        std::cerr << "blockwright-cli: warning: " << in << ": " << n
                  << (n == 1 ? " glyph has" : " glyphs have") << " no CP437 byte and "
                  << (n == 1 ? "is" : "are") << " written as '?' (first: U+" << std::hex << std::uppercase
                  << unsigned(u.ch) << std::dec << " '" << glyph << "' at column " << u.x + 1 << ", row " << u.y + 1
                  << ")\n";
    }
    if (const int n = res.blinkDropped)
        std::cerr << "blockwright-cli: warning: " << in << ": " << n
                  << (n == 1 ? " blinking cell loses its blink" : " blinking cells lose their blink")
                  << ": bright backgrounds need iCE colours, where blink is a bright background\n";
    if (res.lossy() && !allowLossy)
        throw Failure{out + ": not written: the conversion loses data (--allow-lossy writes it anyway)"};
    // QSaveFile writes a temporary file beside the destination, syncs it and
    // renames it over the destination on commit, or removes it on failure, so
    // a failed write never leaves a partial file or damages the input.
    QSaveFile f(QString::fromStdString(out));
    if (!f.open(QIODevice::WriteOnly))
        throw Failure{out + ": " + f.errorString().toStdString()};
    const auto n = static_cast<qint64>(res.bytes.size());
    errno = 0;
    if (f.write(res.bytes.data(), n) != n || !f.flush()) {
        // Qt's error string for a failed write is often "Unknown error"
        const std::string why = errno ? std::strerror(errno) : f.errorString().toStdString();
        f.cancelWriting();
        throw Failure{out + ": write failed: " + why};
    }
    if (!f.commit())
        throw Failure{out + ": write failed: " + f.errorString().toStdString()};
    return kOk;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("blockwright-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Headless Blockwright.\n\n"
        "Commands:\n"
        "  info <file>          dimensions and SAUCE fields\n"
        "  convert <in> <out>   read an .ans and write it back out"));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();
    const QCommandLineOption lossyOpt(QStringLiteral("allow-lossy"),
                                      QStringLiteral("convert: write even if something can't be represented"));
    parser.addOption(lossyOpt);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("info or convert"));
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("command arguments"), QStringLiteral("[args...]"));
    if (!parser.parse(QCoreApplication::arguments())) {
        std::cerr << "blockwright-cli: " << parser.errorText().toStdString() << "\n";
        return kUsage;
    }
    if (parser.isSet(versionOpt)) {
        std::cout << "blockwright-cli " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n";
        return kOk;
    }
    if (parser.isSet(helpOpt))
        parser.showHelp(kOk);

    const QStringList args = parser.positionalArguments();
    auto usage = [](const char* msg) {
        std::cerr << "blockwright-cli: " << msg << "\n"
                  << "usage: blockwright-cli info <file> | convert [--allow-lossy] <in> <out> | --version\n";
        return kUsage;
    };
    if (args.isEmpty())
        return usage("no command given");
    const QString cmd = args.first();
    try {
        if (cmd == QLatin1String("info")) {
            if (args.size() != 2)
                return usage("info takes one file");
            return info(args[1].toStdString());
        }
        if (cmd == QLatin1String("convert")) {
            if (args.size() != 3)
                return usage("convert takes an input and an output file");
            return convert(args[1].toStdString(), args[2].toStdString(), parser.isSet(lossyOpt));
        }
        return usage(("unknown command '" + cmd.toStdString() + "'").c_str());
    } catch (const Failure& f) {
        std::cerr << "blockwright-cli: " << f.message << "\n";
        return kBadInput;
    }
}
