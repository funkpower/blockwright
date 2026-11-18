#include "blockwright/steps.h"

#include "blockwright/ansi.h"
#include "blockwright/big_text.h"
#include "blockwright/cp437.h"
#include "blockwright/face.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/tdf.h"
#include "blockwright/text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace bw::steps {

namespace {

// What each action takes besides the keys every step does.
struct ActionKeys {
    std::string_view action;
    std::vector<std::string_view> keys;
};

const std::vector<ActionKeys>& actionKeys() {
    static const std::vector<ActionKeys> all = {
        // brushes: a stroke of cells
        {"shade", {"points", "button"}},
        {"rampShade", {"points", "button"}},
        {"form", {"points", "button"}},
        {"soft", {"points", "button"}},
        {"despeckle", {"points", "button"}},
        {"smudge", {"points", "button"}},
        {"glyph", {"points", "button"}},
        {"colorize", {"points", "button"}},
        {"outline", {"points", "button"}},
        {"rimLight", {"points", "button"}},
        {"texture", {"points", "button"}},
        {"spray", {"points", "button"}},
        // the clone stamp: its source (Ctrl+click) and Tool Options' Aligned,
        // which is its alone (#418)
        {"clone", {"points", "button", "source", "aligned"}},
        // the half-block tools: pixels
        {"halfBlock", {"points", "button"}},
        {"line", {"from", "to", "button"}},
        {"rectangle", {"from", "to", "button"}},
        {"ellipse", {"from", "to", "button"}},
        // Tool Options' Sample merged is these two tools' alone
        {"fill", {"at", "button", "sampleMerged"}},
        {"eyedropper", {"at", "button", "sampleMerged"}},
        // cells again
        {"gradient", {"from", "to", "button"}},
        {"keyboard", {"at", "text"}},
        // the Text tool: its click, what is typed and its Tool Options,
        // which are its alone (#421)
        {"text", {"at", "text", "textOptions"}},
        // the active live text layer's whole text replaced, and a Library
        // text style put on the active layer (#422 D3, D2)
        {"editText", {"text"}},
        {"applyTextStyle", {"style"}},
        {"move", {"by"}},
        // the selection
        {"select", {"rect"}},
        {"selectAll", {}},
        {"deselect", {}},
        {"moveSelection", {"by"}},
        {"copy", {}},
        {"cut", {}},
        {"deleteSelection", {}},
        {"paste", {"at"}},
        {"flipHorizontal", {}},
        {"flipVertical", {}},
        {"rotateClockwise", {}},
        {"rotateCounterClockwise", {}},
        // layers
        {"newCellsLayer", {"rename"}},
        {"newPixelLayer", {"rename"}},
        {"deleteLayer", {}},
        {"raiseLayer", {}},
        {"lowerLayer", {}},
        {"setActiveLayer", {"layer"}},
        {"layerProperties", {"rename", "visible", "locked", "alphaLocked"}},
        // File > Open Reference Image... (#422 D5)
        {"openReferenceImage", {"file"}},
        // the canvas: Format > Canvas Size... (#420)
        {"canvasSize", {"size", "anchor"}},
        // the Colours menu's palette items (#497)
        {"palette", {"preset", "colours"}},
        {"fitToPalette", {"preset", "colours"}},
        {"remapColours", {"map", "scope"}},
        // the window's view (#398)
        {"view", {"zoom", "scrollTo", "ircFit", "ircPad", "ircSwapShades", "guides"}},
        // the Library (#431): a stamp placed from the dock, and Selection >
        // Save to Library...
        {"placeStamp", {"stamp", "at", "library"}},
        {"saveToLibrary", {"stamp", "category", "tags"}},
    };
    return all;
}

const std::set<std::string_view> kOptionKeys = {
    "fg",       "bg",      "size",   "tip",    "strength", "pressure", "light",  "ramp",
    "symmetry", "form",    "hardness", "outline", "keepLint", "glyph", "filled", "pencil",
    "gradient", "texture", "spray",   "pasteMode",
};

const std::set<std::string_view> kStepKeys = {"name", "action", "capture", "screenshot"};

std::string utf8(const QString& s) { return s.toStdString(); }

// Throws for `what` in `where` ("step 3 (outline)", or "start").
[[noreturn]] void fail(const std::string& where, const std::string& why) {
    throw StepsError(where + ": " + why);
}

const char* kindOf(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Null: return "null";
    case QJsonValue::Bool: return "a boolean";
    case QJsonValue::Double: return "a number";
    case QJsonValue::String: return "a string";
    case QJsonValue::Array: return "an array";
    case QJsonValue::Object: return "an object";
    case QJsonValue::Undefined: break;
    }
    return "nothing";
}

int intOf(const std::string& where, const std::string& key, const QJsonValue& v, int lo, int hi) {
    const double d = v.toDouble(std::nan(""));
    if (!v.isDouble() || d != std::floor(d) || d < lo || d > hi)
        fail(where, "\"" + key + "\" must be a whole number " + std::to_string(lo) + " to " + std::to_string(hi) +
                        (v.isDouble() ? "" : std::string(", not ") + kindOf(v)));
    return static_cast<int>(d);
}

double numberOf(const std::string& where, const std::string& key, const QJsonValue& v, double lo, double hi) {
    const double d = v.toDouble(std::nan(""));
    if (!v.isDouble() || !(d >= lo && d <= hi)) {
        char range[64];
        std::snprintf(range, sizeof range, "%g to %g", lo, hi);
        fail(where, "\"" + key + "\" must be a number " + range + (v.isDouble() ? "" : std::string(", not ") + kindOf(v)));
    }
    return d;
}

bool boolOf(const std::string& where, const std::string& key, const QJsonValue& v) {
    if (!v.isBool())
        fail(where, "\"" + key + "\" must be true or false, not " + kindOf(v));
    return v.toBool();
}

std::string stringOf(const std::string& where, const std::string& key, const QJsonValue& v) {
    if (!v.isString())
        fail(where, "\"" + key + "\" must be a string, not " + kindOf(v));
    return utf8(v.toString());
}

