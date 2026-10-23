// blockwright-cli: headless access to the core.
//
//   blockwright-cli --version
//   blockwright-cli info <file.ans>
//   blockwright-cli convert [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli render [--scale N] [--font-width 8|9] <in.ans> <out.png>
//   blockwright-cli export --irc [--budget N] [--allow-over-budget] <in.ans> <out.txt>
//   blockwright-cli shade --ramp NAME|c,c,... [--darken] [--steps N]
//                         [--box x0,y0,x1,y1] [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli library list [dir]
//   blockwright-cli library check <dir>
//
// Exit status: 0 ok, 1 bad input, an I/O failure or a lossy convert or shade
// without --allow-lossy (nothing is written), or a library check that found
// a problem, 2 usage error, 3 an IRC line over the byte budget without
// --allow-over-budget (nothing is written).

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/cp437.h"
#include "blockwright/credits.h"
#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/lint.h"
#include "blockwright/render.h"
#include "blockwright/version.h"
#include "read_file.h"

#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QImageWriter>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kOk = 0, kBadInput = 1, kUsage = 2, kOverBudget = 3;

using bwcli::Failure;
using bwcli::readFile;

struct Usage {
    std::string message;
};

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

// Writes `bytes` to `path` all or nothing.
void save(const std::string& out, const std::string& bytes) {
    // QSaveFile writes a temporary file beside the destination, syncs it and
    // renames it over the destination on commit, or removes it on failure, so
    // a failed write never leaves a partial file or damages the input.
    QSaveFile f(QString::fromStdString(out));
    if (!f.open(QIODevice::WriteOnly))
        throw Failure{out + ": " + f.errorString().toStdString()};
    const auto n = static_cast<qint64>(bytes.size());
    errno = 0;
    if (f.write(bytes.data(), n) != n || !f.flush()) {
        // Qt's error string for a failed write is often "Unknown error"
        const std::string why = errno ? std::strerror(errno) : f.errorString().toStdString();
        f.cancelWriting();
        throw Failure{out + ": write failed: " + why};
    }
    if (!f.commit())
        throw Failure{out + ": write failed: " + f.errorString().toStdString()};
}

// Writes the document as .ans, warning once per kind of loss; a lossy
// result is written only with allowLossy. `why` replaces the refusal's
// reason when the caller knows it.
void saveAnsi(const std::string& in, const std::string& out, const bw::AnsiDocument& doc, bool allowLossy,
              const std::string& why = {}) {
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
        throw Failure{out + ": not written: " + (why.empty() ? "the conversion loses data" : why) +
                      " (--allow-lossy writes it anyway)"};
    save(out, res.bytes);
}

int convert(const std::string& in, const std::string& out, bool allowLossy) {
    saveAnsi(in, out, load(in), allowLossy);
    return kOk;
}

// Refuses an output that is the input under another name: the same path, a
// ./ or ../ form of it, a symlink or a hard link to it.
void refuseSameFile(const std::string& in, const std::string& out) {
    namespace fs = std::filesystem;
    std::error_code ec1, ec2;
    bool same = fs::equivalent(in, out, ec1);
    if (!same) {
        const fs::path a = fs::weakly_canonical(in, ec1), b = fs::weakly_canonical(out, ec2);
        same = !ec1 && !ec2 && a == b;
    }
    if (same)
        throw Failure{out + ": not written: it is the input file"};
}

