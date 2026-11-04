#include "tool_illustration.h"

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "frame_tool.h"
#include "select_tool.h"
#include "settings.h"
#include "text_tool.h"
#include "tool.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/texture.h"
#include "blockwright/canvas_renderer.h"
#include "blockwright/halfblock.h"
#include "blockwright/palette.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace bw::help {

namespace {

constexpr int kW = kIllustrationSize.width(), kH = kIllustrationSize.height();

// A press, the moves and a release, through half-block pixels: a column and
// a half row (two to a cell row). One point is a click.
struct Stroke {
    std::vector<QPoint> at;
    Qt::MouseButton button = Qt::LeftButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

// A tool's picture: the tool, the document it starts on (a cells canvas of
// kIllustrationSize, then whatever `setup` adds: layers, the pen, the brush,
// the ramp), the strokes, and what is typed after them.
struct Script {
    QString id;
    std::function<Tool*()> make;
    std::function<void(Canvas&)> start;
    std::function<void(Document&, CanvasView&)> setup;
    std::vector<Stroke> strokes = {};
    QString typed = {};
};

// The top half of cell (x, y), as a stroke's point.
QPoint cell(int x, int y) { return {x, 2 * y}; }

// A straight drag from one cell to another, a point a cell.
Stroke drag(QPoint from, QPoint to, Qt::MouseButton button = Qt::LeftButton) {
    Stroke s{{}, button};
    const int n = std::max(std::abs(to.x() - from.x()), std::abs(to.y() - from.y()));
    for (int i = 0; i <= n; ++i)
        s.at.push_back(n ? from + (to - from) * i / n : from);
    return s;
}

Stroke cellDrag(int x0, int y0, int x1, int y1, Qt::MouseButton button = Qt::LeftButton) {
    return drag(cell(x0, y0), cell(x1, y1), button);
}

Ramp ramp(const char* name) { return *findRamp(name); }

void setPen(CanvasView& v, std::uint8_t fg, std::uint8_t bg = Black) { v.setPen({Color::indexed(fg), Color::indexed(bg)}); }

void setBrush(CanvasView& v, int size, double strength, Tip tip = Tip::Square) {
    BrushSettings b = v.brushSettings();
    b.size = size;
    b.strength = strength;
    b.tip = tip;
    v.setBrushSettings(b);
}

void fill(Canvas& c, const Cell& with) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            c.at(x, y) = with;
}

// The cells inside the ellipse in the middle of cell columns x0-x1 and rows
// y0-y1, set to `with`.
void disc(Canvas& c, int x0, int y0, int x1, int y1, const Cell& with) {
    const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
    const double rx = (x1 - x0 + 1) / 2.0, ry = (y1 - y0 + 1) / 2.0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (std::pow((x - cx) / rx, 2) + std::pow((y - cy) / ry, 2) <= 1.0)
                c.at(x, y) = with;
}

// A small piece of art, five cells by three, with its top left at (x, y).
std::vector<std::pair<QPoint, Cell>> sprite(int x, int y) {
    static const char32_t* rows[] = {U" ▄█▄ ", U"█▓▒▓█", U" ▀█▀ "};
    std::vector<std::pair<QPoint, Cell>> out;
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < 5; ++i)
            if (rows[r][i] != U' ')
                out.push_back({{x + i, y + r}, Cell{rows[r][i], Color::indexed(Yellow), Color::indexed(Red)}});
    return out;
}

void putSprite(Canvas& c, int x, int y) {
    for (const auto& [at, cl] : sprite(x, y))
        c.at(at.x(), at.y()) = cl;
}