// A string as CP437 bytes, NFC-composed as the Save to Library dialog takes
// a name (toCp437): refused when a character has no CP437 glyph.
std::string cp437Text(const std::string& where, const std::string& key, const std::string& text) {
    std::string out;
    for (const char32_t ch : QString::fromStdString(text).normalized(QString::NormalizationForm_C).toUcs4()) {
        const std::optional<std::uint8_t> b = unicodeToCp437(ch);
        if (!b || *b == 0)
            fail(where, "\"" + key + "\" \"" + text + "\" holds a character CP437 has no glyph for");
        out += char(*b);
    }
    return out;
}

// One of `names` (a name per enum value, in order).
template <typename E>
E choiceOf(const std::string& where, const std::string& key, const QJsonValue& v,
           const std::vector<std::pair<std::string_view, E>>& names) {
    const std::string s = stringOf(where, key, v);
    std::string all;
    for (const auto& [n, e] : names) {
        if (s == n)
            return e;
        all += (all.empty() ? "" : ", ") + std::string(n);
    }
    fail(where, "\"" + key + "\" takes " + all + ", not \"" + s + "\"");
}

QJsonObject objectOf(const std::string& where, const std::string& key, const QJsonValue& v,
                     const std::set<std::string_view>& keys) {
    if (!v.isObject())
        fail(where, "\"" + key + "\" must be an object, not " + kindOf(v));
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!keys.count(utf8(it.key())))
            fail(where, "\"" + key + "\" has no \"" + utf8(it.key()) + "\"");
    return o;
}

// [x, y] or, with `pressure`, [x, y, pressure]. Any int: where it lies is
// checked when the step is replayed.
Point pointOf(const std::string& where, const std::string& key, const QJsonValue& v, bool pressure) {
    const QJsonArray a = v.toArray();
    if (!v.isArray() || a.size() < 2 || a.size() > (pressure ? 3 : 2))
        fail(where, "\"" + key + "\" must be [x, y]" + (pressure ? " or [x, y, pressure]" : "") + ", not " +
                        (v.isArray() ? "an array of " + std::to_string(a.size()) : std::string(kindOf(v))));
    Point p;
    p.x = intOf(where, key + " x", a[0], -kMaxBlankSide * 2, kMaxBlankSide * 2);
    p.y = intOf(where, key + " y", a[1], -kMaxBlankSide * 2, kMaxBlankSide * 2);
    if (a.size() == 3)
        p.pressure = numberOf(where, key + " pressure", a[2], 0.0, 1.0);
    return p;
}

std::uint8_t colourOf(const std::string& where, const std::string& key, const QJsonValue& v) {
    return static_cast<std::uint8_t>(intOf(where, key, v, 0, 15));
}

Ramp rampOf(const std::string& where, const QJsonValue& v) {
    if (v.isString()) {
        const std::string name = utf8(v.toString());
        if (const auto r = findRamp(name))
            return *r;
        std::string names;
        for (const Ramp& r : namedRamps())
            names += (names.empty() ? "" : ", ") + r.name;
        fail(where, "\"ramp\" takes a ramp name (" + names + ") or palette indices dark to light, not \"" + name + "\"");
    }
    if (!v.isArray())
        fail(where, "\"ramp\" must be a ramp name or an array of palette indices, not " + std::string(kindOf(v)));
    Ramp r;
    r.name = "custom";
    for (const QJsonValue& c : v.toArray())
        r.colours.push_back(colourOf(where, "ramp", c));
    if (const std::string why = rampProblem(r); !why.empty())
        fail(where, "\"ramp\": " + why);
    return r;
}

Symmetry symmetryOf(const std::string& where, const QJsonValue& v) {
    static const std::vector<std::pair<std::string_view, SymmetryMode>> modes = {
        {"none", SymmetryMode::None},         {"mirrorX", SymmetryMode::MirrorX}, {"mirrorY", SymmetryMode::MirrorY},
        {"mirrorXY", SymmetryMode::MirrorXY}, {"radial", SymmetryMode::Radial},
    };
    Symmetry s;
    if (v.isString()) {
        s.mode = choiceOf(where, "symmetry", v, modes);
        return s;
    }
    const QJsonObject o = objectOf(where, "symmetry", v, {"mode", "folds", "centre"});
    if (!o.contains("mode"))
        fail(where, "\"symmetry\" needs a \"mode\"");
    s.mode = choiceOf(where, "symmetry mode", o["mode"], modes);
    if (o.contains("folds"))
        s.folds = intOf(where, "symmetry folds", o["folds"], kMinFolds, kMaxFolds);
    if (o.contains("centre")) {
        const Point c = pointOf(where, "symmetry centre", o["centre"], false);
        s.centre = std::pair{c.x, c.y};
    }
    if (s.mode != SymmetryMode::Radial && (o.contains("folds") || o.contains("centre")))
        fail(where, "\"symmetry\": only radial symmetry takes folds and a centre");
    return s;
}

char32_t glyphOf(const std::string& where, const QJsonValue& v) {
    const std::u32string g = QString::fromStdString(stringOf(where, "glyph", v)).toStdU32String();
    // C0, DEL and C1 are refused as a text step refuses them (typedInText)
    if (g.size() != 1 || !typedInText(g[0]))
        fail(where, "\"glyph\" must be one printable character");
    return g[0];
}

