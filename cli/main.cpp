// blockwright-cli: headless access to the core.
//
//   blockwright-cli --version
//   blockwright-cli info <file.ans|project>
//   blockwright-cli convert [--allow-lossy] <in.ans|project> <out.ans>
//   blockwright-cli render [--scale N] [--font-width 8|9] <in.ans> <out.png>
//   blockwright-cli export --irc [--budget N] [--fit] [--pad] [--swap-shades]
//                          [--allow-over-budget] <in.ans|project> <out.txt>
//   blockwright-cli shade --ramp NAME|c,c,... [--darken] [--steps N] [--box x0,y0,x1,y1]
//                         [--mask <mask.ans>] [--ice] [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli shade --form dome|cylinder|plane [--glint] | --despeckle | --rim
//                         --ramp NAME|c,c,... [--light N|NE|...|NW] --box x0,y0,x1,y1 and/or
//                         --mask <mask.ans> [--ice] [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli lint [--ramp NAME|c,c,...]... [--box x0,y0,x1,y1] [--json] <in.ans>
//   blockwright-cli render --irc [--budget N] [--scale N] [--font-width 8|9] <in.ans> <out.png>
//   blockwright-cli library list [dir]
//   blockwright-cli library check <dir>
//   blockwright-cli pack <in.ans|project> <out.bwp>
//   blockwright-cli unpack <in.ans|project> <dir>
//   blockwright-cli snapshot [--allow-lossy] <project>
//   blockwright-cli add-generator [--name NAME] [--timeout S] <project> <generator-dir>
//   blockwright-cli run-generators [--layer ID]... [--timeout S] <project>
//
// Every command that reads an .ans also reads a project (PLAN §6), a .bwp
// zip or a project directory, as its flattened composite.
//
// Exit status: 0 ok, 1 bad input, an I/O failure or a lossy convert or shade
// without --allow-lossy (nothing is written), or a library check that found
// a problem, or a lossy snapshot without --allow-lossy, or a generator that failed (nothing is written), 2 usage
// error, 3 an IRC line over the byte budget without
// --allow-over-budget (nothing is written), 4 lint reported findings.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/cp437.h"
#include "blockwright/credits.h"
#include "blockwright/generator.h"
#include "blockwright/irc.h"
#include "blockwright/irc_render.h"
#include "blockwright/irc_view.h"
#include "blockwright/library.h"
#include "blockwright/lint.h"
#include "blockwright/project.h"
#include "blockwright/render.h"
#include "blockwright/version.h"
#include "generators.h"
#include "lint.h"
#include "read_file.h"
#include "shade.h"

#include <QBuffer>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QImageWriter>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kOk = 0, kBadInput = 1, kUsage = 2, kOverBudget = 3, kFindings = 4;

using bwcli::Failure;
using bwcli::readFile;

struct Usage {
    std::string message;
};

bw::project::Project loadProject(const std::string& path) {
    try {
        return bw::project::read(path);
    } catch (const bw::project::ProjectError& e) {
        throw Failure{path + ": " + e.what()};
    }
}