const std::vector<Script>& scripts() {
    static const std::vector<Script> all = [] {
        std::vector<Script> s;
        const Ramp fire = ramp("fire"), steel = ramp("steel");

        s.push_back({QStringLiteral("keyboard"), [] { return new Tool(QStringLiteral("keyboard"), QString()); }, {},
                     [](Document&, CanvasView& v) { setPen(v, LightCyan); }, {{{cell(2, 2)}}},
                     QStringLiteral("hello, ansi")});

        // The art is lifted off a ground and set down further on, leaving
        // a hole.
        s.push_back({QStringLiteral("select"), [] { return new SelectTool; },
                     [steel](Canvas& c) {
                         fill(c, rungCell(steel, 2));
                         putSprite(c, 1, 1);
                     },
                     {},
                     {{{cell(1, 1), cell(3, 2), {5, 7}}}, cellDrag(3, 2, 10, 3)}});

        // The art's layer moves over the ground, which stays whole.
        s.push_back({QStringLiteral("move"), [] { return new MoveTool; },
                     [steel](Canvas& c) { fill(c, rungCell(steel, 2)); },
                     [](Document& d, CanvasView&) {
                         const LayerId art = d.addLayer(LayerKind::Cells);
                         d.setActiveLayer(art);
                         CellEdit e;
                         for (const auto& [at, cl] : sprite(1, 1))
                             e.changes.push_back({at.x(), at.y(), d.layerCanvas().at(at.x(), at.y()), cl});
                         d.edit(e);
                     },
                     {cellDrag(2, 2, 10, 3)}});

        Stroke wave;
        for (int x = 1; x <= 14; ++x)
            wave.at.push_back({x, 6 + int(std::lround(4 * std::sin((x - 1) * 0.55)))});
        s.push_back({QStringLiteral("halfBlock"), [] { return new HalfBlockTool; }, {},
                     [](Document&, CanvasView& v) { setPen(v, LightGreen); }, {wave}});

        // Each pass builds a cell up one step: four strokes, each shorter.
        s.push_back({QStringLiteral("shade"), [] { return new ShadeTool; }, {},
                     [](Document&, CanvasView& v) {
                         setPen(v, LightCyan);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(0, 3, 15, 3), cellDrag(4, 3, 15, 3), cellDrag(8, 3, 15, 3), cellDrag(12, 3, 15, 3)}});

        // A flat ground: the right lightened twice over, the left darkened.
        s.push_back({QStringLiteral("rampShade"), [] { return new RampShadeTool; },
                     [fire](Canvas& c) { fill(c, rungCell(fire, 6)); },
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 6, 0.25);
                     },
                     {cellDrag(10, 3, 15, 3), cellDrag(13, 3, 15, 3), cellDrag(0, 3, 5, 3, Qt::RightButton),
                      cellDrag(0, 3, 2, 3, Qt::RightButton)}});

        // The selection, shaded as a dome lit from the top left.
        s.push_back({QStringLiteral("form"), [] { return new FormTool; }, {},
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 6, 1.0);
                         v.setSelection(Selection(CellRect{2, 0, 12, 6}));
                     },
                     {cellDrag(2, 3, 13, 3)}});

        s.push_back({QStringLiteral("soft"), [] { return new SoftTool; },
                     [fire](Canvas& c) { fill(c, rungCell(fire, 4)); },
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 6, 1.0, Tip::Round);
                     },
                     {cellDrag(3, 3, 12, 3)}});

        // Specks on a flat ground; the brush clears the right half's.
        s.push_back({QStringLiteral("despeckle"), [] { return new DespeckleTool; },
                     [steel](Canvas& c) {
                         fill(c, rungCell(steel, 8));
                         for (const auto& [x, y, rung] : {std::tuple{1, 1, 5}, {3, 4, 10}, {5, 2, 5},
                                                          {10, 1, 10}, {12, 4, 5}, {13, 2, 10}})
                             c.at(x, y) = rungCell(steel, rung);
                     },
                     [steel](Document&, CanvasView& v) {
                         v.setRamp(steel);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(9, 2, 15, 2), cellDrag(9, 4, 15, 4)}});

        // Stripes, dragged sideways through the middle.
        s.push_back({QStringLiteral("smudge"), [] { return new SmudgeTool; },
                     [fire](Canvas& c) {
                         for (int y = 0; y < c.height(); ++y)
                             for (int x = 0; x < c.width(); ++x)
                                 c.at(x, y) = rungCell(fire, (x / 3) % 2 ? 4 : 12);
                     },
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 2, 0.75);
                     },
                     {cellDrag(1, 2, 14, 2), cellDrag(1, 3, 14, 3)}});

        s.push_back({QStringLiteral("glyph"),
                     [] {
                         auto* t = new GlyphTool;
                         t->setGlyph(U'♥');
                         return t;
                     },
                     {}, [](Document&, CanvasView& v) { setPen(v, LightRed); },
                     {cellDrag(1, 4, 4, 1), cellDrag(4, 1, 7, 4), cellDrag(7, 4, 10, 1), cellDrag(10, 1, 13, 4)}});

        // Grey text, its right half recoloured.
        s.push_back({QStringLiteral("colorize"), [] { return new ColorizeTool; },
                     [](Canvas& c) {
                         const std::u32string text = U"░▒▓█ colour ▓▒░";
                         for (int y = 0; y < c.height(); ++y)
                             for (int x = 0; x < c.width(); ++x)
                                 c.at(x, y) = Cell{text[(x + y) % text.size()], Color::indexed(Grey),
                                                   Color::indexed(Black)};
                     },
                     [](Document&, CanvasView& v) {
                         setPen(v, LightMagenta);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(9, 2, 15, 2), cellDrag(9, 4, 15, 4)}});

        s.push_back({QStringLiteral("line"), [] { return new ShapeTool(ShapeTool::Shape::Line); }, {},
                     [](Document&, CanvasView& v) { setPen(v, LightCyan); }, {drag({1, 10}, {14, 1})}});
        s.push_back({QStringLiteral("rectangle"), [] { return new ShapeTool(ShapeTool::Shape::Rectangle); }, {},
                     [](Document&, CanvasView& v) { setPen(v, Yellow); }, {drag({1, 1}, {14, 10})}});
        s.push_back({QStringLiteral("ellipse"), [] { return new ShapeTool(ShapeTool::Shape::Ellipse); }, {},
                     [](Document&, CanvasView& v) { setPen(v, LightMagenta); }, {drag({1, 0}, {14, 11})}});

        // A double-line frame over a field of dots, whose open middle it leaves.
        s.push_back({QStringLiteral("frame"),
                     [] {
                         auto* t = new FrameTool;
                         t->setStyle(QStringLiteral("double"));
                         t->setPenColours(true);
                         return t;
                     },
                     [](Canvas& c) {
                         for (int y = 0; y < c.height(); ++y)
                             for (int x = 0; x < c.width(); ++x)
                                 if ((x + y) % 2)
                                     c.at(x, y) = Cell{U'·', Color::indexed(DarkGrey), Color::indexed(Black)};
                     },
                     [](Document&, CanvasView& v) { setPen(v, Yellow); }, {cellDrag(1, 0, 14, 5)}});

        // An outline, flooded from inside.
        s.push_back({QStringLiteral("fill"), [] { return new FillTool; },
                     [](Canvas& c) {
                         for (const auto& [x, py] : ellipseShape(1, 0, 14, 11, false, c.width(), 2 * c.height()))
                             c.at(x, py / 2) = setHalfBlock(c.at(x, py / 2), py % 2, LightBlue);
                     },
                     [](Document&, CanvasView& v) { setPen(v, Cyan); }, {{{{7, 6}}}}});

        s.push_back({QStringLiteral("gradient"), [] { return new GradientTool; }, {},
                     [fire](Document&, CanvasView& v) { v.setRamp(fire); }, {drag({0, 6}, {15, 6})}});

        // A lit disc: its right half outlined.
        s.push_back({QStringLiteral("outline"), [] { return new OutlineTool; },
                     [fire](Canvas& c) { disc(c, 2, 0, 13, 5, rungCell(fire, 12)); },
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(8, 2, 15, 2), cellDrag(8, 4, 15, 4)}});

        s.push_back({QStringLiteral("rimLight"), [] { return new RimLightTool; },
                     [fire](Canvas& c) { disc(c, 2, 0, 13, 5, rungCell(fire, 4)); },
                     [fire](Document&, CanvasView& v) {
                         v.setRamp(fire);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(0, 2, 15, 2), cellDrag(0, 4, 15, 4)}});

        s.push_back({QStringLiteral("texture"), [] { return new TextureTool; }, {},
                     [](Document&, CanvasView& v) {
                         v.setRamp(ramp("wood"));
                         v.setTextureOptions({TextureKind::Brick, 0.6, 1});
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(0, 1, 15, 1), cellDrag(0, 3, 15, 3), cellDrag(0, 5, 15, 5)}});

        // Ctrl+click the art to take it as the source, then paint a copy.
        s.push_back({QStringLiteral("clone"), [] { return new CloneTool; }, [](Canvas& c) { putSprite(c, 1, 1); },
                     [](Document&, CanvasView& v) { setBrush(v, 2, 1.0); },
                     {{{cell(1, 1)}, Qt::LeftButton, Qt::ControlModifier},
                      {{cell(9, 1), cell(10, 1), cell(11, 1), cell(12, 1), cell(13, 1), cell(13, 2), cell(13, 3),
                        cell(12, 3), cell(11, 3), cell(10, 3), cell(9, 3), cell(9, 2)}}}});

        s.push_back({QStringLiteral("spray"),
                     [] {
                         auto* t = new SprayTool;
                         t->setDensity(100);
                         return t;
                     },
                     {},
                     [](Document&, CanvasView& v) {
                         setPen(v, White);
                         setBrush(v, 6, 1.0);
                     },
                     {cellDrag(0, 2, 15, 2), cellDrag(0, 4, 15, 4)}});

        s.push_back({QStringLiteral("text"), [] { return new TextTool; }, {}, {}, {{{cell(0, 1)}}},
                     QStringLiteral("Hi")});
        return s;
    }();
    return all;
}

