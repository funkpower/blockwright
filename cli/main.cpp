// blockwright-cli: headless access to the core.
//
//   blockwright-cli --version
//   blockwright-cli info <file.ans|project>
//   blockwright-cli convert [--allow-lossy] [--output-encoding cp437|utf-8] [--palette P]
//                           <in.ans|project> <out.ans|out.xb|out.bin>
//   blockwright-cli render [--scale N] [--font-width 8|9] [--palette P] <in.ans> <out.png>
//   blockwright-cli remap --map 3=11,... | --fit-palette P [--allow-lossy]
//                         [--output-encoding cp437|utf-8] <in.ans|project> <out.ans|out.xb|out.bin>
//   blockwright-cli export --irc [--budget N] [--fit] [--pad] [--swap-shades]
//                          [--colours 16|99] [--allow-over-budget] <in.ans|project> <out.txt>
//   blockwright-cli export --micron [--centre] [--cache S] <in.ans|project> <out.mu>
//   blockwright-cli shade --ramp NAME|c,c,... [--darken] [--steps N] [--box x0,y0,x1,y1]
//                         [--mask <mask.ans>] [--ice] [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli shade --form dome|cylinder|plane [--glint] | --despeckle | --rim
//                         --ramp NAME|c,c,... [--light N|NE|...|NW] --box x0,y0,x1,y1 and/or
//                         --mask <mask.ans> [--ice] [--allow-lossy] <in.ans> <out.ans>
//   blockwright-cli lint [--ramp NAME|c,c,...]... [--box x0,y0,x1,y1] [--target ansi|irc|micron]
//                        [--colours 16|99] [--json] <in.ans>
//   blockwright-cli render --irc [--budget N] [--fit] [--pad] [--swap-shades] [--colours 16|99]
//                          [--scale N] [--font-width 8|9] <in.ans|project> <out.png>
//   blockwright-cli library list [dir] | library list [--dir DIR]
//   blockwright-cli library check <dir>
//   blockwright-cli library new [--dir DIR] <collection>
//   blockwright-cli library delete [--dir DIR] [--yes] [--permanent] <collection>
//   blockwright-cli library rename [--dir DIR] <collection> <new name>
//   blockwright-cli library add [--dir DIR] <collection> <file.ans|folder>...
//   blockwright-cli library remove [--dir DIR] [--yes] [--permanent] <stamp.ans>...
//   blockwright-cli library edit [--dir DIR] [--name N] [--category C] [--tags t1,t2] <stamp.ans>
//   blockwright-cli pack [--allow-lossy] [--target ansi|irc|micron] [--colours 16|99] <in.ans|project> <out.bwp>
//   blockwright-cli unpack [--allow-lossy] [--target ansi|irc|micron] [--colours 16|99] <in.ans|project> <dir>
//   blockwright-cli snapshot [--allow-lossy] [--output-encoding cp437|utf-8] <project>
//   blockwright-cli add-generator [--name NAME] [--timeout S] <project> <generator-dir>
//   blockwright-cli run-generators [--layer ID]... [--timeout S] <project>
//   blockwright-cli import-scene [--timeout S] [--bake] [--author NAME] [--date CCYYMMDD]
//                                <scene.py> <out.bwp|dir>
//   blockwright-cli replay [--project] [--render [--scale N] [--font-width 8|9]] [--allow-lossy]
//                          [--output-encoding cp437|utf-8] [--user-library <dir>] <steps.json> -o <dir>
//
// info, convert, render, export, shade, lint, pack and unpack take
// --encoding auto|cp437|utf-8: what an .ans input's text is read as (PLAN §8).
// convert, shade and replay take --output-encoding cp437|utf-8: what an .ans
// output's text is written as (default cp437). snapshot takes it too,
// defaulting to the project's own encoding (its ansEncoding).
//
// --palette and --fit-palette take a preset's name (cga, xterm, ...;
// bw::palettePresets) or a palette file (bw::readPaletteFile).
//
// Every command that reads an .ans also reads an XBIN (.xb, or any file that
// starts with the XBIN header) or a BIN (.bin), and a project (PLAN §6), a
// .bwp zip or a project directory, as its flattened composite. convert and
// shade write the format <out>'s name gives (.xb, .bin, else .ans). convert,
// shade, export and render refuse a .bwp <out>: pack writes a project.
//
// Exit status: 0 ok, 1 bad input, an I/O failure or a lossy convert, shade,
// pack or unpack without --allow-lossy (nothing is written), or a library check that found
// a problem, or a lossy snapshot without --allow-lossy, or a generator or scene import that failed (nothing is written), 2 usage
// error, 3 an IRC line over the byte budget without
// --allow-over-budget (nothing is written), 4 lint reported findings. An
// error no command names a file for (main's std::exception backstop) is
// "blockwright-cli: <command>: <what>", exit 1.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/composite.h"
#include "blockwright/cp437.h"
#include "blockwright/credits.h"
#include "blockwright/generator.h"
#include "blockwright/scene_import.h"
#include "blockwright/irc.h"
#include "blockwright/irc_render.h"
#include "blockwright/irc_view.h"
#include "blockwright/micron.h"
#include "blockwright/palette_swap.h"
#include "blockwright/library.h"
#include "blockwright/lint.h"
#include "blockwright/logo_text.h"
#include "blockwright/project.h"
#include "blockwright/render.h"
#include "blockwright/samples.h"
#include "blockwright/sauce.h"
#include "blockwright/steps.h"
#include "blockwright/version.h"
#include "blockwright/xbin.h"
#include "generators.h"
#include "lint.h"
#include "loss.h"
#include "read_file.h"
#include "replay.h"
#include "save_all.h"
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
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

// A project as read, and a note on stderr for what the read changed (the
// attribute bits a project before 5.0 can't hold, cleared), as the app's
// status bar says it on open (#370). Never stdout, which may be piped art.
bw::project::Project loadProject(const std::string& path) {
    bw::project::ReadNotes notes;
    bw::project::Project p;
    try {
        p = bw::project::read(path, &notes);
    } catch (const bw::project::ProjectError& e) {
        throw Failure{path + ": " + e.what()};
    }
    for (const std::string& n : bwcli::projectReadNotes(notes))
        std::cerr << "blockwright-cli: note: " << path << ": " << n << "\n";
    return p;
}

// --encoding: the character set every .ans a command reads is read as;
// nothing: what bw::detectEncoding finds in each.
std::optional<bw::TextEncoding> gEncoding;
// --output-encoding: the character set convert, shade and replay write an .ans
// in (snapshot's default is the project's own).
bw::TextEncoding gOutputEncoding = bw::TextEncoding::Cp437;

// The most bytes a flat file (.ans, XBIN, BIN, ADF, IDF, PCB, .asc, .txt)
// may hold: what a project's art entry may (project::kMaxEntryBytes), so a
// piece that packs into a project opens flat too.
constexpr std::size_t kMaxFlatBytes = bw::project::kMaxEntryBytes;

// An .ans, XBIN or BIN, or a project as its flattened composite.
bw::AnsiDocument load(const std::string& path) {
    if (bw::project::isProject(path))
        return bw::project::toAnsi(loadProject(path));
    const std::string data = readFile(path, kMaxFlatBytes);
    if (data.empty())
        throw Failure{path + ": empty file"};
    bw::AnsiDocument doc;
    try {
        doc = bw::parseArt(data, bw::artFormatOf(path, data), {.encoding = gEncoding});
    } catch (const bw::AnsiError& e) {
        throw Failure{path + ": " + e.what()};
    }
    for (const std::string& w : bwcli::readWarnings(doc.readLoss))
        std::cerr << "blockwright-cli: warning: " << path << ": " << w << "\n";
    return doc;
}

// A project as IRC text sends it: its composite as the editor saves it
// (toAnsi), except that under the IRC or Micron target an RGB colour is sent
// as it is, not folded by iCE, and, under any target, in `colours` 16 as
// the nearest of mIRC 0-15 (irc::sentCell, irc::fold16).
bw::AnsiDocument sentDocument(const bw::project::Project& p, bw::IrcColours colours = bw::IrcColours::Mirc99) {
    bw::AnsiDocument doc = bw::project::toAnsi(p);
    if (bw::richTarget(p.target)) {
        bw::CompositeOptions o;
        o.ice = p.ice;
        doc.canvas = bw::irc::sentCanvas(bw::flatten(p.layers, o), p.ice, p.target, colours);
    } else if (colours == bw::IrcColours::Mirc16) {
        // toAnsi keeps an RGB foreground, which the encoder would send as
        // the nearest of the 99
        for (int y = 0; y < doc.canvas.height(); ++y)
            for (int x = 0; x < doc.canvas.width(); ++x) {
                bw::Cell& c = doc.canvas.at(x, y);
                c.fg = bw::irc::fold16(c.fg);
                c.bg = bw::irc::fold16(c.bg);
            }
    }
    return doc;
}