// The Text tool's Tool Options (TextOptions, #421).
Options::Text textOptionsOf(const std::string& where, const QJsonValue& v) {
    const QJsonObject o = objectOf(where, "textOptions", v,
                                   {"face", "case", "letterSpacing", "lineSpacing", "align", "wrap", "colour",
                                    "bigStyle", "bigColours"});
    Options::Text t;
    if (o.contains("face") && o["face"].isObject()) {
        // a TheDraw font (#422): the file is read by the load pass and the
        // font found in it when the step is applied
        const QJsonObject f = objectOf(where, "textOptions face", o["face"], {"tdf", "font", "index"});
        Options::Text::Tdf tdf;
        if (!f.contains("tdf"))
            fail(where, "\"textOptions face\" needs \"tdf\", the .tdf file's path");
        if (!f.contains("font"))
            fail(where, "\"textOptions face\" needs \"font\", the font's name in the file");
        tdf.path = stringOf(where, "textOptions face tdf", f["tdf"]);
        if (const auto why = pathProblem(tdf.path))
            fail(where, "\"textOptions face tdf\" " + *why);
        tdf.font = stringOf(where, "textOptions face font", f["font"]);
        if (tdf.font.empty())
            fail(where, "\"textOptions face font\" is empty");
        if (f.contains("index"))
            tdf.index = intOf(where, "textOptions face index", f["index"], 0, int(kMaxTdfFonts) - 1);
        t.tdf = std::move(tdf);
    } else if (o.contains("face")) {
        const std::string name = stringOf(where, "textOptions face", o["face"]);
        std::string names;
        for (const Face* f : bundledFaces())
            names += (names.empty() ? "" : ", ") + std::string(f->name());
        if (!bundledFace(name))
            fail(where, "\"textOptions face\" takes a bundled face (" + names +
                            ") or a TheDraw font ({\"tdf\": ..., \"font\": ...}), not \"" + name + "\"");
        t.face = name;
    }
    if (o.contains("case"))
        t.textCase = choiceOf<TextCase>(where, "textOptions case", o["case"],
                                        {{textCaseName(TextCase::AsTyped), TextCase::AsTyped},
                                         {textCaseName(TextCase::Capitals), TextCase::Capitals}});
    if (o.contains("letterSpacing"))
        t.letterSpacing = intOf(where, "textOptions letterSpacing", o["letterSpacing"], 0, TextSource::kMaxSpacing);
    if (o.contains("lineSpacing"))
        t.lineSpacing = intOf(where, "textOptions lineSpacing", o["lineSpacing"], 0, TextSource::kMaxSpacing);
    if (o.contains("align"))
        t.align = choiceOf<TextAlign>(where, "textOptions align", o["align"],
                                      {{textAlignName(TextAlign::Left), TextAlign::Left},
                                       {textAlignName(TextAlign::Centre), TextAlign::Centre},
                                       {textAlignName(TextAlign::Right), TextAlign::Right}});
    if (o.contains("wrap"))
        t.wrapWidth = intOf(where, "textOptions wrap", o["wrap"], 0, TextSource::kMaxWrapWidth);
    if (o.contains("colour"))
        t.colour = colourOf(where, "textOptions colour", o["colour"]);
    if (o.contains("bigStyle")) {
        std::vector<std::pair<std::string_view, BigTextStyle>> styles;
        for (int i = 0; !bigTextStyleName(BigTextStyle(i)).empty(); ++i)
            styles.emplace_back(bigTextStyleName(BigTextStyle(i)), BigTextStyle(i));
        t.bigStyle = choiceOf<BigTextStyle>(where, "textOptions bigStyle", o["bigStyle"], styles);
    }
    if (o.contains("bigColours")) {
        const QJsonValue& c = o["bigColours"];
        if (!c.isArray() || c.toArray().isEmpty() || c.toArray().size() > qsizetype(TextSource::kMaxBigColours))
            fail(where, "\"textOptions bigColours\" must be an array of 1 to " +
                            std::to_string(TextSource::kMaxBigColours) + " mIRC colours 0 to 15" +
                            (c.isArray() ? "" : std::string(", not ") + kindOf(c)));
        std::vector<std::uint8_t> colours;
        for (const QJsonValue& n : c.toArray())
            colours.push_back(colourOf(where, "textOptions bigColours", n));
        t.bigColours = std::move(colours);
    }
    return t;
}