// Writes the piece as IRC text and reports every line's size in bytes, as
// the row encoder writes it. A line over the budget is listed and nothing
// is written, unless allowOver.
int exportIrc(const std::string& in, const std::string& out, std::size_t budget, bool allowOver) {
    refuseSameFile(in, out);
    const bw::AnsiDocument doc = load(in);
    const std::vector<std::string> lines = bw::irc::exportCanvas(doc.canvas);
    const auto over = bw::irc::overBudget(lines, budget);
    std::size_t longest = 0;
    std::cout << "line  bytes\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t n = lines[i].size();
        longest = std::max(longest, n);
        std::cout << std::setw(4) << i + 1 << "  " << std::setw(5) << n;
        if (n > budget)
            std::cout << "  over by " << n - budget;
        std::cout << "\n";
    }
    std::cout << lines.size() << (lines.size() == 1 ? " line" : " lines") << ", longest " << longest
              << " bytes, budget " << budget << ": " << over.size() << " over\n";
    if (!over.empty()) {
        for (const auto& o : over)
            std::cerr << "blockwright-cli: " << in << ": line " << o.line << " is " << o.bytes << " bytes, over the "
                      << budget << "-byte budget by " << o.bytes - budget << "\n";
        if (!allowOver) {
            std::cerr << "blockwright-cli: " << out
                      << ": not written: lines over the budget (--allow-over-budget writes it anyway)\n";
            return kOverBudget;
        }
    }
    save(out, bw::irc::joinLines(lines));
    return kOk;
}

// A positive integer option value, or a usage error naming the option.
int intValue(const QString& s, const char* option, int lo, int hi) {
    bool ok = false;
    const int v = s.toInt(&ok);
    if (!ok || v < lo || v > hi)
        throw Usage{std::string("--") + option + " must be " + std::to_string(lo) + "-" + std::to_string(hi) +
                    ", not '" + s.toStdString() + "'"};
    return v;
}

// A ramp by name (the named ramps) or as palette indices dark to light.
bw::Ramp rampValue(const QString& s) {
    if (const auto r = bw::findRamp(s.toStdString()))
        return *r;
    bw::Ramp r;
    r.name = s.toStdString();
    for (const QString& part : s.split(QLatin1Char(','))) {
        bool ok = false;
        const int v = part.trimmed().toInt(&ok);
        if (!ok || v < 0 || v > 15) {
            std::string names;
            for (const bw::Ramp& n : bw::namedRamps())
                names += (names.empty() ? "" : ", ") + n.name;
            throw Usage{"--ramp takes a ramp name (" + names + ") or palette indices 0-15 dark to light, not '" +
                        s.toStdString() + "'"};
        }
        r.colours.push_back(static_cast<std::uint8_t>(v));
    }
    if (const std::string why = bw::rampProblem(r); !why.empty())
        throw Usage{"--ramp " + s.toStdString() + ": " + why};
    return r;
}

