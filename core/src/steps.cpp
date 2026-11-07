#include "blockwright/steps.h"

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
        // the half-block tools: pixels
        {"halfBlock", {"points", "button"}},
        {"line", {"from", "to", "button"}},
        {"rectangle", {"from", "to", "button"}},
        {"ellipse", {"from", "to", "button"}},
        {"fill", {"at", "button"}},
        {"eyedropper", {"at", "button"}},
        // cells again
        {"gradient", {"from", "to", "button"}},
        {"keyboard", {"at", "text"}},
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
        // the window's view (#398)
        {"view", {"zoom", "scrollTo", "ircFit", "ircPad", "ircSwapShades"}},
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
    if (g.size() != 1 || g[0] < 0x20 || g[0] == 0x7f)
        fail(where, "\"glyph\" must be one printable character");
    return g[0];
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
}

// 1-64 of a-z 0-9 - _, starting with a letter or digit: a file name on any
// system, as Publish's piece names are.
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
        if (s.file.empty())
            fail(where, "\"file\" is empty");
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
    const QJsonObject o = objectOf(where, "screenshot", v, {"of", "crop", "when", "scale"});
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
        // a view step is the window's, not the pen's: it takes no Tool Options
        if (!kStepKeys.count(key) && !(kOptionKeys.count(key) && st.action != "view") &&
            std::find(it->keys.begin(), it->keys.end(), key) == it->keys.end())
            fail(where, st.action + " takes no \"" + key + "\"");
    }
    if (o.contains("capture"))
        st.capture = choiceOf<Capture>(where, "capture", o["capture"],
                                       {{"after", Capture::After}, {"before-after", Capture::BeforeAfter}});
    readOptions(where, o, st.options);
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
    if (o.contains("text")) {
        if (!o.contains("at"))
            fail(where, "\"text\" needs an \"at\" cell to start from");
        // NFC first, as keyboard mode types (CanvasView::typeText): "e" and
        // U+0301 are the glyph é
        st.text = QString::fromStdString(stringOf(where, "text", o["text"]))
                      .normalized(QString::NormalizationForm_C)
                      .toStdU32String();
    }
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
        if (v == View{})
            fail(where, "view needs at least one of \"zoom\", \"scrollTo\", \"ircFit\", \"ircPad\" or \"ircSwapShades\"");
    }
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

bool pixelAction(std::string_view action) {
    return action == "halfBlock" || action == "line" || action == "rectangle" || action == "ellipse" ||
           action == "fill" || action == "eyedropper";
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
    for (qsizetype i = 0; i < steps.size(); ++i)
        s.steps.push_back(readStep(steps[i], int(i) + 1));
    return s;
}

} // namespace bw::steps