void readOptions(const std::string& where, const QJsonObject& o, Options& opt) {
    if (o.contains("fg"))
        opt.fg = colourOf(where, "fg", o["fg"]);
    if (o.contains("bg"))
        opt.bg = colourOf(where, "bg", o["bg"]);
    if (o.contains("size"))
        opt.size = intOf(where, "size", o["size"], 1, kMaxBrushSize);
    if (o.contains("tip"))
        opt.tip = choiceOf<Tip>(where, "tip", o["tip"], {{"square", Tip::Square}, {"round", Tip::Round}});
    if (o.contains("strength"))
        opt.strength = numberOf(where, "strength", o["strength"], 0.0, 1.0);
    if (o.contains("pressure"))
        opt.pressure = choiceOf<PressureTo>(
            where, "pressure", o["pressure"],
            {{"none", PressureTo::Nothing}, {"size", PressureTo::Size}, {"strength", PressureTo::Strength}});
    if (o.contains("light"))
        opt.light = choiceOf<Compass>(where, "light", o["light"],
                                      {{"N", Compass::N},
                                       {"NE", Compass::NE},
                                       {"E", Compass::E},
                                       {"SE", Compass::SE},
                                       {"S", Compass::S},
                                       {"SW", Compass::SW},
                                       {"W", Compass::W},
                                       {"NW", Compass::NW}});
    if (o.contains("ramp"))
        opt.ramp = rampOf(where, o["ramp"]);
    if (o.contains("symmetry"))
        opt.symmetry = symmetryOf(where, o["symmetry"]);
    if (o.contains("form")) {
        const QJsonObject f = objectOf(where, "form", o["form"], {"shape", "rim", "specular"});
        FormOptions fo;
        if (f.contains("shape"))
            fo.shape = choiceOf<FormShape>(
                where, "form shape", f["shape"],
                {{"dome", FormShape::Dome}, {"cylinder", FormShape::Cylinder}, {"plane", FormShape::Plane}});
        if (f.contains("rim"))
            fo.rim = boolOf(where, "form rim", f["rim"]);
        if (f.contains("specular"))
            fo.specular = boolOf(where, "form specular", f["specular"]);
        opt.form = fo;
    }
    if (o.contains("hardness"))
        opt.hardness = numberOf(where, "hardness", o["hardness"], 0.0, 1.0);
    if (o.contains("outline"))
        opt.outline = choiceOf<OutlineShape>(where, "outline", o["outline"],
                                             {{"ramp", OutlineShape::Ramp}, {"selection", OutlineShape::Selection}});
    if (o.contains("keepLint"))
        opt.keepLint = boolOf(where, "keepLint", o["keepLint"]);
    if (o.contains("glyph"))
        opt.glyph = glyphOf(where, o["glyph"]);
    if (o.contains("filled"))
        opt.filled = boolOf(where, "filled", o["filled"]);
    if (o.contains("pencil")) {
        const int joins = intOf(where, "pencil", o["pencil"], 4, 8);
        if (joins != 4 && joins != 8)
            fail(where, "\"pencil\" takes 4 or 8");
        opt.pencil = joins == 8 ? Connectivity::Eight : Connectivity::Four;
    }
    if (o.contains("gradient")) {
        const QJsonObject g = objectOf(where, "gradient", o["gradient"], {"shape", "banding", "from", "to"});
        if (g.contains("shape"))
            opt.gradientShape = choiceOf<GradientShape>(where, "gradient shape", g["shape"],
                                                        {{"linear", GradientShape::Linear}, {"radial", GradientShape::Radial}});
        if (g.contains("banding"))
            opt.gradientBanding = choiceOf<GradientBanding>(
                where, "gradient banding", g["banding"], {{"cells", GradientBanding::Cells}, {"bayer", GradientBanding::Bayer}});
        if (g.contains("from"))
            opt.gradientFrom = intOf(where, "gradient from", g["from"], 0, kRungsPerStep * 15);
        if (g.contains("to"))
            opt.gradientTo = intOf(where, "gradient to", g["to"], 0, kRungsPerStep * 15);
    }
    if (o.contains("sampleMerged"))
        opt.sampleMerged = boolOf(where, "sampleMerged", o["sampleMerged"]);
    if (o.contains("pasteMode"))
        opt.pasteMode = choiceOf<PasteMode>(
            where, "pasteMode", o["pasteMode"],
            {{"normal", PasteMode::Normal}, {"transparent", PasteMode::Transparent}, {"under", PasteMode::Under}});
    if (o.contains("texture")) {
        const QJsonObject t = objectOf(where, "texture", o["texture"], {"kind", "density", "seed"});
        TextureOptions to;
        if (t.contains("kind"))
            to.kind = choiceOf<TextureKind>(where, "texture kind", t["kind"],
                                            {{"scales", TextureKind::Scales},
                                             {"foliage", TextureKind::Foliage},
                                             {"hoard", TextureKind::Hoard},
                                             {"brick", TextureKind::Brick},
                                             {"noise", TextureKind::Noise}});
        if (t.contains("density"))
            to.density = numberOf(where, "texture density", t["density"], kMinTextureDensity, kMaxTextureDensity);
        if (t.contains("seed"))
            to.seed = intOf(where, "texture seed", t["seed"], 0, kMaxTextureSeed);
        opt.texture = to;
    }
    if (o.contains("spray")) {
        const QJsonObject s = objectOf(where, "spray", o["spray"], {"kind", "density", "seed"});
        SprayOptions so;
        if (s.contains("kind"))
            so.kind = choiceOf<SprayKind>(where, "spray kind", s["kind"],
                                          {{"stars", SprayKind::Stars}, {"haze", SprayKind::Haze}, {"smoke", SprayKind::Smoke}});
        if (s.contains("density"))
            so.density = intOf(where, "spray density", s["density"], kMinSprayDensity, kMaxSprayDensity);
        if (s.contains("seed"))
            so.seed = intOf(where, "spray seed", s["seed"], 0, kMaxSpraySeed);
        opt.spray = so;
    }
    if (o.contains("aligned"))
        opt.aligned = boolOf(where, "aligned", o["aligned"]);
    if (o.contains("textOptions"))
        opt.text = textOptionsOf(where, o["textOptions"]);
}

// 1-64 of a-z 0-9 - _, starting with a letter or digit: a file name on any
// system.
bool goodName(const std::string& s) {
    if (s.empty() || s.size() > 64)
        return false;
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(s[0]))
        return false;
    return std::all_of(s.begin(), s.end(), [&](char c) { return alnum(c) || c == '-' || c == '_'; });
}

Start readStart(const QJsonValue& v) {
    const std::string where = "start";
    const QJsonObject o = objectOf(where, "start", v, {"file", "sample", "variant", "blank"});
    const int kinds = int(o.contains("file")) + int(o.contains("sample")) + int(o.contains("blank"));
    if (kinds != 1)
        fail(where, "needs exactly one of \"file\", \"sample\" or \"blank\"");
    Start s;
    if (o.contains("variant") && !o.contains("sample"))
        fail(where, "only a sample takes a \"variant\"");
    if (o.contains("file")) {
        s.kind = Start::Kind::File;
        s.file = stringOf(where, "file", o["file"]);
        if (const auto why = pathProblem(s.file))
            fail(where, "\"file\" " + *why);
    } else if (o.contains("sample")) {
        s.kind = Start::Kind::Sample;
        s.sample = stringOf(where, "sample", o["sample"]);
        if (s.sample.empty() || s.sample.find_first_of("/\\") != std::string::npos || s.sample == "." ||
            s.sample == "..")
            fail(where, "\"sample\" must name a sample, not a path");
        if (o.contains("variant"))
            s.variant = stringOf(where, "variant", o["variant"]);
    } else {
        s.kind = Start::Kind::Blank;
        const QJsonArray a = o["blank"].toArray();
        if (!o["blank"].isArray() || a.size() != 2)
            fail(where, "\"blank\" must be [width, height]");
        s.width = intOf(where, "blank width", a[0], 1, kMaxBlankSide);
        s.height = intOf(where, "blank height", a[1], 1, kMaxBlankSide);
    }
    return s;
}