// A project as Micron writes it, under either target: its composite as the
// editor shows it, cells not yet folded by iCE, since micron::encode folds an
// indexed one and keeps an RGB one as it is, as the app's Export Micron does
// (toAnsi would fold an RGB background to the nearest of the 16 first).
bw::AnsiDocument micronDocument(const bw::project::Project& p) {
    bw::AnsiDocument doc = bw::project::toAnsi(p);
    bw::CompositeOptions o;
    o.ice = p.ice;
    doc.canvas = bw::flatten(p.layers, o);
    return doc;
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
              << "encoding:    " << (p.ansEncoding == bw::TextEncoding::Utf8 ? "UTF-8" : "CP437") << "\n"
              << "target:      " << bw::targetName(p.target) << "\n"
              << "irc colours: " << bw::ircColourCount(p.ircColours) << "\n"
              << "palette:     " << (p.palette ? "custom" : "standard") << "\n"
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
    const std::string data = readFile(path, kMaxFlatBytes);
    const bw::ArtFormat format = bw::artFormatOf(path, data);
    // in binary text 0x1A is a glyph or a colour: the content is the bytes the cells take
    const std::string_view content = bw::splitArtSauce(data, format).content;
    std::cout << "file:        " << path << "\n";
    if (format == bw::ArtFormat::Xbin) {
        std::cout << "format:      XBIN" << (data[10] & 0x04 ? ", compressed" : "") << "\n"
                  << "palette:     " << (!doc.palette ? "none" : bw::customPalette(doc) ? "custom" : "standard") << "\n"
                  << "font:        ";
        if (doc.font)
            std::cout << "8x" << doc.font->height << "\n";
        else
            std::cout << "none\n";
    } else if (format == bw::ArtFormat::Bin) {
        std::cout << "format:      BIN\n";
    } else {
        std::cout << "encoding:    " << (doc.encoding == bw::TextEncoding::Utf8 ? "UTF-8" : "CP437") << "\n";
    }
    std::cout << "size:        " << doc.canvas.width() << "x" << doc.canvas.height() << "\n"
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

// Test-only fault hook (#380): with BLOCKWRIGHT_CLI_FAULT set to `step`,
// throws a std::runtime_error, a type no command wraps as a Failure, so the
// tests reach main's std::exception backstop part way through a command.
// std::getenv is standard C++, so it works alike on every platform.
void faultPoint(std::string_view step) {
    if (const char* v = std::getenv("BLOCKWRIGHT_CLI_FAULT"); v && step == v)
        throw std::runtime_error("fault injected at " + std::string(step));
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
    // the temporary is written; an exception here leaves it uncommitted,
    // which QSaveFile's destructor removes
    faultPoint("save");
    if (!f.commit())
        throw Failure{out + ": write failed: " + f.errorString().toStdString()};
}

// Writes the document in the format out's name gives (.xb, .bin, else
// .ans), warning once per kind of loss; a lossy
// result is written only with allowLossy. `why` replaces the refusal's
// reason when the caller knows it. `blinkDropped` is blink the document lost
// before it got here (openAsEditor), counted with what writeAnsi drops.
// Warns once per kind of loss in what writeAnsi wrote for `in`, with a count.
void warnLoss(const std::string& in, const bw::AnsiOutput& res) {
    for (const std::string& w : bwcli::unwritableWarnings(res))
        std::cerr << "blockwright-cli: warning: " << in << ": " << w << "\n";
    if (const int n = res.blinkDropped)
        std::cerr << "blockwright-cli: warning: " << in << ": " << n
                  << (n == 1 ? " blinking cell loses its blink" : " blinking cells lose their blink")
                  << ": bright backgrounds need iCE colours, where blink is a bright background\n";
    if (res.paletteDropped)
        std::cerr << "blockwright-cli: warning: " << in
                  << ": its palette is left out: only XBIN and a project hold one (remap --fit-palette cga first "
                     "keeps its look)\n";
    if (const int n = res.paletteRounded)
        std::cerr << "blockwright-cli: warning: " << in << ": " << n
                  << (n == 1 ? " palette colour is" : " palette colours are")
                  << " rounded to the 64 levels a channel XBIN holds\n";
    if (res.fontDropped)
        std::cerr << "blockwright-cli: warning: " << in << ": its font is left out: only XBIN holds one\n";
    if (res.letterSpacingDropped)
        std::cerr << "blockwright-cli: warning: " << in
                  << ": its 9 px letter spacing is left out: XBIN has no letter spacing and draws 8 px\n";
    if (res.aspectDropped)
        std::cerr << "blockwright-cli: warning: " << in << ": its " << aspectName(res.aspectDropped)
                  << " aspect ratio is left out: XBIN has no aspect ratio\n";
    if (!res.fontNameDropped.empty())
        std::cerr << "blockwright-cli: warning: " << in << ": its SAUCE font name (" << text(res.fontNameDropped)
                  << ") is left out: XBIN names no font\n";
    if (const int n = res.coloursQuantised)
        std::cerr << "blockwright-cli: warning: " << in << ": " << n
                  << (n == 1 ? " cell is in a colour" : " cells are in colours")
                  << " the file can't hold, written as the nearest of the 16\n";
    if (const int n = res.formattingDropped)
        std::cerr << "blockwright-cli: warning: " << in << ": " << n << (n == 1 ? " cell has" : " cells have")
                  << " bold, italic or underline, which the file can't hold: left out\n";
}

void saveAnsi(const std::string& in, const std::string& out, const bw::AnsiDocument& doc, bool allowLossy,
              const std::string& why = {}, int blinkDropped = 0) {
    bw::AnsiOutput res = bw::writeArt(doc, bw::artFormatFor(out), gOutputEncoding);
    res.blinkDropped += blinkDropped;
    warnLoss(in, res);
    if (res.lossy() && !allowLossy)
        throw Failure{out + ": not written: " + (why.empty() ? "the conversion loses data" : why) +
                      " (--allow-lossy writes it anyway)"};
    save(out, res.bytes);
}

// convert and shade write .ans, XBIN or BIN, export --irc IRC text and render
// a PNG. A .bwp is a project, which the app opens as a zip, so any of those
// bytes under that name are refused before the input is read, as save
// refuses to write over a project by any name: pack writes a project.
// `writes` says what the command writes instead.
void refuseProjectOutput(const char* cmd, const char* writes, const QString& out) {
    if (QFileInfo(out).suffix().compare(QLatin1String("bwp"), Qt::CaseInsensitive) == 0)
        throw Failure{out.toStdString() + ": not written: " + cmd + " writes " + writes +
                      "; use pack for a project"};
}

// A --palette or --fit-palette value: a preset's name, else a palette
// file. The standard palette is nothing, as a document without one.
std::optional<bw::Palette> paletteValue(const QString& v) {
    if (const bw::PalettePreset* p = bw::palettePreset(v.toStdString()))
        return p->colours == bw::kPalette ? std::nullopt : std::optional(p->colours);
    const std::string path = v.toStdString();
    if (!QFileInfo(v).isFile()) {
        std::string names;
        for (const bw::PalettePreset& p : bw::palettePresets())
            names += std::string(names.empty() ? "" : ", ") + std::string(p.name);
        throw Usage{"--palette takes a preset (" + names + ") or a palette file, not '" + path + "'"};
    }
    std::string error;
    int colours = 0;
    const std::optional<bw::Palette> p = bw::readPaletteFile(path, readFile(path, kMaxFlatBytes), &error, &colours);
    if (!p)
        throw Failure{path + ": " + error};
    if (colours > 16)
        std::cerr << "blockwright-cli: note: " << path << ": " << colours << " colours; the first 16 are the palette\n";
    if (*p == bw::kPalette)
        return std::nullopt;
    return p;
}

int convert(const std::string& in, const std::string& out, bool allowLossy,
            const std::optional<std::optional<bw::Palette>>& palette) {
    bw::AnsiDocument doc = load(in);
    if (palette)
        doc.palette = *palette;
    saveAnsi(in, out, doc, allowLossy);
    return kOk;
}

// remap: the cells' colour indices mapped by `map`, or fitted to `fit`
// (the palette becomes it and each index the nearest of its entries), then
// written as convert writes.
int remap(const std::string& in, const std::string& out, const std::optional<bw::ColourMap>& map,
          const std::optional<std::optional<bw::Palette>>& fit, bool allowLossy) {
    bw::AnsiDocument doc = load(in);
    bw::ColourMap m = map.value_or(bw::identityColourMap());
    int lost = 0;
    if (fit) {
        const bw::Palette from = doc.palette.value_or(bw::kPalette), to = fit->value_or(bw::kPalette);
        m = bw::fitColourMap(from, to);
        lost = bw::fitLoss(doc.canvas, from, to, m);
        doc.palette = *fit;
    }
    const int n = bw::remapCanvas(doc.canvas, m);
    std::cout << in << ": " << n << (n == 1 ? " cell" : " cells") << " remapped\n";
    if (lost)
        std::cerr << "blockwright-cli: warning: " << in << ": " << lost << (lost == 1 ? " cell shows" : " cells show")
                  << " another colour: the new palette has no entry that matches\n";
    saveAnsi(in, out, doc, allowLossy);
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

// An .ans (or XBIN, BIN) as a one-layer project, opened as the editor opens
// it (a warning for blink that loses), or a project as it is. A project
// keeps an XBIN's palette but not its font: a warning for that, and nothing is
// written to `out` without allowLossy, as convert refuses. Blink is lost on
// opening, as the editor loses it, so it is only warned of. `target` and
// `colours`, when given, are the project's target and IRC colours (a flat
// file's are ANSI and 99).
bw::project::Project asProject(const std::string& in, const std::string& out, bool allowLossy,
                               std::optional<bw::Target> target, std::optional<bw::IrcColours> colours) {
    if (bw::project::isProject(in)) {
        bw::project::Project p = loadProject(in);
        p.target = target.value_or(p.target);
        p.ircColours = colours.value_or(p.ircColours);
        return p;
    }
    bw::Opened opened;
    bw::AnsiDocument doc = load(in);
    bw::AnsiOutput loss;
    loss.fontDropped = doc.font.has_value();
    bw::project::Project p = bw::project::fromAnsi(doc, &opened);
    loss.blinkDropped = opened.blinkDropped;
    warnLoss(in, loss);
    if (loss.fontDropped && !allowLossy)
        throw Failure{out + ": not written: the project loses data (--allow-lossy writes it anyway)"};
    p.target = target.value_or(bw::Target::Ansi);
    p.ircColours = colours.value_or(bw::IrcColours::Mirc99);
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

int pack(const std::string& in, const std::string& out, bool directory, bool allowLossy,
         std::optional<bw::Target> target, std::optional<bw::IrcColours> colours) {
    refuseSameFile(in, out);
    writeProject(asProject(in, out, allowLossy, target, colours), out, directory);
    return kOk;
}

// Saves the project's composite as its next round, in the form it was read,
// in `encoding` or with none in the project's own.
int snapshot(const std::string& path, bool allowLossy, std::optional<bw::TextEncoding> encoding) {
    if (!bw::project::isProject(path))
        throw Failure{path + ": not a project"};
    bw::project::Project p = loadProject(path);
    bw::AnsiOutput res;
    try {
        res = bw::project::takeSnapshot(p, encoding);
    } catch (const bw::project::ProjectError& e) {
        // a project whose last round is r999999999 has no rounds left (#380)
        throw Failure{path + ": " + e.what()};
    }
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
    if (!name)
        name = bw::generator::directoryLayerName(dir);
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

// Runs the project's generator layers (those in `ids`, or every visible
// one), bottom first, and saves the project in the form it was read with
// their new cells. A hidden layer runs only when `ids` names it; the ones
// skipped are listed first. If any fails nothing is written.
int runGenerators(const std::string& path, const std::vector<bw::LayerId>& ids,
                  const bw::generator::RunOptions& opts) {
    if (!bw::project::isProject(path))
        throw Failure{path + ": not a project"};
    bw::project::Project p = loadProject(path);
    std::vector<bw::LayerId> hidden;
    const std::vector<bw::LayerId> todo = bwcli::generatorsToRun(p.layers, ids, &hidden);
    for (const bw::LayerId id : hidden)
        std::cout << "layer " << id << " (" << p.layers.find(id)->props.name << "): hidden, not run\n";
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

// What import-scene's --bake, --author and --date (or SOURCE_DATE_EPOCH)
// ask for, checked before the scene runs.
struct SceneFix {
    bool bake = false;
    bw::scene::Stamp stamp;
};

// Opens the scene as a new project (scene::importScene), each scene layer a
// generator layer run once, and writes it to `out`: a zip when its name
// ends in .bwp, else a directory. Prints each layer as it runs. Nothing is
// written over a file or a directory that isn't empty. With `fix.bake` the
// layers are written as cells, without their scripts; `fix.stamp` sets the
// SAUCE author and date.
int importScene(const std::string& scene, const std::string& out, const bw::generator::RunOptions& opts,
                const SceneFix& fix) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool directory = !QString::fromStdString(out).endsWith(QLatin1String(".bwp"), Qt::CaseInsensitive);
    if (fs::exists(fs::symlink_status(out, ec)) && !(fs::is_directory(out, ec) && fs::is_empty(out, ec)))
        throw Failure{out + ": not written: it exists" + (directory ? " and isn't an empty directory" : "")};
    bwcli::prepareToRunGenerators();
    bw::project::Project p;
    try {
        p = bw::scene::importScene(scene, opts, [](std::size_t i, std::size_t n, const std::string& name) {
            std::cout << "layer " << i + 1 << "/" << n << " (" << name << ")\n" << std::flush;
        });
    } catch (const bw::scene::SceneError& e) {
        throw Failure{scene + ": " + e.what() + "\n" + out + ": not written"};
    }
    if (fix.bake)
        bw::scene::bake(p);
    bw::scene::stamp(p, fix.stamp);
    writeProject(p, out, directory);
    std::cout << p.layers.size() << (p.layers.size() == 1 ? " layer, " : " layers, ") << p.layers.width() << "x"
              << p.layers.height() << "\n";
    return kOk;
}

// The IRC options export --irc and render --irc take: --budget, --fit, --pad
// and --swap-shades.
struct IrcArgs {
    std::optional<std::size_t> budget;
    // --colours: the mIRC colours sent; none, a project's own under the IRC
    // target (sentIrcColours), else 99
    std::optional<bw::IrcColours> colours;
    bool fit = false;
    bw::irc::EncodeOptions opts;  // pad and swapShades; fit is set by ircSend
};

// What export --irc writes and render --irc shows, worked out in one place
// so the two agree: the piece, the budget (`budget` if given, else a
// project's own, project::IrcBudget, else EFnet / WeeChat's) and the encoder
// options, with `fit` fitting to that budget.
struct IrcSend {
    bw::AnsiDocument doc;
    std::size_t budget = 0;
    bw::irc::EncodeOptions opts;
};

IrcSend ircSend(const std::string& in, const IrcArgs& args) {
    IrcSend s;
    std::optional<std::size_t> budget = args.budget;
    if (bw::project::isProject(in)) {
        const bw::project::Project p = loadProject(in);
        budget = budget.value_or(p.ircBudget.bytes);
        s.doc = sentDocument(p, args.colours.value_or(bw::sentIrcColours(p.target, p.ircColours)));
    } else {
        // a flat file holds only the 16 (the parser quantises any other
        // colour as it reads it), so 16 colours change nothing
        s.doc = load(in);
    }
    s.budget = budget.value_or(bw::irc::kEfnetWeechatBudget);
    s.opts = args.opts;
    s.opts.fit = args.fit ? s.budget : 0;
    return s;
}

// Writes the piece as IRC text and reports every line's size in bytes, as
// the row encoder writes it with the options (ircSend) and as WeeChat's
// ascii script sends it (irc::txtSentBytes: it writes each line again, with
// two-digit colour codes, #364). A line the script sends over the budget is
// listed and nothing is written, unless allowOver. The cells IRC text writes
// as '?' (irc::unwritable) are warned of; they don't stop it.
int exportIrc(const std::string& in, const std::string& out, const IrcArgs& args, bool allowOver) {
    refuseSameFile(in, out);
    const IrcSend send = ircSend(in, args);
    const bw::AnsiDocument& doc = send.doc;
    const std::size_t limit = send.budget;
    const bool fit = args.fit;
    const bw::irc::EncodeOptions& opts = send.opts;
    std::vector<std::string> lines;
    std::size_t longest = 0, longestSent = 0, fitted = 0;
    int merged = 0;
    std::cout << "line  bytes   sent\n";
    for (bw::irc::Line& l : bw::irc::exportLines(doc.canvas, opts)) {
        const std::size_t n = l.text.size(), sent = bw::irc::txtSentBytes(l.text, limit);
        longest = std::max(longest, n);
        longestSent = std::max(longestSent, sent);
        std::cout << std::setw(4) << lines.size() + 1 << "  " << std::setw(5) << n << "  " << std::setw(5) << sent;
        if (sent > limit)
            std::cout << "  over by " << sent - limit;
        if (l.merged > 0) {
            std::cout << "  fit: " << l.merged << (l.merged == 1 ? " cell" : " cells") << " merged";
            merged += l.merged;
            ++fitted;
        }
        std::cout << "\n";
        lines.push_back(std::move(l.text));
    }
    const auto over = bw::irc::txtOverBudget(lines, limit);
    std::cout << lines.size() << (lines.size() == 1 ? " line" : " lines") << ", longest " << longest
              << " bytes, " << longestSent << " sent, budget " << limit << ": " << over.size() << " over\n";
    if (fit)
        std::cout << "fit: " << merged << (merged == 1 ? " cell" : " cells") << " merged in " << fitted
                  << (fitted == 1 ? " line" : " lines") << "\n";
    // what IRC text writes as '?', as a UTF-8 .ans's losses are told; the
    // lines are written all the same
    for (const std::string& w : bwcli::ircUnwritableWarnings(bw::irc::unwritable(doc.canvas)))
        std::cerr << "blockwright-cli: warning: " << in << ": " << w << "\n";
    if (!over.empty()) {
        for (const auto& o : over)
            std::cerr << "blockwright-cli: " << in << ": line " << o.line << " is " << o.bytes
                      << " bytes as WeeChat's ascii script sends it" << (o.where.empty() ? "" : " " + o.where)
                      << ", over the " << limit << "-byte budget by " << o.bytes - limit << "\n";
        if (!allowOver) {
            std::cerr << "blockwright-cli: " << out
                      << ": not written: lines over the budget (--allow-over-budget writes it anyway)\n";
            return kOverBudget;
        }
    }
    save(out, bw::irc::joinLines(lines));
    return kOk;
}

// Writes the piece as a Micron page (micron.h): what export --irc reads, the
// cells as the editor shows them (a project's composite as micronDocument
// has it, an RGB cell as it is under either target), in the piece's own
// palette (an XBIN's) and iCE setting. A flat file is opened as the editor
// opens it (openAsEditor): iCE colours off with a bright background anyway
// turn on, so that background is written bright, as the app's Export Micron
// writes it. The cells Micron writes as '?' are warned of; they don't stop it.
int exportMicron(const std::string& in, const std::string& out, const bw::micron::Options& opts) {
    refuseSameFile(in, out);
    bw::AnsiDocument doc;
    if (bw::project::isProject(in)) {
        doc = micronDocument(loadProject(in));
    } else {
        doc = load(in);
        bw::openAsEditor(doc);
    }
    for (const std::string& w : bwcli::micronUnwritableWarnings(bw::micron::unwritable(doc.canvas)))
        std::cerr << "blockwright-cli: warning: " << in << ": " << w << "\n";
    save(out, bw::micron::encode(doc, opts));
    std::cout << doc.canvas.height() << (doc.canvas.height() == 1 ? " line, " : " lines, ") << doc.canvas.width()
              << " columns" << (opts.centre ? ", centred" : "") << (bw::customPalette(doc) ? ", own palette" : "")
              << "\n";
    return kOk;
}

// --encoding's value: auto (nothing), cp437, or utf-8 (or utf8), in any case.
std::optional<bw::TextEncoding> encodingValue(const QString& s) {
    const QString v = s.toLower();
    if (v == QLatin1String("auto"))
        return std::nullopt;
    if (v == QLatin1String("cp437"))
        return bw::TextEncoding::Cp437;
    if (v == QLatin1String("utf-8") || v == QLatin1String("utf8"))
        return bw::TextEncoding::Utf8;
    throw Usage{"--encoding takes auto, cp437 or utf-8, not '" + s.toStdString() + "'"};
}

// --output-encoding's value: cp437, or utf-8 (or utf8), in any case.
bw::TextEncoding outputEncodingValue(const QString& s) {
    const QString v = s.toLower();
    if (v == QLatin1String("cp437"))
        return bw::TextEncoding::Cp437;
    if (v == QLatin1String("utf-8") || v == QLatin1String("utf8"))
        return bw::TextEncoding::Utf8;
    throw Usage{"--output-encoding takes cp437 or utf-8, not '" + s.toStdString() + "'"};
}

// --output-encoding utf-8 is for an .ans: XBIN and BIN are CP437 bytes.
void refuseUtf8Binary(const QString& out) {
    if (gOutputEncoding == bw::TextEncoding::Utf8 &&
        bw::artFormatFor(QFile::encodeName(out).toStdString()) != bw::ArtFormat::Ansi)
        throw Usage{"--output-encoding utf-8 writes an .ans; " + out.toStdString() + " is XBIN or BIN, which are CP437"};
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

// --budget N for export --irc and render --irc, their one parser: 1 to 512
// bytes, the range the app's Budget dialog and a project take
// (irc::validBudget).
std::size_t budgetValue(const QString& s) {
    bool ok = false;
    const qulonglong v = s.toULongLong(&ok);
    if (!ok || v > bw::irc::kMaxBudget || !bw::irc::validBudget(static_cast<std::size_t>(v)))
        throw Usage{"--budget takes " + std::to_string(bw::irc::kMinBudget) + " to " +
                    std::to_string(bw::irc::kMaxBudget) + " bytes"};
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

// With `irc`, the IRC view (irc_view.h) of the lines export --irc writes
// with the same options (ircSend), cut at the budget, and a line on stdout
// saying how many rows the cut reaches.
int render(const std::string& in, const std::string& out, const bw::RenderOptions& opts,
           const std::optional<IrcArgs>& irc, const std::optional<std::optional<bw::Palette>>& palette) {
    refuseSameFile(in, out);
    std::optional<IrcSend> send;
    if (irc)
        send = ircSend(in, *irc);
    bw::AnsiDocument doc = send ? send->doc : load(in);
    if (palette)
        doc.palette = *palette;
    if (doc.sauce && doc.sauce->aspectRatio() == 1)
        std::cerr << "blockwright-cli: note: " << in
                  << ": SAUCE asks for the legacy (stretched) aspect ratio; the PNG has square pixels\n";
    QImage img;
    try {
        if (send) {
            const bw::irc::View view = bw::irc::ircView(doc.canvas, send->budget, send->opts);
            img = bw::renderIrcView(view, bw::glyphWidth(doc, opts), opts.scale);
            const int cut = view.cutRows();
            std::cout << "IRC view: " << cut << (cut == 1 ? " row" : " rows") << " cut at " << send->budget
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

// replay --help: the steps file's format (PLAN §9, blockwright/steps.h).
constexpr const char* kReplayHelp = R"(usage: blockwright-cli replay [options] <steps.json> -o <dir>

Applies a steps file's edits to its start through the editor's own brushes,
shapes, selection and layer operations, and writes the canvas (flattened,
with SAUCE) after each step: NN-<name>.ans, or NN-<name>-before.ans and
NN-<name>-after.ans for a step marked "capture": "before-after". NN is the
step's number from 01, with as many digits as the number of steps has and
never fewer than two (001 to 120 for 120 steps). Nothing is written unless
every step applies. <dir> is made if it isn't there. Every file is made in
memory, written to a temporary beside it as convert writes its output, and
only once all are written moved into place, so a write that fails leaves no
new file; a directory in the way is refused before anything is written. An
existing file is written through a symlink and keeps its permissions, and
one that isn't writable is refused. Files already in <dir> are left as they
are unless this run writes one of the same name: replay a shorter script
into an empty directory to have only its captures. The same file writes the
same bytes every run, but for the date of the SAUCE record a capture gets
when its start has none: today's, or SOURCE_DATE_EPOCH's day in UTC when
that is set (seconds since 1970-01-01 UTC, as import-scene takes it).

Options:
  -o, --output <dir>   where the files go (required)
  --project            also write each capture as NN-<name>.bwp
  --render             also write each capture as NN-<name>.png
  --scale N            --render: integer zoom, 1-8 (default 1)
  --font-width 8|9     --render: glyph width (default: SAUCE, else 8)
  --allow-lossy        write an .ans that loses something (a warning says what)
  --output-encoding cp437|utf-8
                       the .ans files' text (default cp437)
  --user-library <dir> the user library placeStamp steps find stamps in
                       and saveToLibrary steps save to, written once
                       every capture is (made if it isn't there), so a
                       run that fails writes nothing to it; without it
                       a saved stamp is kept for this replay only

A steps file is JSON:

  {"version": 1,
   "start": {"file": "piece.ans"}            .ans, .xb, .bin or a project, relative
                                             to the steps file
          | {"sample": "01-layers-showcase", "variant": "Night"}
          | {"blank": [80, 25]},
   "ice": true,                              a blank start's iCE colours (default on)
   "steps": [{"name": "base-ramp", "action": "gradient", ...}, ...]}

A path in a steps file ("file" here, in openReferenceImage and in
applyTextStyle's "style", and a text step's "tdf") is relative to the
steps file's directory, with / between directories, and .. may leave
that directory. An absolute path (/..., C:...) and one with a \ in it are
refused, so a steps file and the files beside it replay the same anywhere.
A missing or unreadable file stops the replay before any step is applied,
naming the step (or "start") and the path.

Each step has a "name" (1-64 of a-z 0-9 - _, starting with a letter or
digit) and an "action", the editor's name for it. Points are [x, y]: cells,
or for the half-block tools (halfBlock, line, rectangle, ellipse, fill,
eyedropper) pixels, two rows to a cell row. "button": "right" is the right
mouse button (default left).

  tool strokes   shade rampShade form soft despeckle smudge glyph colorize
                 outline rimLight texture spray: "points": [[x, y], ...]
                 (cells; [x, y, pressure] for a pressure of 0-1,
                 which scales what "pressure" says; 1 when left out)
  half-block     halfBlock: "points" (pixels); line, rectangle, ellipse:
                 "from", "to" (pixels); fill, eyedropper: "at" (a pixel)
                 and "sampleMerged" (below)
  clone          the Clone Stamp: "source": [x, y] (a cell, the one
                 Ctrl+click sets, set before the stroke and held for later
                 clone steps), "points" (cells, as a stroke's), "aligned"
                 (below). A stroke needs a source, on its own step or an
                 earlier clone step; a step with a source and no points
                 only sets it. Either button copies the same
  gradient       "from", "to" (cells)
  text           the Text tool: "at" (a cell), "text" (as typed, not
                 NFC-composed, 1-4096 characters; "\n" is Enter, any
                 other control character, U+0000-U+001F, U+007F or
                 U+0080-U+009F, is refused): a click at "at",
                 off every visible, unlocked text layer (a click on one
                 would edit it and is refused), then the text typed
                 there, making a new live text layer "Text N" above the
                 active layer and making it active. Its settings are the
                 Text tool's, set by "textOptions" (below). A step with
                 "textOptions" and no "at" sets them as Tool Options
                 does: on the active text layer's settings, which become
                 the defaults, and on the layer unless it is locked;
                 settings it already shows change nothing
  editText       "text" (as the text step's): the active live text
                 layer's whole text replaced, in its own settings; a
                 locked or hidden layer, or one that isn't live text, is
                 refused, and the text it has already changes nothing
  applyTextStyle "style": a bundled Library text style's name ("chrome",
                 its file's name under the library's text-styles/) or
                 {"file": "my.style.json"} (a Library text style file,
                 up to 1 MiB, read strictly): the style put on the active
                 layer, as the Library dock does. The layer must be live
                 text, cells or pixels; a style that needs iCE colours is
                 refused when they are off, and the style the layer has
                 already changes nothing
  openReferenceImage
                 "file": a PNG or JPEG picture (up to 256 MiB), kept byte
                 for byte; any other format is refused (save it as a
                 PNG). File > Open Reference Image...: a new reference
                 layer "Reference N" on top of the stack, made active,
                 the canvas's width in pixels across with its aspect kept
                 (a JPEG turned as its EXIF orientation says). A PNG over
                 2^26 pixels is refused, as the editor refuses to decode
                 it. Captures don't show it: the .ans holds cells, and a
                 reference layer is only a guide to draw over
  keyboard       "at" (a cell), "text" (NFC-composed, as typing is; a
                 newline starts the next row at the "at" column). What
                 may be typed follows the start's target and .ans
                 encoding, not --output-encoding: under IRC or UTF-8 any
                 cluster a UTF-8 .ans holds, wide ones as a pair; under
                 ANSI with CP437 only CP437 glyphs. Each line is typed as
                 one commit: a line of combining marks alone joins the
                 character before "at"; a line that starts with a mark
                 and has more after it is refused, as the editor does
  move           "by": [dx, dy], the active layer's offset (canvas pixels
                 for a reference layer's picture); a locked or hidden
                 layer is refused
  selection      select "rect": [x0, y0, x1, y1] (corners included),
                 selectAll, deselect, moveSelection "by": [dx, dy], copy,
                 cut, deleteSelection, paste "at" (a cell), flipHorizontal,
                 flipVertical, rotateClockwise, rotateCounterClockwise
  library        placeStamp "stamp" (a Library stamp's name), "at" (a
                 cell), "library": "bundled" | "user" (needed only when
                 both hold the name): the stamp placed as the Library
                 dock places it, in transparent paste mode for that
                 stamp only, and committed as a paste. The user library
                 is --user-library's and what earlier steps saved.
                 saveToLibrary "stamp" (a name of CP437 glyphs),
                 "category", "tags": [...] (optional): Selection > Save
                 to Library..., the selected cells as the canvas shows
                 them saved as a stamp in the user library, replacing
                 one of that name; a "category" naming one of its
                 collections saves into that collection's folder
  layers         newCellsLayer, newPixelLayer (each above the active layer
                 and made active; "rename" names it, else Layer N),
                 deleteLayer, raiseLayer, lowerLayer, setActiveLayer
                 "layer" (a name), layerProperties "rename", "visible",
                 "locked", "alphaLocked"
  canvas         canvasSize "size": [w, h] (1-400 by 1-5000 cells),
                 "anchor": top-left|top|top-right|left|centre|right|
                 bottom-left|bottom|bottom-right (top-left): Format >
                 Canvas Size..., applied as the dialog applies it; the
                 selection moves with the cells and is cut to the canvas
  palette        palette "preset": cga|cga-yellow|xterm|windows|campbell|
                 mirc or "colours": [16 "#rrggbb"]: Colours > Palette,
                 the piece's palette, no cell changed; fitToPalette, the
                 same keys: Colours > Fit to Palette, every cells, pixels
                 and plain text layer mapped to the nearest entries, the
                 colours its style names too, a note counting the cells
                 that show another colour and the layers left;
                 remapColours "map": "3=11,11=3" (or 16 hex digits),
                 "scope": selection|layer|document (layer): Colours >
                 Remap Colours...; snapMicronColours, no keys: Colours >
                 Snap to Micron Colours, a Micron piece's RGB colours in
                 the selection (else the active layer) to the nearest
                 twelve-bit colour

  view           the window's view, for blockwright --screenshot only:
                 "zoom" 1-8, "scrollTo" [x, y] (a cell on the canvas, put
                 at the view's top left), "ircFit", "ircPad",
                 "ircSwapShades" (booleans: View > IRC's Fit, Pad, Swap
                 Shades), "guides": none|80x25|80x40|80x50 (View >
                 Guides); at least one. This command has no window and
                 refuses a file with a view step (exit status 1)

Two keys are the window's, read by blockwright --screenshot and passed over
here: a top-level "window": [w, h], the window's size in pixels (320-4096
a side; 1280 x 800 without one), and a step's

  "screenshot": {"of": "window" | a dock or widget's object name,
                 "crop": {"cells": [x, y, w, h]} | {"pixels": [x, y, w, h]},
                 "when": "before" | "after" | "both", "scale": 1 | 2,
                 "pointer": true | false}

each key optional (the whole window after the step, at scale 1, with no
pointer). A crop in cells needs "of" to hold the canvas; one in pixels is in
"of"'s pixels. "scale": 2 draws the picture at device pixel ratio 2, twice
the pixels each way, from the window laid out at the same size. "pointer":
true draws a mouse pointer at the step's last point ("to", "at", the last of
"points", a select "rect"'s bottom-right cell), or at its first in a picture
before the step. A clone step's "source" is clicked before its stroke: the
pointer is there before the step, and after it too when the step has no
"points". A step with none, or a point the picture doesn't show, is refused.

A tool step without points only sets Tool Options. Any step but a view,
canvasSize or palette step may set Tool Options, which hold for the steps after it (but
for "aligned", which only clone takes, "sampleMerged", which only fill and
eyedropper take, and "textOptions", which only text takes):

  "fg", "bg" 0-15 (7 on 0)   "size" 1-256 (1)   "tip" square|round
  "strength" 0-1 (1)         "light" N|NE|E|SE|S|SW|W|NW (NW)
  "pressure" none|size|strength (none): what a point's pressure scales
  "ramp" a name or [palette indices, dark to light] (fire)
  "symmetry" none|mirrorX|mirrorY|mirrorXY or
             {"mode": "radial", "folds": 2-8, "centre": [x, y]} (a cell
             on the canvas)
  "form" {"shape": dome|cylinder|plane, "rim": bool, "specular": bool}
  "hardness" 0-1 (soft)      "outline" ramp|selection    "keepLint" bool
  "glyph" one printable character (no C0, DEL or C1 control)
  "filled" bool (rectangle, ellipse)
  "pencil" 4|8 (halfBlock)   "pasteMode" normal|transparent|under
  "gradient" {"shape": linear|radial, "banding": cells|bayer,
              "from": rung, "to": rung}
  "texture" {"kind": scales|foliage|hoard|brick|noise, "density", "seed"}
  "spray" {"kind": stars|haze|smoke, "density", "seed"}
  "aligned" bool (on): the clone stamp's Aligned, which only a clone step
             takes. On, the first stroke after a "source" sets the
             distance every later stroke copies at; off, every stroke
             copies from the source again
  "sampleMerged" bool (off): Sample merged. Off, fill floods the active
             layer's pixels and the eyedropper picks from it (nothing
             from a transparent cell or pixel); on, both read every
             visible layer as the canvas shows it, and fill paints the
             region it found there into the active layer
  "textOptions" {"face": block|vga|mini (a bundled face) or
             {"tdf": "fonts/x.tdf", "font": NAME, "index": 0-255} (a
             TheDraw font: the .tdf file, the font's name in it, and its
             place in the file, needed only when two fonts share the
             name), "case": as-typed|capitals, "letterSpacing",
             "lineSpacing" 0-64,
             "align": left|centre|right, "wrap" 0-65535 (0 for none),
             "colour" 0-15, "bigStyle": none|shadow|3d|outline|block|
             rainbow, "bigColours" [1 to 16 of 0-15]}: the Text tool's
             settings, which only a text step takes; each key optional,
             the rest kept. As in Tool Options, a big style picked puts
             its own colours in and draws in the block face, so under one
             "face" other than block and "colour" are refused, and
             "bigColours" needs one. A TheDraw font's file is copied
             into the project (fonts/<sha256>.tdf) when a layer is set in
             it, so a --project capture draws without the steps file's
             fonts. A file that isn't a TheDraw font stops the replay,
             naming the step; a name it doesn't hold (or that two of its
             fonts share, with no "index") and an index past its fonts or
             naming another font stop it naming the step and listing the
             fonts the file holds; and a step that would leave the text
             layers using over 16 font files, more than a project holds,
             stops it naming the step

The editor opens on the bottom layer, with nothing selected. A step that
can't be applied (an unknown action or key, a value out of range, a point
off the canvas, no selection to transform, a layer that can't take the
edit, a symmetry centre off the canvas) stops the replay with exit status
1 and a message naming the step.
)";

struct ReplayArgs {
    std::string outDir;
    bool project = false;
    std::optional<bw::RenderOptions> render;
    bool allowLossy = false;
    // the date of the SAUCE record a capture with none gets (SOURCE_DATE_EPOCH)
    std::optional<std::string> today;
    // --user-library (#431)
    std::optional<std::string> userLibrary;
};

// SOURCE_DATE_EPOCH's day in UTC (bw::sauceDateFromEpoch), which import-scene
// and replay take for today: nothing when it is unset or empty, and a usage
// error when it is something else.
std::optional<std::string> sourceDateEpochDay() {
    const QByteArray epoch = qgetenv("SOURCE_DATE_EPOCH");
    if (epoch.isEmpty())
        return {};
    std::optional<std::string> day = bw::sauceDateFromEpoch(epoch.toStdString());
    if (!day)
        throw Usage{"SOURCE_DATE_EPOCH is seconds since 1970-01-01 UTC as digits, up to the end of "
                    "9999, not '" + epoch.toStdString() + "'"};
    return day;
}

// replay <steps.json> -o <dir>: every capture made in memory first, then
// written, so a step that fails writes nothing and a write that fails
// leaves no new file (saveAll).
int replay(const std::string& stepsPath, const ReplayArgs& a) {
    namespace fs = std::filesystem;
    bw::steps::Script script;
    try {
        script = bw::steps::parse(readFile(stepsPath, 16 << 20));
    } catch (const bw::steps::StepsError& e) {
        throw Failure{stepsPath + ": " + e.what()};
    }
    // only the window has a view (#398): refused before the start is read,
    // as a bad step is
    if (const bw::steps::Step* v = bwcli::replay::viewStep(script))
        throw Failure{stepsPath + ": " + bw::steps::stepLabel(*v) +
                      ": view sets the window's zoom, scroll, guides and IRC options, and blockwright-cli replay has no "
                      "window; replay this file with blockwright --screenshot"};
    const auto native = [](const QString& p) { return fs::path(QFile::encodeName(p).toStdString()); };
    const fs::path binDir = bw::library::realBinDir(native(QCoreApplication::applicationFilePath()),
                                                    native(QCoreApplication::applicationDirPath()));
    const fs::path samplesDir = bw::samples::bundledDir(binDir);
    bwcli::replay::Outcome outcome;
    // what reading the start changed, said before any step runs, whether or
    // not a step then fails: the read happened either way (#370)
    bw::project::Project startProject;
    try {
        const fs::path stepsDir = fs::path(stepsPath).parent_path();
        startProject = bwcli::replay::startSaying(script, stepsDir, samplesDir, std::cerr, "blockwright-cli", stepsPath);
        // every file a step reads, before any step runs (#422)
        bwcli::replay::load(script, stepsDir, bw::library::bundledLibraryDir(binDir),
                            a.userLibrary ? fs::path(*a.userLibrary) : fs::path());
    } catch (const Failure& f) {
        throw Failure{stepsPath + ": " + f.message};
    }
    try {
        outcome = bwcli::replay::run(script, std::move(startProject));
    } catch (const Failure& f) {
        throw Failure{stepsPath + ": " + f.message};
    }
    for (const std::string& n : outcome.notes)
        std::cerr << "blockwright-cli: note: " << stepsPath << ": " << n << "\n";

    const fs::path dir(a.outDir);
    std::vector<std::pair<std::string, std::string>> files;  // path, bytes
    for (const bwcli::replay::Capture& c : outcome.captures) {
        const std::string base = (dir / c.stem).string();
        const bw::AnsiDocument doc = bwcli::replay::document(c.project, a.today);
        const bw::AnsiOutput res = bw::writeArt(doc, bw::ArtFormat::Ansi, gOutputEncoding);
        warnLoss(base + ".ans", res);
        if (res.lossy() && !a.allowLossy)
            throw Failure{base + ".ans: not written: the conversion loses data (--allow-lossy writes it anyway)"};
        files.push_back({base + ".ans", res.bytes});
        if (a.project) {
            try {
                files.push_back({base + ".bwp", bw::project::zipBytes(c.project)});
            } catch (const bw::project::ProjectError& e) {
                throw Failure{base + ".bwp: " + e.what()};
            }
        }
        if (a.render) {
            QImage img;
            try {
                img = bw::renderDocument(doc, *a.render);
            } catch (const std::length_error&) {
                throw Failure{base + ".png: not written: the image would be too large"};
            }
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            QImageWriter w(&buf, "png");
            if (!w.write(img))
                throw Failure{base + ".png: not written: PNG encoding failed: " + w.errorString().toStdString()};
            files.push_back({base + ".png", std::string(png.constData(), static_cast<size_t>(png.size()))});
        }
    }
    // refused before <dir> is made, so a refused run leaves nothing, as a
    // failed step does
    if (const auto limit = bwcli::tooFewOpenFiles(files.size()))
        throw Failure{"not written: " + std::to_string(files.size()) + " outputs need " +
                      std::to_string(files.size() + bwcli::kSpareOpenFiles) + " open files at once (one each and " +
                      std::to_string(bwcli::kSpareOpenFiles) + " to spare), more than the open-file limit of " +
                      std::to_string(*limit)};
    std::error_code ec;
    if (fs::exists(dir, ec) ? !fs::is_directory(dir, ec) : !fs::create_directories(dir, ec))
        throw Failure{a.outDir + ": " + (ec ? ec.message() : std::string("not a directory"))};
    bwcli::saveAll(files);
    // the stamps saved, once every capture is written (#431)
    for (const bwcli::replay::SavedStamp& saved : outcome.saved) {
        if (!a.userLibrary) {
            std::cerr << "blockwright-cli: note: " << stepsPath << ": " << saved.step
                      << ": the stamp is kept for this replay only (--user-library <dir> writes it)\n";
            continue;
        }
        try {
            bwcli::replay::writeSaved(saved, *a.userLibrary);
        } catch (const Failure& f) {
            // the load pass checked the library, so this is rare: a full
            // disk, a folder changed under the replay
            throw Failure{stepsPath + ": " + f.message + " (the captures are written; this stamp and any saved after it "
                          "are not)"};
        }
    }
    return kOk;
}

// The whole lint of the piece (lint_rules.h) with its own iCE colours, for
// `target`, else a project's own, else ANSI, and under IRC for `colours`,
// else a project's own, else 99: one line per finding and a summary, or
// JSON. Findings but notes exit kFindings.
int lint(const std::string& in, bwcli::LintCliOptions o, std::optional<bw::Target> target,
         std::optional<bw::IrcColours> colours) {
    bw::AnsiDocument doc;
    if (bw::project::isProject(in)) {
        const bw::project::Project p = loadProject(in);
        o.target = target.value_or(p.target);
        o.colours = bw::sentIrcColours(o.target, colours.value_or(p.ircColours));
        // the cells unfolded, so irc-fold can count the ones 16 colours fold
        doc = bw::richTarget(o.target) ? sentDocument(p) : bw::project::toAnsi(p);
    } else {
        o.target = target.value_or(bw::Target::Ansi);
        o.colours = bw::sentIrcColours(o.target, colours.value_or(bw::IrcColours::Mirc99));
        doc = load(in);
    }
    const bw::lint::LintReport r = bwcli::lintDocument(doc, o);
    std::cout << (o.json ? bwcli::lintJson(r) : bwcli::lintText(r));
    return r.clean() ? kOk : kFindings;
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

void listCollections(const std::filesystem::path& lib);

// Text as library list, delete and remove print it: each control
// character (a tab or newline in a folder's name) as U+FFFD, so a path
// stays one field of one line.
std::string printable(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            out += "\xEF\xBF\xBD";
        else
            out += c;
    }
    return out;
}

std::string listPath(const std::filesystem::path& p) { return printable(p.string()); }

// library list [dir]: one entry a line, tab-separated: its kind (stamp or
// text-style), category, name, width x height, the widest row's IRC bytes,
// "ice" for a text style that needs iCE colours ("-" otherwise), tags
// (comma-separated: a tag never holds a comma or a tab, and a category or
// name never holds a tab or newline), path (each control character in it,
// a tab or newline in a folder's name, printed as U+FFFD). A text style's
// size and bytes are its thumbnail sample's, drawn with iCE colours.
// Without a dir, the bundled library and then the user library (or --dir
// alone): in each, the stamps and then the text styles. The user library
// (or --dir) starts with its collections, empty ones included, one a line:
// "collection", name, how many stamps, path. Entries that can't be read, and
// a user library whose collections can't be listed, are warnings on stderr;
// the exit status is still 0.
int libraryList(const std::optional<std::string>& dir, const std::optional<std::filesystem::path>& userDir) {
    std::vector<std::filesystem::path> dirs;
    if (dir) {
        dirs.push_back(libraryDir(*dir));
    } else if (userDir) {
        dirs.push_back(*userDir);
    } else {
        // Beside the real binary, not a symlink to it.
        const auto native = [](const QString& p) { return std::filesystem::path(QFile::encodeName(p).toStdString()); };
        dirs.push_back(bw::library::bundledLibraryDir(bw::library::realBinDir(
            native(QCoreApplication::applicationFilePath()), native(QCoreApplication::applicationDirPath()))));
        dirs.push_back(bw::library::userLibraryDir());
    }
    for (const auto& d : dirs) {
        // The user library's collections first, empty ones included.
        if (!dir && d == dirs.back())
            listCollections(d);
        const bw::library::ScanResult r = bw::library::scan(d);
        for (const bw::library::Stamp& s : r.stamps)
            std::cout << "stamp\t" << s.meta.category << "\t" << s.displayName() << "\t" << s.art.canvas.width()
                      << "x" << s.art.canvas.height() << "\t" << s.meta.ircBytes << "\t-\t"
                      << joined(s.meta.tags, ",") << "\t" << listPath(s.path) << "\n";
        for (const bw::library::TextStyleEntry& t : r.styles) {
            const bw::Canvas sample = bw::library::textStyleSample(t.style, true);
            std::cout << "text-style\t" << t.category << "\t" << t.name << "\t" << sample.width() << "x"
                      << sample.height() << "\t" << bw::library::widestRowBytes(sample) << "\t"
                      << (t.iceRequired ? "ice" : "-") << "\t" << joined(t.tags, ",") << "\t" << listPath(t.path)
                      << "\n";
        }
        for (const bw::library::Problem& p : r.problems)
            std::cerr << "warning: " << p.path.string() << ": " << p.message << "\n";
    }
    return kOk;
}

// library check <dir>: reads every stamp, sidecar and text style under dir
// and reports each problem on stderr, plus any stamp whose widest row alone
// is over the IRC byte budget, and any text style whose thumbnail sample
// can't be drawn or has a row over it (library::budgetProblems, as WeeChat's
// ascii script sends the row and as it is written). Exit 1 if there was any.
int libraryCheck(const std::string& dir) {
    const bw::library::ScanResult r = bw::library::scan(libraryDir(dir));
    std::size_t problems = r.problems.size();
    for (const bw::library::Problem& p : r.problems)
        std::cerr << p.path.string() << ": " << p.message << "\n";
    for (const bw::library::Problem& p : bw::library::budgetProblems(r, bw::irc::kEfnetWeechatBudget)) {
        std::cerr << p.path.string() << ": " << p.message << "\n";
        ++problems;
    }
    std::cout << r.stamps.size() << (r.stamps.size() == 1 ? " stamp, " : " stamps, ") << r.styles.size()
              << (r.styles.size() == 1 ? " text style, " : " text styles, ") << problems
              << (problems == 1 ? " problem\n" : " problems\n");
    return problems ? kBadInput : kOk;
}

// The library the collection subcommands work on: --dir, else the user's.
std::filesystem::path userLibrary(const std::optional<std::filesystem::path>& dir) {
    return dir ? *dir : bw::library::userLibraryDir();
}

// library list's collection lines: "collection", name, how many stamps,
// path, tab-separated. A library that can't be listed is a warning, as an
// unreadable entry is: library list still exits 0.
void listCollections(const std::filesystem::path& lib) {
    std::vector<bw::library::Collection> cs;
    try {
        cs = bw::library::collections(lib);
    } catch (const bw::library::LibraryError& e) {
        std::cerr << "warning: " << e.what() << "\n";
        return;
    }
    for (const bw::library::Collection& c : cs)
        std::cout << "collection\t" << c.name << "\t" << c.stamps << "\t" << listPath(c.dir) << "\n";
}

// A stamp's name typed as UTF-8, as SAUCE holds it: CP437.
std::string cp437Name(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const std::optional<char32_t> cp = bw::nextUtf8(utf8, i);
        if (!cp)
            throw Failure{"library edit: --name isn't UTF-8"};
        const std::optional<std::uint8_t> b = bw::unicodeToCp437(*cp);
        if (!b)
            throw Failure{"library edit: --name holds a character CP437 has no glyph for"};
        out += char(*b);
    }
    return out;
}

std::string stampCount(std::size_t n) { return std::to_string(n) + (n == 1 ? " stamp" : " stamps"); }

// library new <collection>: makes the collection's folder; prints it.
int libraryNew(const std::filesystem::path& lib, const std::string& name) {
    std::cout << bw::library::createCollection(lib, name).dir.string() << "\n";
    return kOk;
}

// What a removal says when the trash refused: nothing went, and how to
// delete it for good instead.
constexpr const char* kPermanentHint = "; run again with --permanent to delete it for good";

// library delete <collection> [--yes] [--permanent]: without --yes, says
// what would go and exits 1; with it, moves the folder to the trash, or
// with --permanent deletes it. A trash that refuses removes nothing (exit 1).
int libraryDelete(const std::filesystem::path& lib, const std::string& name, bool yes, bool permanent) {
    std::optional<bw::library::Collection> c;
    try {
        c = bw::library::findCollection(lib, name);
    } catch (const bw::library::LibraryError& e) {
        throw Failure{"library delete: " + printable(e.what())};
    }
    if (!c)
        throw Failure{"library delete: no collection named \"" + printable(name) + "\" in " + listPath(lib)};
    if (!yes) {
        std::cerr << "blockwright-cli: library delete: this would remove " << listPath(c->dir) << " and its "
                  << stampCount(c->stamps) << "; run again with --yes\n";
        return kBadInput;
    }
    bw::library::Removed r;
    try {
        r = bw::library::deleteCollection(lib, name,
                                          permanent ? bw::library::Removal::Delete : bw::library::Removal::Trash);
    } catch (const bw::library::TrashUnavailable& e) {
        throw Failure{"library delete: " + printable(e.what()) + kPermanentHint};
    } catch (const bw::library::LibraryError& e) {
        throw Failure{"library delete: " + printable(e.what())};
    }
    std::cout << (r.trashed ? "moved to the trash\t" : "deleted\t") << listPath(c->dir) << "\t"
              << stampCount(r.stamps) << "\n";
    return kOk;
}

// library rename <collection> <new name>: prints the collection's folder.
int libraryRename(const std::filesystem::path& lib, const std::string& from, const std::string& to) {
    std::cout << bw::library::renameCollection(lib, from, to).dir.string() << "\n";
    return kOk;
}

// library add <collection> <file.ans|folder>...: each .ans (a folder's own
// .ans files, not its subfolders'; not hidden ones) added as a stamp, one
// line each: added, name, path. One that can't be added, or a folder with no
// .ans file or one that can't be listed, is an error on stderr; the rest are
// still added, and the exit status is 1.
int libraryAdd(const std::filesystem::path& lib, const std::string& collection,
               const std::vector<std::filesystem::path>& inputs) {
    if (!bw::library::findCollection(lib, collection))
        throw Failure{"library add: no collection named \"" + printable(collection) + "\" in " + listPath(lib)};
    int status = kOk;
    const auto refuse = [&](const std::filesystem::path& file, std::string_view why) {
        std::cerr << "blockwright-cli: library add: " << listPath(file) << ": " << printable(why) << "\n";
        status = kBadInput;
    };
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::path& in : inputs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(in, ec)) {
            files.push_back(in);
            continue;
        }
        try {
            std::vector<std::filesystem::path> found = bw::library::folderArt(in);
            if (found.empty())
                refuse(in, "no .ans files in the folder");
            files.insert(files.end(), found.begin(), found.end());
        } catch (const std::exception& e) {
            refuse(in, e.what());
        }
    }
    // one scan of the library for all of them (addStamps), not one per file
    for (const bw::library::AddOutcome& o : bw::library::addStamps(lib, collection, files)) {
        if (o.added)
            std::cout << "added\t" << printable(bw::cp437ToUtf8(o.added->name)) << "\t" << listPath(o.added->path)
                      << "\n";
        else
            refuse(o.file, o.error);
    }
    return status;
}

// library remove <stamp.ans>... --yes [--permanent]: each stamp's .ans and
// sidecar moved to the trash, or with --permanent deleted; one line each,
// the path as library list prints it. Without --yes, says what would go
// (and why any of them wouldn't) and exits 1. A trash that refuses removes
// nothing.
int libraryRemove(const std::filesystem::path& lib, const std::vector<std::filesystem::path>& stamps, bool yes,
                  bool permanent) {
    if (!yes) {
        for (const std::filesystem::path& s : stamps) {
            try {
                bw::library::requireRemovable(lib, s);
                std::cerr << "blockwright-cli: library remove: this would remove " << listPath(s) << "\n";
            } catch (const bw::library::LibraryError& e) {
                std::cerr << "blockwright-cli: library remove: " << printable(e.what()) << "\n";
            }
        }
        std::cerr << "blockwright-cli: library remove: run again with --yes\n";
        return kBadInput;
    }
    int status = kOk;
    for (const std::filesystem::path& s : stamps) {
        try {
            const bw::library::Removed r = bw::library::removeStamp(
                lib, s, permanent ? bw::library::Removal::Delete : bw::library::Removal::Trash);
            std::cout << (r.trashed ? "moved to the trash\t" : "deleted\t") << listPath(s) << "\n";
        } catch (const bw::library::TrashUnavailable& e) {
            std::cerr << "blockwright-cli: library remove: " << printable(e.what()) << kPermanentHint << "\n";
            status = kBadInput;
        } catch (const bw::library::LibraryError& e) {
            std::cerr << "blockwright-cli: library remove: " << printable(e.what()) << "\n";
            status = kBadInput;
        }
    }
    return status;
}

// An option as it is typed: `-o` for a one-letter name, `--name` for a long one.
std::string optionName(const QCommandLineOption& o) {
    const std::string n = o.names().first().toStdString();
    return (n.size() == 1 ? "-" : "--") + n;
}

} // namespace

// render --irc's entry in the command list spells these out.
static_assert(bw::irc::kMinBudget == 1 && bw::irc::kMaxBudget == 512 && bw::irc::kEfnetWeechatBudget == 407,
              "update render --irc in the --help command list");

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("blockwright-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Headless Blockwright.\n\n"
        "Commands:\n"
        "  info <file>          dimensions and SAUCE fields\n"
        "  convert <in> <out>   read an .ans and write it back out as .ans, XBIN\n"
        "                       (<out> named .xb) or BIN (.bin); <out> may be\n"
        "                       <in>, which is rewritten in place (export and\n"
        "                       render refuse to write over their input); a\n"
        "                       .bwp <out> is refused: pack writes a project\n"
        "  render <in> <out>    draw an .ans as a PNG in its own palette (an XBIN's\n"
        "                       or a project's) or the one --palette gives, with\n"
        "                       the IBM VGA font; a .bwp <out> is refused\n"
        "  remap --map M <in> <out>\n"
        "                       map the cells' colour indices (3=11,11=3 swaps 3\n"
        "                       and 11; or 16 hex digits, digit n what n becomes)\n"
        "                       and write as convert writes\n"
        "  remap --fit-palette P <in> <out>\n"
        "                       give the piece palette P and map each index to\n"
        "                       the entry of P nearest its colour, so it looks the\n"
        "                       same as near as P allows (the cells that don't are\n"
        "                       counted); --fit-palette cga before a .bin or .ans\n"
        "                       save keeps an XBIN's look\n"
        "  export --irc <in> <out>\n"
        "                       write IRC text (mIRC colour codes) and report each\n"
        "                       line's size in bytes; a project's own budget is\n"
        "                       the default; a .bwp <out> is refused. --colours\n"
        "                       16 sends only mIRC colours 0-15, any other as the\n"
        "                       nearest of them; 99 all (default: a project's\n"
        "                       own under the IRC target, else 99)\n"
        "  export --micron <in> <out>\n"
        "                       write a Micron page (.mu) for NomadNet: the piece\n"
        "                       in its own colours, left-aligned or --centre,\n"
        "                       with a #!c= cache header if --cache S is given;\n"
        "                       a .bwp <out> is refused\n"
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
        "                       named ramp and the craft pairs), pixel noise and\n"
        "                       the craft rules; one line per finding (x,y kind\n"
        "                       ramp message) and a summary, or --json. Exits 4\n"
        "                       when there are findings other than notes. --target\n"
        "                       irc (default: a project's own, else ansi) turns\n"
        "                       no-cp437 off and counts what an .ans save drops\n"
        "                       instead (ans-loss, a note: printed with \"note:\",\n"
        "                       it exits 0); wasted-format flags bold, italic or\n"
        "                       underline on a cell where it draws nothing.\n"
        "                       --colours 16 under --target irc (default: a\n"
        "                       project's own) notes the cells IRC text in 16\n"
        "                       colours sends as the nearest of 0-15 (irc-fold)\n"
        "                       --target micron adds two notes: micron-colour\n"
        "                       (colours written as truecolour, `FTrrggbb or\n"
        "                       `BTrrggbb, not three digits) and micron-glyph\n"
        "                       (glyphs a page writes as '?', such as code\n"
        "                       points NomadNet strips)\n"
        "  render --irc <in> <out.png>\n"
        "                       draw the IRC view: the mIRC palette, what IRC\n"
        "                       drops, and each line cut at --budget bytes, 1 to\n"
        "                       512 (default: a project's own, else 407); --fit,\n"
        "                       --pad, --swap-shades and --colours apply as in\n"
        "                       export --irc\n"
        "  library list [dir]   list the stamps and text styles in dir (default:\n"
        "                       the bundled and user libraries): kind, category,\n"
        "                       name, size, widest IRC row in bytes, ice when a\n"
        "                       text style needs iCE colours, tags, path;\n"
        "                       tab-separated (a text style's size and bytes are\n"
        "                       its thumbnail sample's)\n"
        "  library check <dir>  check every stamp, sidecar and text style in dir,\n"
        "                       drawing each text style's sample\n"
        "  library new <collection>\n"
        "                       make a collection: a folder in the user library\n"
        "                       (or --dir), named for the collection\n"
        "  library delete <collection> --yes [--permanent]\n"
        "                       move a collection's folder and its stamps to the\n"
        "                       trash, or with --permanent delete them; where the\n"
        "                       trash refuses, nothing goes and it exits 1; without\n"
        "                       --yes it says what would go and exits 1. Neither\n"
        "                       this nor new, rename, add, remove or edit changes\n"
        "                       the bundled library\n"
        "  library rename <collection> <new name>\n"
        "                       rename a collection, its folder and its stamps'\n"
        "                       category\n"
        "  library add <collection> <file.ans|folder>...\n"
        "                       copy stamps, or .ans files (a folder's own, not\n"
        "                       hidden ones or its subfolders'), into a collection;\n"
        "                       one without a sidecar is named from its SAUCE\n"
        "                       title, else its file name; a folder with no .ans\n"
        "                       file is refused and the rest are added\n"
        "  library remove <stamp.ans>... --yes [--permanent]\n"
        "                       move a stamp in the user library (or --dir) and its\n"
        "                       sidecar to the trash, or with --permanent delete\n"
        "                       them\n"
        "  library edit <stamp.ans> [--name N] [--category C] [--tags t1,t2]\n"
        "                       rename a stamp or change its category or tags; a\n"
        "                       category that is another collection's name moves\n"
        "                       it there. library list shows the user library's\n"
        "                       collections (and --dir's) first: collection, name,\n"
        "                       stamps, path\n"
        "  pack <in> <out.bwp>  write an .ans or a project as a project zip\n"
        "  unpack <in> <dir>    write an .ans or a project as a project directory;\n"
        "                       <dir> must be empty or a project already. A\n"
        "                       project keeps an XBIN's palette but can't hold its\n"
        "                       font: pack and unpack drop it only with\n"
        "                       --allow-lossy.\n"
        "                       --target ansi|irc|micron sets the project's target\n"
        "                       and --colours 16|99 its IRC colours (default: a\n"
        "                       project's own, else ansi and 99)\n"
        "  snapshot <project>   save the flattened composite as the project's next\n"
        "                       round (snapshots/rN.ans) and print rN\n"
        "  add-generator <project> <dir>\n"
        "                       add a generator layer on top, from a directory\n"
        "                       with a generator.json (and params.json, scripts),\n"
        "                       run it once and save the project\n"
        "  run-generators <project>\n"
        "                       run the project's visible generator layers (or\n"
        "                       each --layer, hidden or not) again and save their\n"
        "                       output; this runs the programs their\n"
        "                       generator.json names\n"
        "  import-scene <scene.py> <out>\n"
        "                       open a layered Python scene (one that answers\n"
        "                       --list-layers) as a new project with a generator\n"
        "                       layer for each of its layers, run once; <out> is a\n"
        "                       .bwp zip, or else a directory that doesn't exist or\n"
        "                       is empty. This runs the scene with python3; on\n"
        "                       Windows with the first of py -3, python and python3\n"
        "                       that is Python 3.\n"
        "                       To rebuild a project byte for byte: --bake writes\n"
        "                       the layers as cells without the scene's path,\n"
        "                       --author and --date set SAUCE's, and\n"
        "                       SOURCE_DATE_EPOCH stands in for today\n"
        "  replay <steps.json> -o <dir>\n"
        "                       apply a steps file's edits through the editor's\n"
        "                       tools and write the canvas after each step as\n"
        "                       NN-<name>.ans (and .bwp with --project, .png with\n"
        "                       --render); replay --help gives the format\n\n"
        "Every command that reads an .ans also reads an XBIN (.xb) or BIN (.bin),\n"
        "and a project (a .bwp zip or a project directory) as its flattened\n"
        "composite. convert, shade and remap write XBIN to an <out> named .xb and\n"
        "BIN to one named .bin. Only XBIN and a project hold a palette: an .ans or\n"
        "BIN <out> of a piece with its own palette is lossy (--allow-lossy).\n"
        "convert, shade, export --irc and render refuse an <out> named .bwp:\n"
        "pack writes a project.\n\n"
        "An .ans is read as CP437, or as UTF-8 when it starts with a byte order\n"
        "mark or is all well-formed UTF-8 with a sequence of three bytes or more\n"
        "(the block and box glyphs terminals draw with); --encoding cp437 or\n"
        "utf-8 says which. info prints the one it was read as. convert, shade\n"
        "and replay write an .ans as CP437, or as UTF-8 with --output-encoding\n"
        "utf-8 (SAUCE stays CP437; a byte order mark only when the text has no\n"
        "sequence of three bytes or more). snapshot writes the project's own\n"
        "encoding, or the one --output-encoding gives. Everything else is\n"
        "written as CP437."));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();
    const QCommandLineOption lossyOpt(QStringLiteral("allow-lossy"),
                                      QStringLiteral("convert, shade, remap, pack, unpack, snapshot, replay: write even if something can't be represented"));
    parser.addOption(lossyOpt);
    const QCommandLineOption paletteOpt(
        QStringLiteral("palette"),
        QStringLiteral("convert, render: give the piece this palette, a preset (cga, cga-yellow, xterm, windows, "
                       "campbell, mirc) or a palette file (an XBIN, GIMP .gpl, JASC .pal or hex colours a line); "
                       "the cells stay"),
        QStringLiteral("P"));
    parser.addOption(paletteOpt);
    const QCommandLineOption mapOpt(
        QStringLiteral("map"),
        QStringLiteral("remap: what each colour index becomes, FROM=TO pairs (3=11,11=3) or 16 hex digits"),
        QStringLiteral("M"));
    parser.addOption(mapOpt);
    const QCommandLineOption fitPaletteOpt(
        QStringLiteral("fit-palette"),
        QStringLiteral("remap: give the piece this palette (as --palette) and map each index to its nearest entry"),
        QStringLiteral("P"));
    parser.addOption(fitPaletteOpt);
    const QCommandLineOption scaleOpt(QStringLiteral("scale"),
                                      QStringLiteral("render, replay --render: integer zoom, nearest neighbour (1-8, default 1)"),
                                      QStringLiteral("N"), QStringLiteral("1"));
    parser.addOption(scaleOpt);
    const QCommandLineOption fontWidthOpt(QStringLiteral("font-width"),
                                          QStringLiteral("render, replay --render: glyph width 8 or 9 (default: SAUCE letter spacing, else 8)"),
                                          QStringLiteral("px"));
    parser.addOption(fontWidthOpt);
    const QCommandLineOption ircOpt(QStringLiteral("irc"),
                                    QStringLiteral("export: IRC text with mIRC colour codes; render: the IRC view"));
    parser.addOption(ircOpt);
    const QCommandLineOption budgetOpt(
        QStringLiteral("budget"),
        QStringLiteral("export --irc, render --irc: bytes a line may take, %2 to %3 (default: a project's own, "
                       "else %1, EFnet via WeeChat)")
            .arg(bw::irc::kEfnetWeechatBudget)
            .arg(bw::irc::kMinBudget)
            .arg(bw::irc::kMaxBudget),
        QStringLiteral("bytes"));
    parser.addOption(budgetOpt);
    const QCommandLineOption fitOpt(
        QStringLiteral("fit"),
        QStringLiteral("export --irc, render --irc: fit lines over the budget by merging their least visible colours"));
    parser.addOption(fitOpt);
    const QCommandLineOption padOpt(
        QStringLiteral("pad"),
        QStringLiteral("export --irc, render --irc: keep trailing black cells and blank edge rows, ending rows on a "
                       "black block"));
    parser.addOption(padOpt);
    const QCommandLineOption swapShadesOpt(
        QStringLiteral("swap-shades"),
        QStringLiteral("export --irc, render --irc: let the encoder draw a shade as its opposite with the colours "
                       "reversed"));
    parser.addOption(swapShadesOpt);
    const QCommandLineOption micronOpt(QStringLiteral("micron"),
                                       QStringLiteral("export: a Micron page (.mu) for NomadNet"));
    parser.addOption(micronOpt);
    const QCommandLineOption centreOpt(QStringLiteral("centre"),
                                       QStringLiteral("export --micron: centre the piece on the page"));
    parser.addOption(centreOpt);
    const QCommandLineOption cacheOpt(
        QStringLiteral("cache"),
        QStringLiteral("export --micron: write the page header #!c=S, the seconds a peer may cache the page "
                       "(0: never)"),
        QStringLiteral("S"));
    parser.addOption(cacheOpt);
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
    const QCommandLineOption nameOpt(
        QStringLiteral("name"),
        QStringLiteral("add-generator: the layer's name (default: the directory's); library edit: the stamp's new name"),
        QStringLiteral("name"));
    parser.addOption(nameOpt);
    const QCommandLineOption dirOpt(QStringLiteral("dir"),
                                    QStringLiteral("library: the library to work on (default: the user library)"),
                                    QStringLiteral("dir"));
    parser.addOption(dirOpt);
    const QCommandLineOption yesOpt(QStringLiteral("yes"),
                                    QStringLiteral("library delete, library remove: do it, without asking"));
    parser.addOption(yesOpt);
    const QCommandLineOption permanentOpt(
        QStringLiteral("permanent"),
        QStringLiteral("library delete, library remove: delete for good rather than move to the trash"));
    parser.addOption(permanentOpt);
    const QCommandLineOption categoryOpt(QStringLiteral("category"),
                                         QStringLiteral("library edit: the stamp's new category"),
                                         QStringLiteral("category"));
    parser.addOption(categoryOpt);
    const QCommandLineOption tagsOpt(QStringLiteral("tags"),
                                     QStringLiteral("library edit: the stamp's new tags, comma-separated (empty: none)"),
                                     QStringLiteral("t1,t2"));
    parser.addOption(tagsOpt);
    const QCommandLineOption layerOpt(
        QStringLiteral("layer"),
        QStringLiteral("run-generators: run this generator layer (an id, as info lists them), even a hidden one; "
                       "more than once for several (default: every visible one)"),
        QStringLiteral("id"));
    parser.addOption(layerOpt);
    const QCommandLineOption timeoutOpt(
        QStringLiteral("timeout"),
        QStringLiteral("add-generator, run-generators, import-scene: seconds a generator may run (1-3600, default %1)")
            .arg(std::chrono::duration_cast<std::chrono::seconds>(bw::generator::kDefaultTimeout).count()),
        QStringLiteral("S"));
    parser.addOption(timeoutOpt);
    const QCommandLineOption bakeOpt(
        QStringLiteral("bake"),
        QStringLiteral("import-scene: write each layer as the cells it made, without its script, so the project "
                       "doesn't name the scene's path; its layers can't be run again"));
    parser.addOption(bakeOpt);
    const QCommandLineOption authorOpt(
        QStringLiteral("author"),
        QStringLiteral("import-scene: the SAUCE author, 1 to %1 CP437 characters").arg(bw::Sauce::kAuthorLen),
        QStringLiteral("name"));
    parser.addOption(authorOpt);
    const QCommandLineOption dateOpt(
        QStringLiteral("date"),
        QStringLiteral("import-scene: the SAUCE date, a day as CCYYMMDD (default: SOURCE_DATE_EPOCH's day in UTC "
                       "when it is set, else today)"),
        QStringLiteral("CCYYMMDD"));
    parser.addOption(dateOpt);
    const QCommandLineOption encodingOpt(
        QStringLiteral("encoding"),
        QStringLiteral("info, convert, render, remap, export, shade, lint, pack, unpack: read each .ans input as auto, "
                       "cp437 or utf-8 (default: auto, UTF-8 when it has a byte order mark or is all well-formed "
                       "UTF-8 with a sequence of three bytes or more, else CP437)"),
        QStringLiteral("enc"), QStringLiteral("auto"));
    parser.addOption(encodingOpt);
    const QCommandLineOption outputEncodingOpt(
        QStringLiteral("output-encoding"),
        QStringLiteral("convert, shade, remap, snapshot, replay: write an .ans output as cp437 or utf-8 (default: cp437, "
                       "and for snapshot the project's encoding; SAUCE stays CP437)"),
        QStringLiteral("enc"), QStringLiteral("cp437"));
    parser.addOption(outputEncodingOpt);
    const QCommandLineOption targetOpt(
        QStringLiteral("target"),
        QStringLiteral("lint: lint for ansi, irc or micron (default: a project's own, else ansi); pack, unpack: the "
                       "project's target (default: a project's own, else ansi)"),
        QStringLiteral("target"));
    parser.addOption(targetOpt);
    const QCommandLineOption coloursOpt(
        QStringLiteral("colours"),
        QStringLiteral("export --irc, render --irc: the mIRC colours sent, 16 (0-15, any other as the nearest of "
                       "them) or 99; lint: under --target irc, 16 notes the cells folded; pack, unpack: the "
                       "project's IRC colours (default: a project's own under the IRC target, else 99)"),
        QStringLiteral("count"));
    parser.addOption(coloursOpt);
    const QCommandLineOption outputOpt(QStringList{QStringLiteral("o"), QStringLiteral("output")},
                                       QStringLiteral("replay: the directory the captures go in"),
                                       QStringLiteral("dir"));
    parser.addOption(outputOpt);
    const QCommandLineOption projectOpt(QStringLiteral("project"),
                                        QStringLiteral("replay: also write each capture as a .bwp"));
    parser.addOption(projectOpt);
    const QCommandLineOption renderOpt(QStringLiteral("render"),
                                       QStringLiteral("replay: also write each capture as a PNG"));
    parser.addOption(renderOpt);
    const QCommandLineOption userLibraryOpt(
        QStringLiteral("user-library"),
        QStringLiteral("replay: the user library placeStamp steps find stamps in and saveToLibrary steps save to"),
        QStringLiteral("dir"));
    parser.addOption(userLibraryOpt);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("info, convert, render, remap, export, shade, lint, library, pack, unpack, snapshot, add-generator, run-generators, import-scene or replay"));
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("command arguments"), QStringLiteral("[args...]"));
    if (!parser.parse(QCoreApplication::arguments())) {
        std::cerr << "blockwright-cli: " << parser.errorText().toStdString() << "\n";
        return kUsage;
    }
    if (parser.isSet(versionOpt)) {
        // The version stays the first line, for the scripts that read it
        std::cout << "blockwright-cli " << BLOCKWRIGHT_VERSION << " (" << BLOCKWRIGHT_GIT_COMMIT << ")\n\n"
                  << bw::kLogoText << "\n"
                  << bw::kFontCredit << bw::kUnifontCredit;
        return kOk;
    }
    // Printed here rather than by showHelp(), which on Windows puts the text
    // in a message box when it finds no console, as under a pipe it may not.
    if (parser.isSet(helpOpt)) {
        if (parser.positionalArguments().value(0) == QLatin1String("replay")) {
            std::cout << kReplayHelp;
            return kOk;
        }
        std::cout << bw::kLogoText << "\n" << parser.helpText().toStdString();
        return kOk;
    }

    const QStringList args = parser.positionalArguments();
    auto usage = [](const char* msg) {
        std::cerr << "blockwright-cli: " << msg << "\n"
                  << "usage: blockwright-cli info <file> | convert [--allow-lossy] [--palette P] <in> <out>\n"
                  << "       | render [--scale N] [--font-width 8|9] [--palette P] <in> <out.png>\n"
                  << "       | remap --map M | --fit-palette P [--allow-lossy] <in> <out>\n"
                  << "       | export --irc [--budget N] [--fit] [--pad] [--swap-shades] [--colours 16|99]\n"
                  << "         [--allow-over-budget] <in> <out>\n"
                  << "       | export --micron [--centre] [--cache S] <in> <out.mu>\n"
                  << "       | shade --ramp R [--darken] [--steps N] [--box x0,y0,x1,y1] [--mask <mask>] [--ice] [--allow-lossy] <in> <out>\n"
                  << "       | shade --form dome|cylinder|plane [--glint] | --despeckle | --rim --ramp R [--light N|NE|...|NW]\n"
                  << "         --box x0,y0,x1,y1 and/or --mask <mask> [--ice] [--allow-lossy] <in> <out>\n"
                  << "       | lint [--ramp R]... [--box x0,y0,x1,y1] [--target ansi|irc|micron] [--colours 16|99] [--json] <in>\n"
                  << "       | render --irc [--budget N] [--fit] [--pad] [--swap-shades] [--colours 16|99] [--scale N]\n"
                  << "         [--font-width 8|9]\n"
                  << "         <in> <out.png>\n"
                  << "       | library list [dir] | library list [--dir DIR] | library check <dir>\n"
                  << "       | library new|delete [--yes] [--permanent]|rename|add|remove [--yes] [--permanent]|edit [--dir DIR] ...\n"
                  << "         (--help lists them)\n"
                  << "       | pack [--target ansi|irc|micron] [--colours 16|99] <in> <out.bwp>\n"
                  << "       | unpack [--target ansi|irc|micron] [--colours 16|99] <in> <dir>\n"
                  << "       | snapshot [--allow-lossy] [--output-encoding enc] <project>\n"
                  << "       | add-generator [--name NAME] [--timeout S] <project> <dir>\n"
                  << "       | run-generators [--layer ID]... [--timeout S] <project>\n"
                  << "       | import-scene [--timeout S] [--bake] [--author NAME] [--date CCYYMMDD]\n"
                  << "         <scene.py> <out.bwp|dir>\n"
                  << "       | replay [--project] [--render [--scale N] [--font-width 8|9]] [--allow-lossy]\n"
                  << "         [--user-library <dir>] <steps.json> -o <dir>   (replay --help gives the steps format)\n"
                  << "       | --version\n"
                  << "       info, convert, render, export, shade, lint, pack and unpack take --encoding auto|cp437|utf-8\n"
                  << "       convert, shade, remap, snapshot and replay take --output-encoding cp437|utf-8\n";
        return kUsage;
    };
    if (args.isEmpty())
        return usage("no command given");
    const QString cmd = args.first();
    // Each option belongs to its commands; every other command refuses it.
    const std::pair<const QCommandLineOption*, std::vector<const char*>> owners[] = {
        {&lossyOpt, {"convert", "shade", "remap", "pack", "unpack", "snapshot", "replay"}},
        {&paletteOpt, {"convert", "render"}}, {&mapOpt, {"remap"}}, {&fitPaletteOpt, {"remap"}},
        {&scaleOpt, {"render", "replay"}}, {&fontWidthOpt, {"render", "replay"}},
        {&ircOpt, {"export", "render"}},   {&budgetOpt, {"export", "render"}}, {&overOpt, {"export"}},
        {&micronOpt, {"export"}},          {&centreOpt, {"export"}}, {&cacheOpt, {"export"}},
        {&fitOpt, {"export", "render"}},   {&padOpt, {"export", "render"}}, {&swapShadesOpt, {"export", "render"}},
        {&rampOpt, {"shade", "lint"}},     {&darkenOpt, {"shade"}},  {&stepsOpt, {"shade"}},
        {&boxOpt, {"shade", "lint"}},      {&iceOpt, {"shade"}},     {&jsonOpt, {"lint"}},
        {&formOpt, {"shade"}},             {&glintOpt, {"shade"}},   {&despeckleOpt, {"shade"}},
        {&rimOpt, {"shade"}},              {&lightOpt, {"shade"}},   {&maskOpt, {"shade"}},
        {&nameOpt, {"add-generator", "library"}},
        {&dirOpt, {"library"}},            {&yesOpt, {"library"}},     {&permanentOpt, {"library"}},
        {&categoryOpt, {"library"}},       {&tagsOpt, {"library"}},     {&layerOpt, {"run-generators"}},
        {&timeoutOpt, {"add-generator", "run-generators", "import-scene"}},
        {&bakeOpt, {"import-scene"}},      {&authorOpt, {"import-scene"}}, {&dateOpt, {"import-scene"}},
        {&encodingOpt, {"info", "convert", "render", "remap", "export", "shade", "lint", "pack", "unpack"}},
        {&outputEncodingOpt, {"convert", "shade", "remap", "snapshot", "replay"}},
        {&targetOpt, {"lint", "pack", "unpack"}},
        {&coloursOpt, {"export", "render", "lint", "pack", "unpack"}},
        {&outputOpt, {"replay"}},          {&projectOpt, {"replay"}}, {&renderOpt, {"replay"}},
        {&userLibraryOpt, {"replay"}},
    };
    if (cmd == QLatin1String("info") || cmd == QLatin1String("convert") || cmd == QLatin1String("render") ||
        cmd == QLatin1String("remap") ||
        cmd == QLatin1String("export") || cmd == QLatin1String("shade") || cmd == QLatin1String("lint") ||
        cmd == QLatin1String("library") ||
        cmd == QLatin1String("pack") || cmd == QLatin1String("unpack") || cmd == QLatin1String("snapshot") ||
        cmd == QLatin1String("add-generator") || cmd == QLatin1String("run-generators") ||
        cmd == QLatin1String("import-scene") || cmd == QLatin1String("replay"))
        for (const auto& [o, cmds] : owners) {
            if (!parser.isSet(*o) ||
                std::any_of(cmds.begin(), cmds.end(), [&](const char* c) { return cmd == QLatin1String(c); }))
                continue;
            std::string owner;
            for (std::size_t i = 0; i < cmds.size(); ++i)
                owner += std::string(i == 0 ? "" : i + 1 == cmds.size() ? " or " : ", ") + cmds[i];
            const bool vowel = std::string_view("aeiou").find(owner[0]) != std::string_view::npos;
            return usage((optionName(*o) + " is " + (vowel ? "an " : "a ") + owner +
                          " option, not a " + cmd.toStdString() + " option")
                             .c_str());
        }
    // --target, read the same way for lint, pack and unpack.
    const auto targetValue = [&]() -> std::optional<bw::Target> {
        if (!parser.isSet(targetOpt))
            return std::nullopt;
        const QString v = parser.value(targetOpt);
        if (const std::optional<bw::Target> t = bw::parseTarget(v.toStdString()))
            return t;
        throw Usage{"--target takes ansi, irc or micron, not '" + v.toStdString() + "'"};
    };
    // --colours, read the same way for every command that takes it.
    const auto coloursValue = [&]() -> std::optional<bw::IrcColours> {
        if (!parser.isSet(coloursOpt))
            return std::nullopt;
        const QString v = parser.value(coloursOpt);
        if (const std::optional<bw::IrcColours> c = bw::parseIrcColours(v.toStdString()))
            return c;
        throw Usage{"--colours takes 16 or 99, not '" + v.toStdString() + "'"};
    };
    // export --irc's and render --irc's options, read the same way for both.
    const auto ircArgs = [&] {
        IrcArgs a;
        a.colours = coloursValue();
        if (parser.isSet(budgetOpt))
            a.budget = budgetValue(parser.value(budgetOpt));
        a.fit = parser.isSet(fitOpt);
        a.opts.pad = parser.isSet(padOpt);
        a.opts.swapShades = parser.isSet(swapShadesOpt);
        return a;
    };
    try {
        gEncoding = encodingValue(parser.value(encodingOpt));
        gOutputEncoding = outputEncodingValue(parser.value(outputEncodingOpt));
        if (cmd == QLatin1String("info")) {
            if (args.size() != 2)
                return usage("info takes one file");
            return info(args[1].toStdString());
        }
        if (cmd == QLatin1String("convert")) {
            if (args.size() != 3)
                return usage("convert takes an input and an output file");
            refuseProjectOutput("convert", ".ans, .xb or .bin", args[2]);
            refuseUtf8Binary(args[2]);
            std::optional<std::optional<bw::Palette>> palette;
            if (parser.isSet(paletteOpt))
                palette = paletteValue(parser.value(paletteOpt));
            return convert(args[1].toStdString(), args[2].toStdString(), parser.isSet(lossyOpt), palette);
        }
        if (cmd == QLatin1String("remap")) {
            if (parser.isSet(mapOpt) == parser.isSet(fitPaletteOpt))
                return usage("remap needs one of --map M or --fit-palette P");
            if (args.size() != 3)
                return usage("remap takes an input and an output file");
            refuseProjectOutput("remap", ".ans, .xb or .bin", args[2]);
            refuseUtf8Binary(args[2]);
            std::optional<bw::ColourMap> map;
            std::optional<std::optional<bw::Palette>> fit;
            if (parser.isSet(mapOpt)) {
                std::string error;
                map = bw::parseColourMap(parser.value(mapOpt).toStdString(), &error);
                if (!map)
                    return usage(("--map: " + error).c_str());
            } else {
                fit = paletteValue(parser.value(fitPaletteOpt));
            }
            return remap(args[1].toStdString(), args[2].toStdString(), map, fit, parser.isSet(lossyOpt));
        }
        if (cmd == QLatin1String("render")) {
            if (args.size() != 3)
                return usage("render takes an input .ans and an output .png");
            refuseProjectOutput(parser.isSet(ircOpt) ? "render --irc" : "render", "a PNG", args[2]);
            bw::RenderOptions opts;
            opts.scale = intValue(parser.value(scaleOpt), "scale", 1, bw::kMaxScale);
            if (parser.isSet(fontWidthOpt))
                opts.fontWidth = intValue(parser.value(fontWidthOpt), "font-width", 8, 9);
            if (!parser.isSet(ircOpt))
                for (const QCommandLineOption* o : {&budgetOpt, &fitOpt, &padOpt, &swapShadesOpt, &coloursOpt})
                    if (parser.isSet(*o))
                        return usage((optionName(*o) + " is a render --irc option").c_str());
            std::optional<IrcArgs> irc;
            if (parser.isSet(ircOpt))
                irc = ircArgs();
            // the IRC view draws in mIRC's colours, by index (PLAN §8, Palettes)
            if (irc && parser.isSet(paletteOpt))
                return usage("--palette is not a render --irc option: the IRC view draws mIRC's colours");
            std::optional<std::optional<bw::Palette>> palette;
            if (parser.isSet(paletteOpt))
                palette = paletteValue(parser.value(paletteOpt));
            return render(args[1].toStdString(), args[2].toStdString(), opts, irc, palette);
        }
        if (cmd == QLatin1String("export")) {
            if (parser.isSet(ircOpt) == parser.isSet(micronOpt))
                return usage("export needs one format: --irc or --micron");
            if (args.size() != 3)
                return usage("export takes an input and an output file");
            if (parser.isSet(micronOpt)) {
                for (const QCommandLineOption* o : {&budgetOpt, &fitOpt, &padOpt, &swapShadesOpt, &overOpt, &coloursOpt})
                    if (parser.isSet(*o))
                        return usage((optionName(*o) + " is an export --irc option").c_str());
                refuseProjectOutput("export --micron", "a Micron page", args[2]);
                bw::micron::Options mo;
                mo.centre = parser.isSet(centreOpt);
                if (parser.isSet(cacheOpt))
                    mo.cacheSeconds = unsigned(intValue(parser.value(cacheOpt), "cache", 0, 31536000));
                return exportMicron(args[1].toStdString(), args[2].toStdString(), mo);
            }
            for (const QCommandLineOption* o : {&centreOpt, &cacheOpt})
                if (parser.isSet(*o))
                    return usage((optionName(*o) + " is an export --micron option").c_str());
            refuseProjectOutput("export --irc", "IRC text", args[2]);
            return exportIrc(args[1].toStdString(), args[2].toStdString(), ircArgs(), parser.isSet(overOpt));
        }
        if (cmd == QLatin1String("shade")) {
            if (!parser.isSet(rampOpt))
                return usage("shade needs a ramp: --ramp NAME or --ramp c,c,...");
            if (args.size() != 3)
                return usage("shade takes an input and an output file");
            refuseProjectOutput("shade", ".ans, .xb or .bin", args[2]);
            refuseUtf8Binary(args[2]);
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
            o.encoding = gOutputEncoding;
            std::optional<std::string> mask;
            if (parser.isSet(maskOpt))
                mask = parser.value(maskOpt).toStdString();
            return shade(args[1].toStdString(), args[2].toStdString(), o, mask);
        }
        if (cmd == QLatin1String("library")) {
            const QString sub = args.value(1);
            const std::string s = sub.toStdString();
            // Each subcommand's own options.
            const std::pair<const QCommandLineOption*, std::vector<const char*>> subOwners[] = {
                {&dirOpt, {"list", "new", "delete", "rename", "add", "remove", "edit"}},
                {&yesOpt, {"delete", "remove"}},
                {&permanentOpt, {"delete", "remove"}},
                {&nameOpt, {"edit"}}, {&categoryOpt, {"edit"}}, {&tagsOpt, {"edit"}},
            };
            for (const auto& [o, subs] : subOwners)
                if (parser.isSet(*o) && std::none_of(subs.begin(), subs.end(), [&](const char* x) { return s == x; }))
                    return usage((optionName(*o) + " isn't a library " + (s.empty() ? "" : s + " ") + "option").c_str());
            const auto native = [](const QString& p) {
                return std::filesystem::path(QFile::encodeName(p).toStdString());
            };
            const std::optional<std::filesystem::path> dir =
                parser.isSet(dirOpt) ? std::optional(native(parser.value(dirOpt))) : std::nullopt;
            const auto paths = [&](qsizetype from) {
                std::vector<std::filesystem::path> out;
                for (qsizetype i = from; i < args.size(); ++i)
                    out.push_back(native(args[i]));
                return out;
            };
            try {
                // Bundled stamps are never changed, whatever --dir names.
                for (const char* m : {"new", "delete", "rename", "add", "remove", "edit"})
                    if (s == m)
                        bw::library::refuseBundled(userLibrary(dir));
                if (sub == QLatin1String("list")) {
                    if (args.size() > 3)
                        return usage("library list takes at most one directory");
                    if (args.size() == 3 && dir)
                        return usage("library list takes a directory or --dir, not both");
                    return libraryList(args.size() == 3 ? std::optional(args[2].toStdString()) : std::nullopt, dir);
                }
                if (sub == QLatin1String("check")) {
                    if (args.size() != 3)
                        return usage("library check takes one directory");
                    return libraryCheck(args[2].toStdString());
                }
                if (sub == QLatin1String("new")) {
                    if (args.size() != 3)
                        return usage("library new takes a collection's name");
                    return libraryNew(userLibrary(dir), args[2].toStdString());
                }
                if (sub == QLatin1String("delete")) {
                    if (args.size() != 3)
                        return usage("library delete takes a collection's name");
                    return libraryDelete(userLibrary(dir), args[2].toStdString(), parser.isSet(yesOpt),
                                         parser.isSet(permanentOpt));
                }
                if (sub == QLatin1String("rename")) {
                    if (args.size() != 4)
                        return usage("library rename takes a collection's name and its new name");
                    return libraryRename(userLibrary(dir), args[2].toStdString(), args[3].toStdString());
                }
                if (sub == QLatin1String("add")) {
                    if (args.size() < 4)
                        return usage("library add takes a collection's name and one or more .ans files or folders");
                    return libraryAdd(userLibrary(dir), args[2].toStdString(), paths(3));
                }
                if (sub == QLatin1String("remove")) {
                    if (args.size() < 3)
                        return usage("library remove takes one or more stamps' .ans files");
                    return libraryRemove(userLibrary(dir), paths(2), parser.isSet(yesOpt), parser.isSet(permanentOpt));
                }
                if (sub == QLatin1String("edit")) {
                    if (args.size() != 3)
                        return usage("library edit takes one stamp's .ans file");
                    bw::library::StampEdit edit;
                    if (parser.isSet(nameOpt))
                        edit.name = cp437Name(parser.value(nameOpt).toStdString());
                    if (parser.isSet(categoryOpt))
                        edit.category = parser.value(categoryOpt).toStdString();
                    if (parser.isSet(tagsOpt)) {
                        std::vector<std::string> tags;
                        for (const QString& t : parser.value(tagsOpt).split(QLatin1Char(','), Qt::SkipEmptyParts))
                            tags.push_back(t.trimmed().toStdString());
                        edit.tags = tags;
                    }
                    if (!edit.name && !edit.category && !edit.tags)
                        return usage("library edit needs --name, --category or --tags");
                    std::cout << bw::library::editStamp(userLibrary(dir), native(args[2]), edit).string() << "\n";
                    return kOk;
                }
            } catch (const bw::library::LibraryError& e) {
                std::cerr << "blockwright-cli: library " << s << ": " << e.what() << "\n";
                return kBadInput;
            }
            return usage(sub.isEmpty() ? "library needs a subcommand: list, check, new, delete, rename, add, remove or edit"
                                       : ("unknown library subcommand '" + s + "'").c_str());
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
            return lint(args[1].toStdString(), o, targetValue(), coloursValue());
        }
        if (cmd == QLatin1String("pack") || cmd == QLatin1String("unpack")) {
            if (args.size() != 3)
                return usage(cmd == QLatin1String("pack") ? "pack takes an input and an output .bwp"
                                                          : "unpack takes an input and an output directory");
            return pack(args[1].toStdString(), args[2].toStdString(), cmd == QLatin1String("unpack"),
                        parser.isSet(lossyOpt), targetValue(), coloursValue());
        }
        if (cmd == QLatin1String("snapshot")) {
            if (args.size() != 2)
                return usage("snapshot takes one project");
            return snapshot(args[1].toStdString(), parser.isSet(lossyOpt),
                            parser.isSet(outputEncodingOpt) ? std::optional(gOutputEncoding) : std::nullopt);
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
        if (cmd == QLatin1String("import-scene")) {
            if (args.size() != 3)
                return usage("import-scene takes a scene.py and an output .bwp or directory");
            SceneFix fix;
            fix.bake = parser.isSet(bakeOpt);
            if (parser.isSet(authorOpt)) {
                fix.stamp.author = bw::scene::sauceText(parser.value(authorOpt).toStdString(), bw::Sauce::kAuthorLen);
                // SAUCE pads the field with spaces, so an author of only
                // spaces reads back empty
                if (!fix.stamp.author || fix.stamp.author->find_first_not_of(' ') == std::string::npos)
                    throw Usage{"--author takes 1 to " + std::to_string(bw::Sauce::kAuthorLen) +
                                " printable CP437 characters, not '" + parser.value(authorOpt).toStdString() + "'"};
            }
            if (parser.isSet(dateOpt)) {
                const std::string date = parser.value(dateOpt).toStdString();
                if (!bw::isSauceDate(date))
                    throw Usage{"--date takes a day as CCYYMMDD, such as 20261103, not '" + date + "'"};
                fix.stamp.date = date;
            } else {
                fix.stamp.date = sourceDateEpochDay();
            }
            // SOURCE_DATE_EPOCH only stands in for today: it makes no record
            fix.stamp.record = fix.stamp.author || parser.isSet(dateOpt);
            return importScene(args[1].toStdString(), args[2].toStdString(), runOpts, fix);
        }
        if (cmd == QLatin1String("replay")) {
            if (args.size() != 2)
                return usage("replay takes one steps file");
            if (!parser.isSet(outputOpt))
                return usage("replay needs -o <dir>, the directory the captures go in");
            if (!parser.isSet(renderOpt))
                for (const QCommandLineOption* o : {&scaleOpt, &fontWidthOpt})
                    if (parser.isSet(*o))
                        return usage((optionName(*o) + " is a replay --render option").c_str());
            ReplayArgs a;
            a.outDir = parser.value(outputOpt).toStdString();
            a.project = parser.isSet(projectOpt);
            a.allowLossy = parser.isSet(lossyOpt);
            if (parser.isSet(userLibraryOpt))
                a.userLibrary = parser.value(userLibraryOpt).toStdString();
            // checked before the steps file is read, as import-scene checks it
            // before the scene runs
            a.today = sourceDateEpochDay();
            if (parser.isSet(renderOpt)) {
                bw::RenderOptions opts;
                opts.scale = intValue(parser.value(scaleOpt), "scale", 1, bw::kMaxScale);
                if (parser.isSet(fontWidthOpt))
                    opts.fontWidth = intValue(parser.value(fontWidthOpt), "font-width", 8, 9);
                a.render = opts;
            }
            return replay(args[1].toStdString(), a);
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
    } catch (const std::exception& e) {
        // Backstop (#380): an error a command didn't wrap as a Failure (a
        // core error, a std::filesystem error) ends the run with its message
        // and exit 1, not std::terminate's abort
        std::cerr << "blockwright-cli: " << cmd.toStdString() << ": " << e.what() << "\n";
        return kBadInput;
    }
}