// --box x0,y0,x1,y1: cells from 0, corners included.
bw::lint::Box boxValue(const QString& s) {
    const QStringList parts = s.split(QLatin1Char(','));
    int v[4] = {};
    bool ok = parts.size() == 4;
    for (int i = 0; ok && i < 4; ++i) {
        v[i] = parts[i].trimmed().toInt(&ok);
        ok = ok && v[i] >= 0;
    }
    if (!ok)
        throw Usage{"--box takes x0,y0,x1,y1 (cells from 0, corners included), not '" + s.toStdString() + "'"};
    return {std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
}

struct ShadeOptions {
    bw::Ramp ramp;
    bw::ShadeDirection direction = bw::ShadeDirection::Lighten;
    int steps = 1;
    std::optional<bw::lint::Box> box;
    bool allowLossy = false;
};

// One pass of the ramp shade brush over the box (the whole canvas without
// one), then the ladder check of the box. The check only reports; the file
// is written either way.
int shade(const std::string& in, const std::string& out, const ShadeOptions& o) {
    bw::AnsiDocument doc = load(in);
    bw::Canvas& c = doc.canvas;
    bw::lint::Box box = o.box.value_or(bw::lint::Box{0, 0, c.width() - 1, c.height() - 1});
    if (box.x0 >= c.width() || box.y0 >= c.height())
        throw Failure{in + ": the box " + std::to_string(box.x0) + "," + std::to_string(box.y0) + "," +
                      std::to_string(box.x1) + "," + std::to_string(box.y1) + " is off the " +
                      std::to_string(c.width()) + "x" + std::to_string(c.height()) + " canvas"};
    box.x1 = std::min(box.x1, c.width() - 1);
    box.y1 = std::min(box.y1, c.height() - 1);
    const bw::RampShadeBrush brush(o.ramp, o.direction, o.steps);
    bw::StrokeOptions so;
    so.selection =
        std::make_shared<const bw::Mask>(bw::Mask::rect(c.width(), c.height(), box.x0, box.y0, box.x1, box.y1));
    // A piece that was written without loss and is lossy once shaded only
    // can be because the shading made a background bright: two bright ramp
    // neighbours, which need iCE colours, where blink can't be kept.
    const bw::AnsiOutput before = bw::writeAnsi(doc);
    const bw::CellEdit edit = bw::paintRegion(c, brush, {}, so);
    edit.apply(c);
    std::string why;
    if (!before.lossy()) {
        const int n = bw::writeAnsi(doc).blinkDropped;
        if (n)
            why = "the shading needs iCE colours (a bright background), and " + std::to_string(n) +
                  (n == 1 ? " blinking cell would lose its blink" : " blinking cells would lose their blink");
    }
    saveAnsi(in, out, doc, o.allowLossy, why);

    const std::size_t n = edit.changes.size();
    std::cout << (o.direction == bw::ShadeDirection::Lighten ? "lightened " : "darkened ") << n
              << (n == 1 ? " cell " : " cells ") << o.steps << (o.steps == 1 ? " rung" : " rungs") << " along "
              << o.ramp.name << " in " << box.x0 << "," << box.y0 << "-" << box.x1 << "," << box.y1 << "\n";
    const bw::lint::LadderReport rep = bw::lint::checkLadder(c, o.ramp, box);
    char noise[32];
    std::snprintf(noise, sizeof noise, "%.3f", rep.noise);
    std::cout << "ladder: " << rep.ladderCells << " of " << rep.cells << " cells on the ramp, " << rep.shadeCells
              << " shaded, noise " << noise << ": "
              << (rep.clean() ? std::string("clean")
                            : std::to_string(rep.findings.size()) + (rep.findings.size() == 1 ? " finding" : " findings")) << "\n";
    for (const auto& f : rep.findings)
        std::cout << "  " << f.message << "\n";
    return kOk;
}

int render(const std::string& in, const std::string& out, const bw::RenderOptions& opts) {
    refuseSameFile(in, out);
    const bw::AnsiDocument doc = load(in);
    if (doc.sauce && doc.sauce->aspectRatio() == 1)
        std::cerr << "blockwright-cli: note: " << in
                  << ": SAUCE asks for the legacy (stretched) aspect ratio; the PNG has square pixels\n";
    QImage img;
    try {
        img = bw::renderDocument(doc, opts);
    } catch (const std::length_error&) {
        throw Failure{out + ": not written: the image would be too large"};
    } catch (const std::invalid_argument& e) {
        throw Failure{out + ": not written: " + e.what()};
    }
    // Encode in memory, then write the bytes as convert does: a failed write
    // then reports the system's reason, not libpng's "Unknown error".
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "png");
    if (!w.write(img))
        throw Failure{out + ": not written: PNG encoding failed: " + w.errorString().toStdString()};
    save(out, std::string(png.constData(), static_cast<size_t>(png.size())));
    return kOk;
}

std::string joined(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? sep : "") + v[i];
    return out;
}

std::filesystem::path libraryDir(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        throw Failure{dir + ": not a directory"};
    return dir;
}