// A step's "screenshot" (#295). Whether the widget it names is there, and
// the crop on it, is the window's to check when it takes the picture.
Screenshot readScreenshot(const std::string& where, const QJsonValue& v) {
    const QJsonObject o = objectOf(where, "screenshot", v, {"of", "crop", "when", "scale", "pointer"});
    Screenshot shot;
    if (o.contains("of")) {
        shot.of = stringOf(where, "screenshot of", o["of"]);
        if (shot.of.empty())
            fail(where, "\"screenshot of\" is empty");
    }
    if (o.contains("when"))
        shot.when = choiceOf<Screenshot::When>(
            where, "screenshot when", o["when"],
            {{"before", Screenshot::When::Before}, {"after", Screenshot::When::After}, {"both", Screenshot::When::Both}});
    if (o.contains("scale"))
        shot.scale = intOf(where, "screenshot scale", o["scale"], 1, 2);
    if (o.contains("pointer"))
        shot.pointer = boolOf(where, "screenshot pointer", o["pointer"]);
    if (o.contains("crop")) {
        const QJsonObject c = objectOf(where, "screenshot crop", o["crop"], {"cells", "pixels"});
        if (c.size() != 1)
            fail(where, "\"screenshot crop\" needs one of \"cells\" or \"pixels\"");
        const bool cells = c.contains("cells");
        const std::string key = cells ? "cells" : "pixels";
        const QJsonValue r = c[QString::fromStdString(key)];
        if (!r.isArray() || r.toArray().size() != 4)
            fail(where, "\"screenshot crop " + key + "\" must be [x, y, width, height]");
        const QJsonArray a = r.toArray();
        const std::string k = "screenshot crop " + key;
        shot.crop = cells ? Screenshot::Crop::Cells : Screenshot::Crop::Pixels;
        shot.x = intOf(where, k + " x", a[0], 0, cells ? kMaxBlankSide - 1 : kMaxWindowSide - 1);
        shot.y = intOf(where, k + " y", a[1], 0, cells ? kMaxBlankSide - 1 : kMaxWindowSide - 1);
        shot.width = intOf(where, k + " width", a[2], 1, cells ? kMaxBlankSide : kMaxWindowSide);
        shot.height = intOf(where, k + " height", a[3], 1, cells ? kMaxBlankSide : kMaxWindowSide);
    }
    return shot;
}

bool takesNoOptions(std::string_view action) {
    return action == "view" || action == "canvasSize" || action == "palette" || action == "fitToPalette" ||
           action == "remapColours";
}