// An .ans, or a project as its flattened composite.
bw::AnsiDocument load(const std::string& path) {
    if (bw::project::isProject(path))
        return bw::project::toAnsi(loadProject(path));
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

const char* yesNo(bool v) { return v ? "yes" : "no"; }

int projectInfo(const std::string& path) {
    const bw::project::Project p = loadProject(path);
    const bw::LayerStack& s = p.layers;
    std::cout << "file:        " << path << "\n"
              << "project:     " << (std::filesystem::is_directory(path) ? "directory" : "zip") << "\n"
              << "size:        " << s.width() << "x" << s.height() << "\n"
              << "ice:         " << (p.ice ? "on" : "off") << "\n"
              << "irc budget:  " << p.ircBudget.bytes << " bytes (" << p.ircBudget.profile << ")\n"
              << "layers:      " << s.size() << " (next id " << s.nextId() << ")\n";
    // top first, as the layer panel lists them
    for (auto it = s.layers().rbegin(); it != s.layers().rend(); ++it) {
        const bw::LayerProperties& l = it->props;
        std::cout << "  " << l.id << "  " << bw::layerKindName(l.kind) << "  " << it->width() << "x" << it->height()
                  << " at " << l.x << "," << l.y << "  " << bw::layerModeName(l.mode) << "  visible "
                  << yesNo(l.visible) << ", locked " << yesNo(l.locked) << ", alpha locked " << yesNo(l.alphaLocked)
                  << "  " << l.name << "\n";
    }
    std::cout << "notes:       " << p.notes.size() << " bytes\n"
              << "snapshots:   " << p.snapshots.size();
    if (!p.snapshots.empty())
        std::cout << " (last r" << p.snapshots.back().round << ")";
    std::cout << "\nfiles:       " << p.files.size() << "\n";
    for (const auto& [name, bytes] : p.files)
        std::cout << "  " << name << "  " << bytes.size() << " bytes\n";
    if (!p.sauce) {
        std::cout << "sauce:       none\n";
        return kOk;
    }
    const bw::Sauce& a = *p.sauce;
    std::cout << "sauce:       yes\n"
              << "title:       " << text(a.title) << "\n"
              << "author:      " << text(a.author) << "\n"
              << "group:       " << text(a.group) << "\n"
              << "date:        " << text(a.date) << "\n"
              << "letter spacing: " << spacingName(a.letterSpacing()) << "\n"
              << "aspect:      " << aspectName(a.aspectRatio()) << "\n"
              << "font:        " << text(a.font) << "\n"
              << "comments:    " << a.comments.size() << "\n";
    for (const auto& c : a.comments)
        std::cout << "  " << text(c) << "\n";
    return kOk;
}

int info(const std::string& path) {
    if (bw::project::isProject(path))
        return projectInfo(path);
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
    // a flat file never replaces a project (shade a.bwp a.bwp, say)
    if (bw::project::isProject(out))
        throw Failure{out + ": not written: it is a project (pack and unpack write projects)"};
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
// reason when the caller knows it. `blinkDropped` is blink the document lost
// before it got here (openAsEditor), counted with what writeAnsi drops.
// Warns once per kind of loss in what writeAnsi wrote for `in`, with a count.
void warnLoss(const std::string& in, const bw::AnsiOutput& res) {
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
}

void saveAnsi(const std::string& in, const std::string& out, const bw::AnsiDocument& doc, bool allowLossy,
              const std::string& why = {}, int blinkDropped = 0) {
    bw::AnsiOutput res = bw::writeAnsi(doc);
    res.blinkDropped += blinkDropped;
    warnLoss(in, res);
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

// An .ans as a one-layer project, opened as the editor opens it (a warning
// for blink that loses), or a project as it is.
bw::project::Project asProject(const std::string& in) {
    if (bw::project::isProject(in))
        return loadProject(in);
    bw::Opened opened;
    bw::project::Project p = bw::project::fromAnsi(load(in), &opened);
    bw::AnsiOutput loss;
    loss.blinkDropped = opened.blinkDropped;
    warnLoss(in, loss);
    return p;
}

void writeProject(const bw::project::Project& p, const std::string& out, bool directory) {
    try {
        if (directory)
            bw::project::writeDirectory(p, out);
        else
            bw::project::writeZip(p, out);
    } catch (const bw::project::ProjectError& e) {
        throw Failure{out + ": " + e.what()};
    }
}

int pack(const std::string& in, const std::string& out, bool directory) {
    refuseSameFile(in, out);
    writeProject(asProject(in), out, directory);
    return kOk;
}

// Saves the project's composite as its next round, in the form it was read.
int snapshot(const std::string& path, bool allowLossy) {
    if (!bw::project::isProject(path))
        throw Failure{path + ": not a project"};
    bw::project::Project p = loadProject(path);
    const bw::AnsiOutput res = bw::project::takeSnapshot(p);
    warnLoss(path, res);
    const int round = p.snapshots.back().round;
    if (res.lossy() && !allowLossy)
        throw Failure{path + ": r" + std::to_string(round) +
                      " not saved: the snapshot loses data (--allow-lossy saves it anyway)"};
    writeProject(p, path, std::filesystem::is_directory(path));
    std::cout << "r" << round << "\n";
    return kOk;
}

// Adds the generator in `dir` to the project as a new layer on top, runs it
// for its first cells and saves the project in the form it was read. Prints
// the new layer's id.
int addGenerator(const std::string& path, const std::string& dir, std::optional<std::string> name,
                 const bw::generator::RunOptions& opts) {
    if (!bw::project::isProject(path))
        throw Failure{path + ": not a project"};
    bw::project::Project p = loadProject(path);
    const auto files = bwcli::readGeneratorDir(dir);
    bwcli::prepareToRunGenerators();
    if (!name) {
        std::filesystem::path d = std::filesystem::absolute(dir).lexically_normal();
        if (d.filename().empty())
            d = d.parent_path();
        name = d.filename().string();
    }
    bw::LayerId id = 0;
    try {
        id = bw::generator::add(p, files, *name, opts);
    } catch (const bw::generator::GeneratorError& e) {
        throw Failure{path + ": " + e.what()};
    }
    writeProject(p, path, std::filesystem::is_directory(path));
    const bw::Layer& l = *p.layers.find(id);
    std::cout << "layer " << id << " (" << l.props.name << "): " << l.width() << "x" << l.height() << "\n";
    return kOk;
}

// Runs the project's generator layers (those in `ids`, or every one), bottom
// first, and saves the project in the form it was read with their new cells.
// If any fails nothing is written.
int runGenerators(const std::string& path, const std::vector<bw::LayerId>& ids,
                  const bw::generator::RunOptions& opts) {
    if (!bw::project::isProject(path))
        throw Failure{path + ": not a project"};
    bw::project::Project p = loadProject(path);
    const std::vector<bw::LayerId> todo = bwcli::generatorsToRun(p.layers, ids);
    bwcli::prepareToRunGenerators();
    for (const bw::LayerId id : todo) {
        try {
            bw::generator::refresh(p, id, opts);
        } catch (const bw::generator::GeneratorError& e) {
            throw Failure{path + ": " + e.what() + "\n" + path + ": not written"};
        }
        const bw::Layer& l = *p.layers.find(id);
        std::cout << "layer " << id << " (" << l.props.name << "): " << l.width() << "x" << l.height() << "\n";
    }
    if (!todo.empty())
        writeProject(p, path, std::filesystem::is_directory(path));
    std::cout << todo.size() << (todo.size() == 1 ? " generator" : " generators") << " run\n";
    return kOk;
}

// Writes the piece as IRC text and reports every line's size in bytes, as
// the row encoder writes it with `opts`. The budget is `budget` if given,
// else a project's own (project::IrcBudget), else EFnet / WeeChat's; with
// `fit` the lines over it are fitted to it. A line still over the budget is
// listed and nothing is written, unless allowOver.
int exportIrc(const std::string& in, const std::string& out, std::optional<std::size_t> budget, bool fit,
              bw::irc::EncodeOptions opts, bool allowOver) {
    refuseSameFile(in, out);
    bw::AnsiDocument doc;
    if (bw::project::isProject(in)) {
        const bw::project::Project p = loadProject(in);
        budget = budget.value_or(p.ircBudget.bytes);
        doc = bw::project::toAnsi(p);
    } else {
        doc = load(in);
    }
    const std::size_t limit = budget.value_or(bw::irc::kEfnetWeechatBudget);
    opts.fit = fit ? limit : 0;
    std::vector<std::string> lines;
    std::size_t longest = 0, fitted = 0;
    int merged = 0;
    std::cout << "line  bytes\n";
    for (bw::irc::Line& l : bw::irc::exportLines(doc.canvas, opts)) {
        const std::size_t n = l.text.size();
        longest = std::max(longest, n);
        std::cout << std::setw(4) << lines.size() + 1 << "  " << std::setw(5) << n;
        if (n > limit)
            std::cout << "  over by " << n - limit;
        if (l.merged > 0) {
            std::cout << "  fit: " << l.merged << (l.merged == 1 ? " cell" : " cells") << " merged";
            merged += l.merged;
            ++fitted;
        }
        std::cout << "\n";
        lines.push_back(std::move(l.text));
    }
    const auto over = bw::irc::overBudget(lines, limit);
    std::cout << lines.size() << (lines.size() == 1 ? " line" : " lines") << ", longest " << longest
              << " bytes, budget " << limit << ": " << over.size() << " over\n";
    if (fit)
        std::cout << "fit: " << merged << (merged == 1 ? " cell" : " cells") << " merged in " << fitted
                  << (fitted == 1 ? " line" : " lines") << "\n";
    if (!over.empty()) {
        for (const auto& o : over)
            std::cerr << "blockwright-cli: " << in << ": line " << o.line << " is " << o.bytes << " bytes, over the "
                      << limit << "-byte budget by " << o.bytes - limit << "\n";
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

// --budget N for render --irc: a positive number of bytes; empty is the
// EFnet / WeeChat budget.
std::size_t budgetValue(const QString& s) {
    if (s.isEmpty())
        return bw::irc::kEfnetWeechatBudget;
    bool ok = false;
    const qulonglong v = s.toULongLong(&ok);
    if (!ok || v == 0)
        throw Usage{"--budget takes a positive number of bytes"};
    return static_cast<std::size_t>(v);
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

using bwcli::ShadeOptions;

const char* const kCompass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
const char* const kForms[] = {"dome", "cylinder", "plane"};

// One pass of o.mode's brush over the selection (the box, the mask piece's
// non-blank cells, both, or the whole canvas) with the piece's iCE colours,
// then the ladder check of the selection's bounds. The check only reports;
// the file is written either way.
int shade(const std::string& in, const std::string& out, ShadeOptions o, const std::optional<std::string>& mask) {
    bw::AnsiDocument doc = load(in);
    if (mask) {
        const bw::AnsiDocument m = load(*mask);
        const bw::Canvas& mc = m.canvas;
        const bw::Canvas& c = doc.canvas;
        if (mc.width() != c.width() || mc.height() != c.height())
            throw Usage{"--mask " + *mask + " is " + std::to_string(mc.width()) + "x" + std::to_string(mc.height()) +
                        ", " + in + " is " + std::to_string(c.width()) + "x" + std::to_string(c.height())};
        o.mask = bwcli::maskOf(mc);
    }
    bwcli::Shaded r;
    try {
        r = bwcli::shadeDocument(doc, o);
    } catch (const Failure& f) {
        throw Failure{in + ": " + f.message};
    }
    saveAnsi(in, out, doc, o.allowLossy, r.why, r.blinkDropped);
    if (r.iceTurnedOn)
        std::cerr << "blockwright-cli: note: " << out << ": written with iCE colours on; " << in
                  << " had them off\n";

    const bw::lint::Box& box = r.box;
    const std::size_t n = r.edit.changes.size();
    const char* cells = n == 1 ? " cell" : " cells";
    const char* light = kCompass[static_cast<int>(o.light)];
    switch (o.mode) {
    case bwcli::ShadeMode::Step:
        std::cout << (o.direction == bw::ShadeDirection::Lighten ? "lightened " : "darkened ") << n << cells << " "
                  << o.steps << (o.steps == 1 ? " rung" : " rungs");
        break;
    case bwcli::ShadeMode::Form:
        std::cout << "shaded " << n << cells << " as a " << kForms[static_cast<int>(o.form)] << " lit from " << light;
        break;
    case bwcli::ShadeMode::Despeckle:
        std::cout << "despeckled " << n << cells;
        break;
    case bwcli::ShadeMode::Rim:
        std::cout << "rim-lit " << n << cells << " from " << light;
        break;
    }
    std::cout << " along " << o.ramp.name << " in " << box.x0 << "," << box.y0 << "-" << box.x1 << "," << box.y1
              << "\n";
    const bw::lint::LadderReport rep = bw::lint::checkLadder(doc.canvas, o.ramp, box);
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

// With `ircBudget`, the IRC view (irc_view.h) cut at that many bytes, and a
// line on stdout saying how many rows the cut reaches.
int render(const std::string& in, const std::string& out, const bw::RenderOptions& opts,
           std::optional<std::size_t> ircBudget) {
    refuseSameFile(in, out);
    const bw::AnsiDocument doc = load(in);
    if (doc.sauce && doc.sauce->aspectRatio() == 1)
        std::cerr << "blockwright-cli: note: " << in
                  << ": SAUCE asks for the legacy (stretched) aspect ratio; the PNG has square pixels\n";
    QImage img;
    try {
        if (ircBudget) {
            const bw::irc::View view = bw::irc::ircView(doc.canvas, *ircBudget);
            img = bw::renderIrcView(view, bw::glyphWidth(doc, opts), opts.scale);
            const int cut = view.cutRows();
            std::cout << "IRC view: " << cut << (cut == 1 ? " row" : " rows") << " cut at " << *ircBudget
                      << " bytes\n";
        } else {
            img = bw::renderDocument(doc, opts);
        }
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

// The whole lint of the piece (lint_rules.h) with its own iCE colours: one
// line per finding and a summary, or JSON. Findings exit kFindings.
int lint(const std::string& in, const bwcli::LintCliOptions& o) {
    const bw::AnsiDocument doc = load(in);
    const bw::lint::LintReport r = bwcli::lintDocument(doc, o);
    std::cout << (o.json ? bwcli::lintJson(r) : bwcli::lintText(r));
    return r.findings.empty() ? kOk : kFindings;
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
        "                       line's size in bytes; a project's own budget is\n"
        "                       the default\n"
        "  shade --ramp R <in> <out>\n"
        "                       lighten (or --darken) every cell in the box one\n"
        "                       rung along the ramp's shading ladder, then check\n"
        "                       the ladder; <out> may be <in>. A piece without\n"
        "                       iCE colours keeps them off (no shade that needs\n"
        "                       a bright background) unless --ice turns them on\n"
        "  shade --form dome|cylinder|plane, --despeckle or --rim\n"
        "                       one pass of the form, despeckle or rim-light\n"
        "                       brush over the box, the non-blank cells of a\n"
        "                       --mask piece the size of <in>, or the mask's cells\n"
        "                       in the box, lit from --light\n"
        "  lint <in>            the ladder check under each --ramp (default: every\n"
        "                       named ramp and the craft pairs\n"
        "                       between), pixel noise and the craft rules; one\n"
        "                       line per finding (x,y kind ramp message) and a\n"
        "                       summary, or --json. Exits 4 when there are findings\n"
        "  render --irc <in> <out.png>\n"
        "                       draw the IRC view: the mIRC palette, what IRC\n"
        "                       drops, and each line cut at --budget bytes\n"
        "  library list [dir]   list the stamps in dir (default: the bundled and\n"
        "                       user libraries): category, name, size, widest\n"
        "                       IRC row in bytes, tags, path; tab-separated\n"
        "  library check <dir>  check every stamp and sidecar in dir\n"
        "  pack <in> <out.bwp>  write an .ans or a project as a project zip\n"
        "  unpack <in> <dir>    write an .ans or a project as a project directory;\n"
        "                       <dir> must be empty or a project already\n"
        "  snapshot <project>   save the flattened composite as the project's next\n"
        "                       round (snapshots/rN.ans) and print rN\n"
        "  add-generator <project> <dir>\n"
        "                       add a generator layer on top, from a directory\n"
        "                       with a generator.json (and params.json, scripts),\n"
        "                       run it once and save the project\n"
        "  run-generators <project>\n"
        "                       run the project's generator layers (or each\n"
        "                       --layer) again and save their output; this runs\n"
        "                       the programs their generator.json names\n\n"
        "Every command that reads an .ans also reads a project (a .bwp zip or a\n"
        "project directory) as its flattened composite."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();
    const QCommandLineOption lossyOpt(QStringLiteral("allow-lossy"),
                                      QStringLiteral("convert, shade, snapshot: write even if something can't be represented"));
    parser.addOption(lossyOpt);
    const QCommandLineOption scaleOpt(QStringLiteral("scale"),
                                      QStringLiteral("render: integer zoom, nearest neighbour (1-8, default 1)"),
                                      QStringLiteral("N"), QStringLiteral("1"));
    parser.addOption(scaleOpt);
    const QCommandLineOption fontWidthOpt(QStringLiteral("font-width"),
                                          QStringLiteral("render: glyph width 8 or 9 (default: SAUCE letter spacing, else 8)"),
                                          QStringLiteral("px"));
    parser.addOption(fontWidthOpt);
    const QCommandLineOption ircOpt(QStringLiteral("irc"),
                                    QStringLiteral("export: IRC text with mIRC colour codes; render: the IRC view"));
    parser.addOption(ircOpt);
    const QCommandLineOption budgetOpt(
        QStringLiteral("budget"),
        QStringLiteral("export --irc, render --irc: bytes a line may take (default: a project's own for export, else "
                       "%1, EFnet via WeeChat)")
            .arg(bw::irc::kEfnetWeechatBudget),
        QStringLiteral("bytes"));
    parser.addOption(budgetOpt);
    const QCommandLineOption fitOpt(
        QStringLiteral("fit"),
        QStringLiteral("export --irc: fit lines over the budget by merging their least visible colours"));
    parser.addOption(fitOpt);
    const QCommandLineOption padOpt(
        QStringLiteral("pad"),
        QStringLiteral("export --irc: keep trailing black cells and blank edge rows, ending rows on a black block"));
    parser.addOption(padOpt);
    const QCommandLineOption swapShadesOpt(
        QStringLiteral("swap-shades"),
        QStringLiteral("export --irc: let the encoder draw a shade as its opposite with the colours reversed"));
    parser.addOption(swapShadesOpt);
    const QCommandLineOption overOpt(QStringLiteral("allow-over-budget"),
                                     QStringLiteral("export --irc: write even if a line is over the budget"));
    parser.addOption(overOpt);
    const QCommandLineOption rampOpt(
        QStringLiteral("ramp"),
        QStringLiteral("shade, lint: a ramp name (fire, steel, ...) or palette indices dark to light, e.g. 0,8,7,15; "
                       "lint takes it more than once (default: every named ramp and the craft pairs)"),
        QStringLiteral("ramp"));
    parser.addOption(rampOpt);
    const QCommandLineOption darkenOpt(QStringLiteral("darken"), QStringLiteral("shade: darken (default: lighten)"));
    parser.addOption(darkenOpt);
    const QCommandLineOption stepsOpt(QStringLiteral("steps"),
                                      QStringLiteral("shade: rungs to move each cell (1-64, default 1)"),
                                      QStringLiteral("N"), QStringLiteral("1"));
    parser.addOption(stepsOpt);
    const QCommandLineOption boxOpt(QStringLiteral("box"),
                                    QStringLiteral("shade, lint: cells x0,y0,x1,y1 from 0, corners included (default: all)"),
                                    QStringLiteral("x0,y0,x1,y1"));
    parser.addOption(boxOpt);
    const QCommandLineOption formOpt(QStringLiteral("form"),
                                     QStringLiteral("shade: shade the selection as a form: dome, cylinder or plane"),
                                     QStringLiteral("shape"));
    parser.addOption(formOpt);
    const QCommandLineOption glintOpt(QStringLiteral("glint"),
                                      QStringLiteral("shade --form: a glint of the ramp's top rung at the brightest point (default: none)"));
    parser.addOption(glintOpt);
    const QCommandLineOption despeckleOpt(QStringLiteral("despeckle"),
                                          QStringLiteral("shade: clear the selection's specks"));
    parser.addOption(despeckleOpt);
    const QCommandLineOption rimOpt(QStringLiteral("rim"),
                                    QStringLiteral("shade: light the selection's edge cells that face the light"));
    parser.addOption(rimOpt);
    const QCommandLineOption lightOpt(
        QStringLiteral("light"),
        QStringLiteral("shade --form, --rim: where the light comes from, N, NE, E, SE, S, SW, W or NW (default: NW)"),
        QStringLiteral("point"));
    parser.addOption(lightOpt);
    const QCommandLineOption maskOpt(
        QStringLiteral("mask"),
        QStringLiteral("shade: a piece the size of the input whose non-blank cells are the selection (with --box, "
                       "those in the box)"),
        QStringLiteral("mask.ans"));
    parser.addOption(maskOpt);
    const QCommandLineOption iceOpt(QStringLiteral("ice"),
                                    QStringLiteral("shade: turn iCE colours on (default: keep the piece's)"));
    parser.addOption(iceOpt);
    const QCommandLineOption jsonOpt(QStringLiteral("json"), QStringLiteral("lint: the findings as JSON"));
    parser.addOption(jsonOpt);
    const QCommandLineOption nameOpt(QStringLiteral("name"),
                                     QStringLiteral("add-generator: the layer's name (default: the directory's)"),
                                     QStringLiteral("name"));
    parser.addOption(nameOpt);
    const QCommandLineOption layerOpt(
        QStringLiteral("layer"),
        QStringLiteral("run-generators: run this generator layer (an id, as info lists them); more than once for "
                       "several (default: every one)"),
        QStringLiteral("id"));
    parser.addOption(layerOpt);
    const QCommandLineOption timeoutOpt(
        QStringLiteral("timeout"),
        QStringLiteral("add-generator, run-generators: seconds a generator may run (1-3600, default %1)")
            .arg(std::chrono::duration_cast<std::chrono::seconds>(bw::generator::kDefaultTimeout).count()),
        QStringLiteral("S"));
    parser.addOption(timeoutOpt);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("info, convert, render, export, shade, lint, library, pack, unpack, snapshot, add-generator or run-generators"));
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
                  << "       | export --irc [--budget N] [--fit] [--pad] [--swap-shades] [--allow-over-budget] <in> <out>\n"
                  << "       | shade --ramp R [--darken] [--steps N] [--box x0,y0,x1,y1] [--mask <mask>] [--ice] [--allow-lossy] <in> <out>\n"
                  << "       | shade --form dome|cylinder|plane [--glint] | --despeckle | --rim --ramp R [--light N|NE|...|NW]\n"
                  << "         --box x0,y0,x1,y1 and/or --mask <mask> [--ice] [--allow-lossy] <in> <out>\n"
                  << "       | lint [--ramp R]... [--box x0,y0,x1,y1] [--json] <in>\n"
                  << "       | render --irc [--budget N] [--scale N] [--font-width 8|9] <in> <out.png>\n"
                  << "       | library list [dir] | library check <dir>\n"
                  << "       | pack <in> <out.bwp> | unpack <in> <dir> | snapshot [--allow-lossy] <project>\n"
                  << "       | add-generator [--name NAME] [--timeout S] <project> <dir>\n"
                  << "       | run-generators [--layer ID]... [--timeout S] <project>\n"
                  << "       | --version\n";
        return kUsage;
    };
    if (args.isEmpty())
        return usage("no command given");
    const QString cmd = args.first();
    // Each option belongs to its commands; every other command refuses it.
    const std::pair<const QCommandLineOption*, std::vector<const char*>> owners[] = {
        {&lossyOpt, {"convert", "shade", "snapshot"}}, {&scaleOpt, {"render"}},  {&fontWidthOpt, {"render"}},
        {&ircOpt, {"export", "render"}},   {&budgetOpt, {"export", "render"}}, {&overOpt, {"export"}},
        {&fitOpt, {"export"}},             {&padOpt, {"export"}},    {&swapShadesOpt, {"export"}},
        {&rampOpt, {"shade", "lint"}},     {&darkenOpt, {"shade"}},  {&stepsOpt, {"shade"}},
        {&boxOpt, {"shade", "lint"}},      {&iceOpt, {"shade"}},     {&jsonOpt, {"lint"}},
        {&formOpt, {"shade"}},             {&glintOpt, {"shade"}},   {&despeckleOpt, {"shade"}},
        {&rimOpt, {"shade"}},              {&lightOpt, {"shade"}},   {&maskOpt, {"shade"}},
        {&nameOpt, {"add-generator"}},     {&layerOpt, {"run-generators"}},
        {&timeoutOpt, {"add-generator", "run-generators"}},
    };
    if (cmd == QLatin1String("info") || cmd == QLatin1String("convert") || cmd == QLatin1String("render") ||
        cmd == QLatin1String("export") || cmd == QLatin1String("shade") || cmd == QLatin1String("lint") ||
        cmd == QLatin1String("library") ||
        cmd == QLatin1String("pack") || cmd == QLatin1String("unpack") || cmd == QLatin1String("snapshot") ||
        cmd == QLatin1String("add-generator") || cmd == QLatin1String("run-generators"))
        for (const auto& [o, cmds] : owners) {
            if (!parser.isSet(*o) ||
                std::any_of(cmds.begin(), cmds.end(), [&](const char* c) { return cmd == QLatin1String(c); }))
                continue;
            std::string owner;
            for (std::size_t i = 0; i < cmds.size(); ++i)
                owner += std::string(i == 0 ? "" : i + 1 == cmds.size() ? " or " : ", ") + cmds[i];
            const bool vowel = std::string_view("aeiou").find(owner[0]) != std::string_view::npos;
            return usage(("--" + o->names().first().toStdString() + " is " + (vowel ? "an " : "a ") + owner +
                          " option, not a " + cmd.toStdString() + " option")
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
            std::optional<std::size_t> ircBudget;
            if (parser.isSet(budgetOpt) && !parser.isSet(ircOpt))
                return usage("--budget is a render --irc option");
            if (parser.isSet(ircOpt))
                ircBudget = budgetValue(parser.value(budgetOpt));
            return render(args[1].toStdString(), args[2].toStdString(), opts, ircBudget);
        }
        if (cmd == QLatin1String("export")) {
            if (!parser.isSet(ircOpt))
                return usage("export needs a format: --irc");
            if (args.size() != 3)
                return usage("export takes an input and an output file");
            std::optional<std::size_t> budget;
            if (parser.isSet(budgetOpt)) {
                bool ok = false;
                const qulonglong v = parser.value(budgetOpt).toULongLong(&ok);
                if (!ok || v == 0)
                    return usage("--budget takes a positive number of bytes");
                budget = static_cast<std::size_t>(v);
            }
            bw::irc::EncodeOptions opts;
            opts.pad = parser.isSet(padOpt);
            opts.swapShades = parser.isSet(swapShadesOpt);
            return exportIrc(args[1].toStdString(), args[2].toStdString(), budget, parser.isSet(fitOpt), opts,
                             parser.isSet(overOpt));
        }
        if (cmd == QLatin1String("shade")) {
            if (!parser.isSet(rampOpt))
                return usage("shade needs a ramp: --ramp NAME or --ramp c,c,...");
            if (args.size() != 3)
                return usage("shade takes an input and an output file");
            const bool step = parser.isSet(darkenOpt) || parser.isSet(stepsOpt);
            const bool form = parser.isSet(formOpt), despeckle = parser.isSet(despeckleOpt), rim = parser.isSet(rimOpt);
            if (int(step) + int(form) + int(despeckle) + int(rim) > 1)
                return usage("shade takes one mode: the rung step, --form, --despeckle or --rim");
            if (parser.isSet(glintOpt) && !form)
                return usage("--glint is a shade --form option");
            if (parser.isSet(lightOpt) && !form && !rim)
                return usage("--light is a shade --form or --rim option");
            ShadeOptions o;
            o.ramp = rampValue(parser.value(rampOpt));
            o.mode = form ? bwcli::ShadeMode::Form
                     : despeckle ? bwcli::ShadeMode::Despeckle
                     : rim ? bwcli::ShadeMode::Rim
                           : bwcli::ShadeMode::Step;
            if (parser.isSet(darkenOpt))
                o.direction = bw::ShadeDirection::Darken;
            o.steps = intValue(parser.value(stepsOpt), "steps", 1, 64);
            if (form) {
                const QString v = parser.value(formOpt);
                const auto* f = std::find_if(std::begin(kForms), std::end(kForms),
                                             [&](const char* n) { return v == QLatin1String(n); });
                if (f == std::end(kForms))
                    throw Usage{"--form takes dome, cylinder or plane, not '" + v.toStdString() + "'"};
                o.form = static_cast<bw::FormShape>(f - std::begin(kForms));
            }
            o.glint = parser.isSet(glintOpt);
            if (parser.isSet(lightOpt)) {
                const QString v = parser.value(lightOpt).toUpper();
                const auto* p = std::find_if(std::begin(kCompass), std::end(kCompass),
                                             [&](const char* n) { return v == QLatin1String(n); });
                if (p == std::end(kCompass))
                    throw Usage{"--light takes a compass point (N, NE, E, SE, S, SW, W, NW), not '" +
                                parser.value(lightOpt).toStdString() + "'"};
                o.light = static_cast<bw::Compass>(p - std::begin(kCompass));
            }
            if ((form || despeckle || rim) && !parser.isSet(boxOpt) && !parser.isSet(maskOpt))
                return usage(form ? "shade --form needs a selection: --box, --mask or both"
                             : despeckle ? "shade --despeckle needs a selection: --box, --mask or both"
                                         : "shade --rim needs a selection: --box, --mask or both");
            if (parser.isSet(boxOpt))
                o.box = boxValue(parser.value(boxOpt));
            o.ice = parser.isSet(iceOpt);
            o.allowLossy = parser.isSet(lossyOpt);
            std::optional<std::string> mask;
            if (parser.isSet(maskOpt))
                mask = parser.value(maskOpt).toStdString();
            return shade(args[1].toStdString(), args[2].toStdString(), o, mask);
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
        if (cmd == QLatin1String("lint")) {
            if (args.size() != 2)
                return usage("lint takes one file");
            bwcli::LintCliOptions o;
            for (const QString& r : parser.values(rampOpt))
                o.ramps.push_back(rampValue(r));
            if (parser.isSet(boxOpt))
                o.box = boxValue(parser.value(boxOpt));
            o.json = parser.isSet(jsonOpt);
            return lint(args[1].toStdString(), o);
        }
        if (cmd == QLatin1String("pack") || cmd == QLatin1String("unpack")) {
            if (args.size() != 3)
                return usage(cmd == QLatin1String("pack") ? "pack takes an input and an output .bwp"
                                                          : "unpack takes an input and an output directory");
            return pack(args[1].toStdString(), args[2].toStdString(), cmd == QLatin1String("unpack"));
        }
        if (cmd == QLatin1String("snapshot")) {
            if (args.size() != 2)
                return usage("snapshot takes one project");
            return snapshot(args[1].toStdString(), parser.isSet(lossyOpt));
        }
        bw::generator::RunOptions runOpts;
        if (parser.isSet(timeoutOpt))
            runOpts.timeout = std::chrono::seconds(intValue(parser.value(timeoutOpt), "timeout", 1, 3600));
        if (cmd == QLatin1String("add-generator")) {
            if (args.size() != 3)
                return usage("add-generator takes a project and a generator directory");
            std::optional<std::string> name;
            if (parser.isSet(nameOpt))
                name = parser.value(nameOpt).toStdString();
            return addGenerator(args[1].toStdString(), args[2].toStdString(), name, runOpts);
        }
        if (cmd == QLatin1String("run-generators")) {
            if (args.size() != 2)
                return usage("run-generators takes one project");
            std::vector<bw::LayerId> ids;
            for (const QString& v : parser.values(layerOpt)) {
                bool ok = false;
                const qulonglong id = v.toULongLong(&ok);
                if (!ok || id == 0 || id > std::numeric_limits<bw::LayerId>::max())
                    return usage(("--layer takes a layer id, not '" + v.toStdString() + "'").c_str());
                ids.push_back(static_cast<bw::LayerId>(id));
            }
            return runGenerators(args[1].toStdString(), ids, runOpts);
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