// library list [dir]: one stamp a line, tab-separated: category, name,
// width x height, the widest row's IRC bytes, tags (comma-separated: a tag
// never holds a comma or a tab, and a category or name never holds a tab or
// newline), path.
// Without a dir, the bundled library and then the user library. Stamps that
// can't be read are warnings on stderr; the exit status is still 0.
int libraryList(const std::optional<std::string>& dir) {
    std::vector<std::filesystem::path> dirs;
    if (dir) {
        dirs.push_back(libraryDir(*dir));
    } else {
        // Beside the real binary, not a symlink to it.
        const auto native = [](const QString& p) { return std::filesystem::path(QFile::encodeName(p).toStdString()); };
        dirs.push_back(bw::library::bundledLibraryDir(bw::library::realBinDir(
            native(QCoreApplication::applicationFilePath()), native(QCoreApplication::applicationDirPath()))));
        dirs.push_back(bw::library::userLibraryDir());
    }
    for (const auto& d : dirs) {
        const bw::library::ScanResult r = bw::library::scan(d);
        for (const bw::library::Stamp& s : r.stamps)
            std::cout << s.meta.category << "\t" << s.displayName() << "\t" << s.art.canvas.width() << "x"
                      << s.art.canvas.height() << "\t" << s.meta.ircBytes << "\t" << joined(s.meta.tags, ",")
                      << "\t" << s.path.string() << "\n";
        for (const bw::library::Problem& p : r.problems)
            std::cerr << "warning: " << p.path.string() << ": " << p.message << "\n";
    }
    return kOk;
}