Step readStep(const QJsonValue& v, int number) {
    Step st;
    st.number = number;
    std::string where = "step " + std::to_string(number);
    if (!v.isObject())
        fail(where, std::string("must be an object, not ") + kindOf(v));
    const QJsonObject o = v.toObject();
    if (!o.contains("name"))
        fail(where, "has no \"name\"");
    st.name = stringOf(where, "name", o["name"]);
    if (!goodName(st.name))
        fail(where, "\"name\" must be 1-64 of a-z 0-9 - _, starting with a letter or digit, not \"" + st.name + "\"");
    where = stepLabel(st);
    if (!o.contains("action"))
        fail(where, "has no \"action\"");
    st.action = stringOf(where, "action", o["action"]);
    const auto& all = actionKeys();
    const auto it = std::find_if(all.begin(), all.end(), [&](const ActionKeys& a) { return a.action == st.action; });
    if (it == all.end()) {
        std::string names;
        for (const ActionKeys& a : all)
            names += (names.empty() ? "" : ", ") + std::string(a.action);
        fail(where, "unknown action \"" + st.action + "\"; the actions are " + names);
    }
    for (auto k = o.begin(); k != o.end(); ++k) {
        const std::string key = utf8(k.key());
        // a view step is the window's, not the pen's, and a canvasSize step's
        // "size" is the canvas's: neither takes Tool Options
        if (!kStepKeys.count(key) && !(kOptionKeys.count(key) && !takesNoOptions(st.action)) &&
            std::find(it->keys.begin(), it->keys.end(), key) == it->keys.end())
            fail(where, st.action + " takes no \"" + key + "\"");
    }
    if (o.contains("capture"))
        st.capture = choiceOf<Capture>(where, "capture", o["capture"],
                                       {{"after", Capture::After}, {"before-after", Capture::BeforeAfter}});
    if (!takesNoOptions(st.action))
        readOptions(where, o, st.options);
    if (st.options.text && st.options.text->tdf)
        st.files.push_back({"textOptions face tdf", st.options.text->tdf->path, std::size_t(kMaxTdfBytes)});
    if (o.contains("screenshot"))
        st.screenshot = readScreenshot(where, o["screenshot"]);
    if (o.contains("button"))
        st.button = choiceOf<Button>(where, "button", o["button"], {{"left", Button::Left}, {"right", Button::Right}});

    if (o.contains("points")) {
        if (!o["points"].isArray() || o["points"].toArray().isEmpty())
            fail(where, "\"points\" must be an array of [x, y] points, at least one");
        const QJsonArray a = o["points"].toArray();
        for (qsizetype i = 0; i < a.size(); ++i)
            st.points.push_back(pointOf(where, "points[" + std::to_string(i) + "]", a[i], true));
    }
    // two corners, or one click: both or neither, so a tool step with
    // neither only sets options
    if (o.contains("from") != o.contains("to"))
        fail(where, st.action + " needs both \"from\" and \"to\", or neither");
    if (o.contains("from")) {
        st.points.push_back(pointOf(where, "from", o["from"], false));
        st.points.push_back(pointOf(where, "to", o["to"], false));
    }
    if (o.contains("at"))
        st.points.push_back(pointOf(where, "at", o["at"], false));
    else if (st.action == "paste")
        fail(where, "paste needs \"at\": the cell the block's top-left corner lands on");
    else if (st.action == "placeStamp")
        fail(where, "placeStamp needs \"at\": the cell the stamp's top-left corner lands on");
    if (st.action == "placeStamp" || st.action == "saveToLibrary") {
        if (!o.contains("stamp"))
            fail(where, st.action + " needs \"stamp\", the stamp's name");
        // saveToLibrary's name, category and tags are trimmed as the Save
        // to Library dialog's are (library::stampText); placeStamp's name
        // is a stamp's title as it is
        std::string name = stringOf(where, "stamp", o["stamp"]);
        if (st.action == "saveToLibrary")
            name = library::stampText(name);
        st.stamp = cp437Text(where, "stamp", name);
        if (st.stamp.empty())
            fail(where, "\"stamp\" must not be empty");
    }
    if (o.contains("library"))
        st.library = choiceOf<Step::Library>(where, "library", o["library"],
                                             {{"bundled", Step::Library::Bundled}, {"user", Step::Library::User}});
    if (st.action == "saveToLibrary") {
        if (!o.contains("category"))
            fail(where, "saveToLibrary needs \"category\", the category the Library dock lists the stamp under");
        st.category = library::stampText(stringOf(where, "category", o["category"]));
        if (o.contains("tags")) {
            if (!o["tags"].isArray())
                fail(where, "\"tags\" must be an array of strings");
            // an empty tag is refused, as the library refuses it; the rest
            // are kept once each, as the dialog keeps them (library::stampTags)
            const QJsonArray a = o["tags"].toArray();
            std::vector<std::string> given;
            for (qsizetype i = 0; i < a.size(); ++i) {
                given.push_back(stringOf(where, "tags[" + std::to_string(i) + "]", a[i]));
                if (library::stampText(given.back()).empty())
                    fail(where, "\"tags[" + std::to_string(i) + "]\" must not be empty");
            }
            st.tags = library::stampTags(given);
        }
    }
    if (st.action == "text" && o.contains("at") != o.contains("text"))
        fail(where, o.contains("at") ? "text needs \"text\" to type at its \"at\" cell: a click with nothing typed "
                                       "makes no text layer"
                                     : "\"text\" needs an \"at\" cell to start from");
    if (st.action == "editText" && !o.contains("text"))
        fail(where, "editText needs \"text\", the active text layer's whole new text");
    if ((st.action == "text" || st.action == "editText") && o.contains("text")) {
        // as typed: the Text tool takes the characters as they come, Enter
        // as a new line, and types no other control character, C0, DEL or
        // C1 (typedInText)
        st.text = QString::fromStdString(stringOf(where, "text", o["text"])).toStdU32String();
        if (st.text.empty() || st.text.size() > TextSource::kMaxLength)
            fail(where, "\"text\" must be 1 to " + std::to_string(TextSource::kMaxLength) + " characters");
        for (const char32_t ch : st.text)
            if (ch != U'\n' && !typedInText(ch)) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "U+%04X", unsigned(ch));
                fail(where, std::string("\"text\" holds a control character the Text tool doesn't type (") + hex +
                                "); only \"\\n\", Enter, starts a new line");
            }
    } else if (o.contains("text")) {
        if (!o.contains("at"))
            fail(where, "\"text\" needs an \"at\" cell to start from");
        // NFC first, as keyboard mode types (CanvasView::typeText): "e" and
        // U+0301 are the glyph é
        st.text = QString::fromStdString(stringOf(where, "text", o["text"]))
                      .normalized(QString::NormalizationForm_C)
                      .toStdU32String();
    }
    if (st.action == "applyTextStyle") {
        if (!o.contains("style"))
            fail(where, "applyTextStyle needs \"style\": a bundled Library preset's name, or {\"file\": ...}");
        const QJsonValue& v = o["style"];
        if (v.isObject()) {
            const QJsonObject f = objectOf(where, "style", v, {"file"});
            if (!f.contains("file"))
                fail(where, "\"style\" needs \"file\", a .style.json's path");
            const std::string path = stringOf(where, "style file", f["file"]);
            if (const auto why = pathProblem(path))
                fail(where, "\"style file\" " + *why);
            st.files.push_back({"style file", path, library::kMaxSidecarBytes});
        } else if (v.isString()) {
            const std::string name = utf8(v.toString());
            if (!goodName(name))
                fail(where, "\"style\" must be a bundled Library preset's name (1-64 of a-z 0-9 - _, the file name "
                            "before .style.json) or {\"file\": ...}, not \"" + name + "\"");
            st.stylePreset = name;
        } else {
            fail(where, std::string("\"style\" must be a bundled Library preset's name or {\"file\": ...}, not ") +
                            kindOf(v));
        }
    }
    if (st.action == "openReferenceImage") {
        if (!o.contains("file"))
            fail(where, "openReferenceImage needs \"file\", a PNG or JPEG picture's path");
        const std::string path = stringOf(where, "file", o["file"]);
        if (const auto why = pathProblem(path))
            fail(where, "\"file\" " + *why);
        st.files.push_back({"file", path, project::kMaxEntryBytes});
    }
    if (o.contains("source"))
        st.source = pointOf(where, "source", o["source"], false);
    if (o.contains("by")) {
        const Point p = pointOf(where, "by", o["by"], false);
        st.dx = p.x;
        st.dy = p.y;
    } else if (st.action == "move" || st.action == "moveSelection") {
        fail(where, st.action + " needs \"by\": [dx, dy]");
    }
    if (st.action == "select") {
        const QJsonArray a = o["rect"].toArray();
        if (!o["rect"].isArray() || a.size() != 4)
            fail(where, "select needs \"rect\": [x0, y0, x1, y1], corners included");
        int c[4];
        for (int i = 0; i < 4; ++i)
            c[i] = intOf(where, "rect", a[i], -kMaxBlankSide, 2 * kMaxBlankSide);
        st.rect = CellRect::fromCorners(c[0], c[1], c[2], c[3]);
    }
    if (st.action == "setActiveLayer") {
        if (!o.contains("layer"))
            fail(where, "setActiveLayer needs \"layer\", a layer's name");
        st.layer = stringOf(where, "layer", o["layer"]);
    }
    if (o.contains("rename")) {
        st.rename = stringOf(where, "rename", o["rename"]);
        if (st.rename->empty())
            fail(where, "\"rename\" is empty");
    }
    if (o.contains("visible"))
        st.visible = boolOf(where, "visible", o["visible"]);
    if (o.contains("locked"))
        st.locked = boolOf(where, "locked", o["locked"]);
    if (o.contains("alphaLocked"))
        st.alphaLocked = boolOf(where, "alphaLocked", o["alphaLocked"]);
    if (st.action == "layerProperties" && !st.rename && !st.visible && !st.locked && !st.alphaLocked)
        fail(where, "layerProperties needs at least one of \"rename\", \"visible\", \"locked\" or \"alphaLocked\"");
    if (st.action == "canvasSize") {
        if (!o.contains("size"))
            fail(where, "canvasSize needs \"size\": [width, height], the new canvas in cells");
        const QJsonValue& z = o["size"];
        if (!z.isArray() || z.toArray().size() != 2)
            fail(where, "\"size\" must be [width, height], the new canvas in cells");
        st.canvasWidth = intOf(where, "size width", z.toArray()[0], 1, kMaxColumns);
        st.canvasHeight = intOf(where, "size height", z.toArray()[1], 1, kMaxRows);
        if (o.contains("anchor")) {
            std::vector<std::pair<std::string_view, Anchor>> names;
            for (int i = 0; i < 9; ++i)
                names.emplace_back(anchorName(Anchor(i)), Anchor(i));
            st.anchor = choiceOf<Anchor>(where, "anchor", o["anchor"], names);
        }
    }
    if (st.action == "palette" || st.action == "fitToPalette") {
        if (o.contains("preset") == o.contains("colours"))
            fail(where, st.action + " needs one of \"preset\", a preset's name, or \"colours\", 16 \"#rrggbb\"");
        if (o.contains("preset")) {
            const std::string name = stringOf(where, "preset", o["preset"]);
            const PalettePreset* p = palettePreset(name);
            if (!p) {
                std::string names;
                for (const PalettePreset& q : palettePresets())
                    names += (names.empty() ? "" : ", ") + std::string(q.name);
                fail(where, "\"preset\" takes " + names + ", not \"" + name + "\"");
            }
            st.palette = p->colours;
        } else {
            const QJsonValue& c = o["colours"];
            if (!c.isArray() || c.toArray().size() != 16)
                fail(where, "\"colours\" must be a list of 16 \"#rrggbb\"");
            for (qsizetype i = 0; i < 16; ++i) {
                const std::string v = stringOf(where, "colours", c.toArray()[i]);
                const auto hex = [](char ch) {
                    return ch >= '0' && ch <= '9'   ? ch - '0'
                           : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                           : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                    : -1;
                };
                bool ok = v.size() == 7 && v[0] == '#';
                for (std::size_t j = 1; ok && j < 7; ++j)
                    ok = hex(v[j]) >= 0;
                if (!ok)
                    fail(where, "\"colours\" " + std::to_string(i) + " must be \"#rrggbb\", not \"" + v + "\"");
                st.palette[std::size_t(i)] = {std::uint8_t(hex(v[1]) << 4 | hex(v[2])),
                                              std::uint8_t(hex(v[3]) << 4 | hex(v[4])),
                                              std::uint8_t(hex(v[5]) << 4 | hex(v[6]))};
            }
        }
    }
    if (st.action == "remapColours") {
        if (!o.contains("map"))
            fail(where, "remapColours needs \"map\": FROM=TO pairs (\"3=11,11=3\") or 16 hex digits");
        std::string error;
        const std::optional<ColourMap> m = parseColourMap(stringOf(where, "map", o["map"]), &error);
        if (!m)
            fail(where, "\"map\": " + error);
        st.colourMap = *m;
        if (o.contains("scope"))
            st.remapScope = choiceOf<Step::RemapScope>(where, "scope", o["scope"],
                                                       {{"selection", Step::RemapScope::Selection},
                                                        {"layer", Step::RemapScope::Layer},
                                                        {"document", Step::RemapScope::Document}});
    }
    if (st.action == "view") {
        View& v = st.view;
        if (o.contains("zoom"))
            v.zoom = intOf(where, "zoom", o["zoom"], kMinZoom, kMaxZoom);
        if (o.contains("scrollTo")) {
            const QJsonValue& c = o["scrollTo"];
            if (!c.isArray() || c.toArray().size() != 2)
                fail(where, "\"scrollTo\" must be [x, y], a cell");
            v.scrollTo = Point{intOf(where, "scrollTo x", c.toArray()[0], 0, kMaxBlankSide - 1),
                               intOf(where, "scrollTo y", c.toArray()[1], 0, kMaxBlankSide - 1)};
        }
        if (o.contains("ircFit"))
            v.ircFit = boolOf(where, "ircFit", o["ircFit"]);
        if (o.contains("ircPad"))
            v.ircPad = boolOf(where, "ircPad", o["ircPad"]);
        if (o.contains("ircSwapShades"))
            v.ircSwapShades = boolOf(where, "ircSwapShades", o["ircSwapShades"]);
        if (o.contains("guides"))
            v.guides = choiceOf<std::string>(
                where, "guides", o["guides"],
                {{"none", "none"}, {"80x25", "80x25"}, {"80x40", "80x40"}, {"80x50", "80x50"}});
        if (v == View{})
            fail(where, "view needs at least one of \"zoom\", \"scrollTo\", \"ircFit\", \"ircPad\", "
                        "\"ircSwapShades\" or \"guides\"");
    }
    if (st.screenshot && st.screenshot->pointer && !pointerAt(st, false))
        fail(where, "\"screenshot pointer\" needs a step the pointer makes, with \"points\", \"from\" and \"to\", "
                    "\"at\", select's \"rect\" or a clone step's \"source\"; this " + st.action + " step has none");
    return st;
}

} // namespace