const Script* scriptFor(const QString& id) {
    for (const Script& s : scripts())
        if (s.id == id)
            return &s;
    return nullptr;
}

// The view's mouse event at a half-block pixel, in widget coordinates.
void mouse(CanvasView& v, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
    const QRect c = v.cellRect(QPoint(at.x(), at.y() >> 1));
    const QPoint px(c.center().x(), c.top() + c.height() * ((at.y() & 1) ? 3 : 1) / 4);
    const QPointF w = QRectF(v.toWidget(QRect(px, QSize(1, 1)))).center();
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, w, v.mapToGlobal(w), type == QEvent::MouseMove ? Qt::NoButton : button, held, mods);
    QApplication::sendEvent(&v, &e);
}

void type(CanvasView& v, QChar ch) {
    const int key = ch.isLetter() ? Qt::Key_A + (ch.toUpper().unicode() - 'A') : ch.unicode();
    const Qt::KeyboardModifiers mods = ch.isUpper() ? Qt::ShiftModifier : Qt::NoModifier;
    QKeyEvent down(QEvent::KeyPress, key, mods, QString(ch));
    QApplication::sendEvent(&v, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods, QString(ch));
    QApplication::sendEvent(&v, &up);
}

std::optional<Illustration> run(const Script& s) {
    const ScratchSettings scratch;
    if (!scratch.valid())
        return {};
    Document doc;
    AnsiDocument start;
    start.canvas = Canvas(kW, kH);
    if (s.start)
        s.start(start.canvas);
    doc.replace(std::move(start), QString());
    AtlasRenderer renderer;
    renderer.setCanvas(&doc.canvas(), glyphWidth(doc.ansi()));
    CanvasView view(&doc, &renderer);
    const std::unique_ptr<Tool> tool(s.make());
    if (s.setup)
        s.setup(doc, view);
    view.setTool(tool.get());
    const Canvas before = doc.canvas();
    for (const Stroke& st : s.strokes) {
        if (st.at.empty())
            continue;
        mouse(view, QEvent::MouseButtonPress, st.at.front(), st.button, st.modifiers);
        for (std::size_t i = 1; i < st.at.size(); ++i)
            mouse(view, QEvent::MouseMove, st.at[i], st.button, st.modifiers);
        mouse(view, QEvent::MouseButtonRelease, st.at.back(), st.button, st.modifiers);
    }
    for (const QChar ch : s.typed)
        type(view, ch);
    view.finishTool();
    view.setTool(nullptr);
    renderer.setCanvas(&doc.canvas(), glyphWidth(doc.ansi()));
    renderer.flush();
    return Illustration{before, doc.canvas(), renderer.cache()};
}

} // namespace

std::optional<Illustration> makeToolIllustration(const QString& id) {
    const Script* s = scriptFor(id);
    return s ? run(*s) : std::nullopt;
}

const Illustration* toolIllustration(const QString& id) {
    static std::map<QString, std::optional<Illustration>> cache;
    auto it = cache.find(id);
    if (it == cache.end())
        it = cache.emplace(id, makeToolIllustration(id)).first;
    return it->second ? &*it->second : nullptr;
}

QStringList illustratedTools() {
    QStringList ids;
    for (const Script& s : scripts())
        ids << s.id;
    return ids;
}

const QList<std::pair<QString, QString>>& toolsWithoutIllustration() {
    static const QList<std::pair<QString, QString>> none{
        {QStringLiteral("eyedropper"), QStringLiteral("it picks a colour into the pen and changes no cells")},
    };
    return none;
}

QString scriptToolId(const QString& id) {
    const Script* s = scriptFor(id);
    const ScratchSettings scratch;
    if (!s || !scratch.valid())
        return {};
    const std::unique_ptr<Tool> t(s->make());
    return t->id();
}

} // namespace bw::help