// library check <dir>: reads every stamp and sidecar under dir and reports
// each problem on stderr, plus any stamp whose widest row alone is over the
// IRC byte budget. Exit 1 if there was any.
int libraryCheck(const std::string& dir) {
    const bw::library::ScanResult r = bw::library::scan(libraryDir(dir));
    std::size_t problems = r.problems.size();
    for (const bw::library::Problem& p : r.problems)
        std::cerr << p.path.string() << ": " << p.message << "\n";
    for (const bw::library::Stamp& s : r.stamps)
        if (s.meta.ircBytes > bw::irc::kEfnetWeechatBudget) {
            std::cerr << s.path.string() << ": the widest row is " << s.meta.ircBytes << " IRC bytes, over the "
                      << bw::irc::kEfnetWeechatBudget << "-byte budget\n";
            ++problems;
        }
    std::cout << r.stamps.size() << " stamp(s), " << problems << " problem(s)\n";
    return problems ? kBadInput : kOk;
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
        "  convert <in> <out>   read an .ans and write it back out; <out> may be\n"
        "                       <in>, which is rewritten in place (export and\n"
        "                       render refuse to write over their input)\n"
        "  render <in> <out>    draw an .ans as a PNG with the IBM VGA font\n"
        "  export --irc <in> <out>\n"
        "                       write IRC text (mIRC colour codes) and report each\n"
        "                       line's size in bytes\n"
        "  shade --ramp R <in> <out>\n"
        "                       lighten (or --darken) every cell in the box one\n"
        "                       rung along the ramp's shading ladder, then check\n"
        "                       the ladder; <out> may be <in>\n"
        "  library list [dir]   list the stamps in dir (default: the bundled and\n"
        "                       user libraries): category, name, size, widest\n"
        "                       IRC row in bytes, tags, path; tab-separated\n"
        "  library check <dir>  check every stamp and sidecar in dir"));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();
    const QCommandLineOption lossyOpt(QStringLiteral("allow-lossy"),
                                      QStringLiteral("convert, shade: write even if something can't be represented"));
    parser.addOption(lossyOpt);
    const QCommandLineOption scaleOpt(QStringLiteral("scale"),
                                      QStringLiteral("render: integer zoom, nearest neighbour (1-8, default 1)"),
                                      QStringLiteral("N"), QStringLiteral("1"));
    parser.addOption(scaleOpt);
    const QCommandLineOption fontWidthOpt(QStringLiteral("font-width"),
                                          QStringLiteral("render: glyph width 8 or 9 (default: SAUCE letter spacing, else 8)"),
                                          QStringLiteral("px"));
    parser.addOption(fontWidthOpt);
    const QCommandLineOption ircOpt(QStringLiteral("irc"), QStringLiteral("export: IRC text with mIRC colour codes"));
    parser.addOption(ircOpt);
    const QCommandLineOption budgetOpt(
        QStringLiteral("budget"),
        QStringLiteral("export --irc: bytes a line may take (default %1, EFnet via WeeChat)")
            .arg(bw::irc::kEfnetWeechatBudget),
        QStringLiteral("bytes"));
    parser.addOption(budgetOpt);
    const QCommandLineOption overOpt(QStringLiteral("allow-over-budget"),
                                     QStringLiteral("export --irc: write even if a line is over the budget"));
    parser.addOption(overOpt);
    const QCommandLineOption rampOpt(
        QStringLiteral("ramp"),
        QStringLiteral("shade: a ramp name (fire, steel, ...) or palette indices dark to light, e.g. 0,8,7,15"),
        QStringLiteral("ramp"));
    parser.addOption(rampOpt);
    const QCommandLineOption darkenOpt(QStringLiteral("darken"), QStringLiteral("shade: darken (default: lighten)"));
    parser.addOption(darkenOpt);
    const QCommandLineOption stepsOpt(QStringLiteral("steps"),
                                      QStringLiteral("shade: rungs to move each cell (1-64, default 1)"),
                                      QStringLiteral("N"), QStringLiteral("1"));
    parser.addOption(stepsOpt);
    const QCommandLineOption boxOpt(QStringLiteral("box"),
                                    QStringLiteral("shade: cells x0,y0,x1,y1 from 0, corners included (default: all)"),
                                    QStringLiteral("x0,y0,x1,y1"));
    parser.addOption(boxOpt);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("info, convert, render, export, shade or library"));
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("command arguments"), QStringLiteral("[args...]"));
    if (!parser.parse(QCoreApplication::arguments())) {
        std::cerr << "blockwright-cli: " << parser.errorText().toStdString() << "\n";
        return kUsage;
    }
    if (parser.isSet(versionOpt)) {
        std::cout << "blockwright-cli " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n";
        std::cout << bw::kFontCredit;
        return kOk;
    }
    if (parser.isSet(helpOpt))
        parser.showHelp(kOk);

    const QStringList args = parser.positionalArguments();
    auto usage = [](const char* msg) {
        std::cerr << "blockwright-cli: " << msg << "\n"
                  << "usage: blockwright-cli info <file> | convert [--allow-lossy] <in> <out>\n"
                  << "       | render [--scale N] [--font-width 8|9] <in> <out.png>\n"
                  << "       | export --irc [--budget N] [--allow-over-budget] <in> <out>\n"
                  << "       | shade --ramp R [--darken] [--steps N] [--box x0,y0,x1,y1] [--allow-lossy] <in> <out>\n"
                  << "       | library list [dir] | library check <dir>\n"
                  << "       | --version\n";
        return kUsage;
    };
    if (args.isEmpty())
        return usage("no command given");
    const QString cmd = args.first();
    // Each option belongs to its commands; every other command refuses it.
    const std::pair<const QCommandLineOption*, std::vector<const char*>> owners[] = {
        {&lossyOpt, {"convert", "shade"}}, {&scaleOpt, {"render"}},  {&fontWidthOpt, {"render"}},
        {&ircOpt, {"export"}},             {&budgetOpt, {"export"}}, {&overOpt, {"export"}},
        {&rampOpt, {"shade"}},             {&darkenOpt, {"shade"}},  {&stepsOpt, {"shade"}},
        {&boxOpt, {"shade"}},
    };
    if (cmd == QLatin1String("info") || cmd == QLatin1String("convert") || cmd == QLatin1String("render") ||
        cmd == QLatin1String("export") || cmd == QLatin1String("shade") || cmd == QLatin1String("library"))
        for (const auto& [o, cmds] : owners) {
            if (!parser.isSet(*o) ||
                std::any_of(cmds.begin(), cmds.end(), [&](const char* c) { return cmd == QLatin1String(c); }))
                continue;
            std::string owner;
            for (std::size_t i = 0; i < cmds.size(); ++i)
                owner += std::string(i == 0 ? "" : i + 1 == cmds.size() ? " or " : ", ") + cmds[i];
            return usage(("--" + o->names().first().toStdString() + " is " + (owner[0] == 'e' ? "an " : "a ") +
                          owner + " option, not a " + cmd.toStdString() + " option")
                             .c_str());
        }
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
        if (cmd == QLatin1String("render")) {
            if (args.size() != 3)
                return usage("render takes an input .ans and an output .png");
            bw::RenderOptions opts;
            opts.scale = intValue(parser.value(scaleOpt), "scale", 1, bw::kMaxScale);
            if (parser.isSet(fontWidthOpt))
                opts.fontWidth = intValue(parser.value(fontWidthOpt), "font-width", 8, 9);
            return render(args[1].toStdString(), args[2].toStdString(), opts);
        }
        if (cmd == QLatin1String("export")) {
            if (!parser.isSet(ircOpt))
                return usage("export needs a format: --irc");
            if (args.size() != 3)
                return usage("export takes an input and an output file");
            std::size_t budget = bw::irc::kEfnetWeechatBudget;
            if (parser.isSet(budgetOpt)) {
                bool ok = false;
                const qulonglong v = parser.value(budgetOpt).toULongLong(&ok);
                if (!ok || v == 0)
                    return usage("--budget takes a positive number of bytes");
                budget = static_cast<std::size_t>(v);
            }
            return exportIrc(args[1].toStdString(), args[2].toStdString(), budget, parser.isSet(overOpt));
        }
        if (cmd == QLatin1String("shade")) {
            if (!parser.isSet(rampOpt))
                return usage("shade needs a ramp: --ramp NAME or --ramp c,c,...");
            if (args.size() != 3)
                return usage("shade takes an input and an output file");
            ShadeOptions o;
            o.ramp = rampValue(parser.value(rampOpt));
            if (parser.isSet(darkenOpt))
                o.direction = bw::ShadeDirection::Darken;
            o.steps = intValue(parser.value(stepsOpt), "steps", 1, 64);
            if (parser.isSet(boxOpt))
                o.box = boxValue(parser.value(boxOpt));
            o.allowLossy = parser.isSet(lossyOpt);
            return shade(args[1].toStdString(), args[2].toStdString(), o);
        }
        if (cmd == QLatin1String("library")) {
            const QString sub = args.value(1);
            if (sub == QLatin1String("list")) {
                if (args.size() > 3)
                    return usage("library list takes at most one directory");
                return libraryList(args.size() == 3 ? std::optional(args[2].toStdString()) : std::nullopt);
            }
            if (sub == QLatin1String("check")) {
                if (args.size() != 3)
                    return usage("library check takes one directory");
                return libraryCheck(args[2].toStdString());
            }
            return usage(sub.isEmpty() ? "library needs a subcommand: list or check"
                                       : ("unknown library subcommand '" + sub.toStdString() + "'").c_str());
        }
        return usage(("unknown command '"+ cmd.toStdString() + "'").c_str());
    } catch (const Usage& u) {
        return usage(u.message.c_str());
    } catch (const Failure& f) {
        std::cerr << "blockwright-cli: " << f.message << "\n";
        return kBadInput;
    } catch (const bw::irc::IrcError& e) {
        std::cerr << "blockwright-cli: " << args.value(1).toStdString() << ": " << e.what() << "\n";
        return kBadInput;
    } catch (const std::bad_alloc&) {
        std::cerr << "blockwright-cli: out of memory\n";
        return kBadInput;
    } catch (const std::length_error&) {
        // Backstop: a string or buffer past its max_size()
        std::cerr << "blockwright-cli: input too large\n";
        return kBadInput;
    }
}