const std::vector<std::string_view>& actions() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> v;
        for (const ActionKeys& a : actionKeys())
            v.push_back(a.action);
        return v;
    }();
    return names;
}

std::optional<std::string> applyTextOptions(TextSource& source, const Options::Text& t, const FaceRef* tdf) {
    if (t.tdf && (!tdf || tdf->kind != FaceRef::Kind::Tdf))
        return "\"textOptions face\" names the TheDraw font \"" + t.tdf->font + "\" in " + t.tdf->path +
               ", which hasn't been read";
    TextSource s = source;
    if (t.bigStyle)
        s.bigStyle = *t.bigStyle;
    const bool big = s.bigStyle != BigTextStyle::None;
    if (big && t.face && *t.face != FaceRef{}.name)
        return "\"textOptions face\" \"" + *t.face + "\" can't be picked under a big style, which draws in the " +
               FaceRef{}.name + " face (Tool Options' face picker is off)";
    if (big && t.tdf)
        return "\"textOptions face\" \"" + t.tdf->font + "\" (" + t.tdf->path +
               ") can't be picked under a big style, which draws in the " + FaceRef{}.name +
               " face (Tool Options' face picker is off)";
    if (big && t.colour)
        return std::string("\"textOptions colour\" can't be set under a big style, which \"bigColours\" colours "
                           "(Tool Options' colour is off)");
    if (!big && t.bigColours)
        return std::string("\"textOptions bigColours\" needs a big style (Tool Options' colours are off without one)");
    if (t.face)
        s.face = FaceRef{FaceRef::Kind::Pixel, *t.face, {}, 0};
    else if (t.tdf)
        s.face = *tdf;
    // picking a style puts its own colours in the field (TextOptions)
    if (s.bigStyle != source.bigStyle)
        s.bigColours = bigTextDefaultColours(s.bigStyle);
    if (t.bigColours)
        s.bigColours = *t.bigColours;
    if (big) {
        s.face = FaceRef{};
        if (s.bigColours.empty())
            s.bigColours = bigTextDefaultColours(s.bigStyle);
    } else
        s.bigColours.clear();
    if (t.textCase)
        s.textCase = *t.textCase;
    if (t.letterSpacing)
        s.letterSpacing = *t.letterSpacing;
    if (t.lineSpacing)
        s.lineSpacing = *t.lineSpacing;
    if (t.align)
        s.align = *t.align;
    if (t.wrapWidth)
        s.wrapWidth = *t.wrapWidth;
    if (t.colour)
        s.colour = *t.colour;
    source = std::move(s);
    return std::nullopt;
}

bool pixelAction(std::string_view action) {
    return action == "halfBlock" || action == "line" || action == "rectangle" || action == "ellipse" ||
           action == "fill" || action == "eyedropper";
}

std::optional<Point> pointerAt(const Step& step, bool before) {
    if (step.rect)
        return before ? Point{step.rect->x, step.rect->y}
                      : Point{step.rect->x + step.rect->width - 1, step.rect->y + step.rect->height - 1};
    // a clone step's Ctrl+click on its source comes before its stroke
    if (step.source && (before || step.points.empty()))
        return step.source;
    if (step.points.empty())
        return std::nullopt;
    return before ? step.points.front() : step.points.back();
}

std::string stepLabel(const Step& step) { return "step " + std::to_string(step.number) + " (" + step.name + ")"; }

Script parse(std::string_view json) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json.data(), qsizetype(json.size())), &err);
    if (err.error != QJsonParseError::NoError)
        throw StepsError("not JSON: " + utf8(err.errorString()) + " at offset " + std::to_string(err.offset));
    if (!doc.isObject())
        throw StepsError("a steps file is a JSON object");
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (const std::string k = utf8(it.key());
            k != "version" && k != "start" && k != "ice" && k != "window" && k != "steps")
            throw StepsError("unknown key \"" + k + "\"; a steps file has version, start, ice, window and steps");
    if (!o.contains("version"))
        throw StepsError("no \"version\"");
    if (!o["version"].isDouble() || o["version"].toDouble() != kVersion)
        throw StepsError("\"version\" must be " + std::to_string(kVersion));
    Script s;
    if (!o.contains("start"))
        throw StepsError("no \"start\"");
    s.start = readStart(o["start"]);
    if (o.contains("ice")) {
        if (s.start.kind != Start::Kind::Blank)
            throw StepsError("\"ice\" is for a blank start; a file or sample has its own");
        s.ice = boolOf("top level", "ice", o["ice"]);
    }
    if (o.contains("window")) {
        const QJsonArray a = o["window"].toArray();
        if (!o["window"].isArray() || a.size() != 2)
            throw StepsError("top level: \"window\" must be [width, height]");
        s.windowWidth = intOf("top level", "window width", a[0], kMinWindowSide, kMaxWindowSide);
        s.windowHeight = intOf("top level", "window height", a[1], kMinWindowSide, kMaxWindowSide);
    }
    if (!o["steps"].isArray() || o["steps"].toArray().isEmpty())
        throw StepsError("\"steps\" must be an array of at least one step");
    const QJsonArray steps = o["steps"].toArray();
    // whether a clone step has set a source yet: no step forgets it (only
    // opening a file or New does, CloneTool::forgetSource)
    bool cloneSource = false;
    for (qsizetype i = 0; i < steps.size(); ++i) {
        s.steps.push_back(readStep(steps[i], int(i) + 1));
        const Step& st = s.steps.back();
        if (st.action != "clone")
            continue;
        cloneSource = cloneSource || st.source;
        if (!st.points.empty() && !cloneSource)
            throw StepsError(stepLabel(st) + ": clone needs a source to copy from: \"source\": [x, y], the cell "
                                             "Ctrl+click sets, on this step or an earlier clone step");
    }
    return s;
}

std::optional<std::string> pathProblem(std::string_view path) {
    if (path.empty())
        return "is empty";
    if (path.find('\0') != std::string_view::npos)
        return "holds a NUL character";
    const std::string quoted = "\"" + std::string(path) + "\"";
    if (path.find('\\') != std::string_view::npos)
        return quoted + " has a \\ in it; write / between directories, relative to the steps file";
    const bool drive = path.size() >= 2 && path[1] == ':' &&
                       ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    if (path.front() == '/' || drive)
        return quoted + " is absolute; a steps file names its files relative to its own directory";
    return std::nullopt;
}

} // namespace bw::steps
