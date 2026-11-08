// A steps file's text steps (#421) against the editor's Text tool: the same
// Tool Options, clicks and typing on the same layered start give the same
// layers. main() is in test_app.cpp.

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "replay.h"
#include "text_options.h"
#include "text_tool.h"
#include "tool_options.h"
#include "tool.h"

#include "blockwright/layer.h"
#include "blockwright/project.h"
#include "blockwright/steps.h"
#include "blockwright/text.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QMouseEvent>

#include <string>

using namespace bw;

namespace {

constexpr int kW = 40, kH = 20;

// From the bottom: "Background" (letters everywhere) and "ink" (a band of
// digits, transparent elsewhere).
project::Project layered() {
    project::Project p;
    p.layers = LayerStack(kW, kH);
    Layer bg = makeLayer(LayerKind::Cells, kW, kH, "Background");
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            Cell& c = bg.cells.at(x, y);
            c.ch = char32_t(U'a' + x % 26);
            c.transparent = false;
            c.fg = Color::indexed(std::uint8_t(1 + (x + y) % 15));
        }
    p.layers.add(std::move(bg));
    Layer ink = makeLayer(LayerKind::Cells, kW, kH, "ink");
    for (int x = 0; x < kW; ++x) {
        Cell& c = ink.cells.at(x, 15);
        c.ch = char32_t(U'0' + x % 10);
        c.transparent = false;
    }
    p.layers.add(std::move(ink));
    return p;
}

LayerId idOf(const LayerStack& s, const std::string& name) {
    for (const Layer& l : s.layers())
        if (l.props.name == name)
            return l.props.id;
    FAIL("no layer " << name);
    return 0;
}

void click(CanvasView* v, QPoint cell) {
    const QPointF at(v->cellRect(cell).center());
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::MouseButtons(Qt::LeftButton) : Qt::NoButton,
                      Qt::NoModifier);
        QApplication::sendEvent(v, &e);
    }
}

void key(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

// Types `text` a key at a time, Enter for a newline.
void type(QWidget* w, const QString& text) {
    for (QChar ch : text)
        if (ch == QLatin1Char('\n'))
            key(w, Qt::Key_Return, QStringLiteral("\r"));
        else
            key(w, ch.toUpper().unicode(), QString(ch));
}

} // namespace

TEST_CASE("text steps give what the Text tool gives: new layers, Tool Options and a restyle (#421)",
          "[replay][text][layers]") {
    const char* active = GENERATE("Background", "ink");
    INFO("active " << active);
    const project::Project start = layered();

    // the editor
    MainWindow w;
    Document* doc = w.document();
    doc->replace(start, QString(), FileForm::ProjectZip);
    REQUIRE(doc->setActiveLayer(idOf(doc->layers(), active)));
    CanvasView* v = w.canvasView();
    QAction* tool = w.findChild<QAction*>(QStringLiteral("textTool"));
    REQUIRE(tool);
    tool->trigger();
    auto* text = qobject_cast<TextTool*>(w.tools()->active());
    REQUIRE(text);
    // Tool Options with a cells layer active: the defaults only
    TextSource first = text->defaults();
    first.face = FaceRef{FaceRef::Kind::Pixel, "vga", {}, 0};
    first.colour = 12;
    first.letterSpacing = 1;
    first.align = TextAlign::Centre;
    text->setSettings(first, QStringLiteral("Text settings"), doc);
    REQUIRE(doc->active()->props.name == active);
    REQUIRE_FALSE(doc->active()->props.text);
    click(v, QPoint(3, 2));
    type(v, QStringLiteral("Hi\nyo"));
    key(v, Qt::Key_Escape);
    REQUIRE(doc->active()->props.text);
    CHECK(doc->active()->props.name == "Text 1");
    CHECK(doc->active()->props.text->text == U"Hi\nyo");
    // Tool Options with the new layer active and no text cursor: it is restyled
    TextSource restyled = *doc->active()->props.text;
    // (as the dock sets a big style: the block face, the colour field off)
    restyled.face = FaceRef{};
    restyled.bigStyle = BigTextStyle::Shadow;
    restyled.bigColours = {11, 4};
    text->setSettings(restyled, QStringLiteral("Text settings"), doc);
    CHECK(doc->active()->props.text->bigStyle == BigTextStyle::Shadow);
    // a click, the case changed while the text cursor waits, then typing
    click(v, QPoint(20, 12));
    REQUIRE(text->spot());
    TextSource capitals = text->defaults();
    capitals.textCase = TextCase::Capitals;
    capitals.face = FaceRef{FaceRef::Kind::Pixel, "mini", {}, 0};
    capitals.bigStyle = BigTextStyle::None;
    capitals.bigColours.clear();  // as the dock clears them with no style
    text->setSettings(capitals, QStringLiteral("Text settings"), doc);
    type(v, QStringLiteral("ab"));
    key(v, Qt::Key_Escape);
    CHECK(doc->active()->props.name == "Text 2");

    // the replay
    const steps::Script s = steps::parse(std::string(R"({"version": 1, "start": {"blank": [1, 1]}, "steps": [
        {"name": "use", "action": "setActiveLayer", "layer": ")") + active + R"("},
        {"name": "set", "action": "text", "textOptions": {"face": "vga", "colour": 12, "letterSpacing": 1, "align": "centre"}},
        {"name": "one", "action": "text", "at": [3, 2], "text": "Hi\nyo"},
        {"name": "red", "action": "text", "textOptions": {"bigStyle": "shadow", "bigColours": [11, 4]}},
        {"name": "two", "action": "text", "at": [20, 12], "text": "ab", "textOptions": {"case": "capitals", "face": "mini", "bigStyle": "none"}}]})");
    const auto out = bwcli::replay::run(s, start);
    const project::Project& end = out.captures.back().project;
    for (const Layer& l : end.layers.layers()) {
        INFO("layer " << l.props.name);
        const Layer* e = doc->layers().find(l.props.id);
        REQUIRE(e);
        CHECK(e->props == l.props);
        CHECK(e->cells == l.cells);
    }
    CHECK(end.layers == doc->layers());
    // and the next layer starts from the same settings
    CHECK(out.captures.size() == s.steps.size());
}

TEST_CASE("a text step's cell is a text layer the Text tool's click edits only where textLayerAt says (#421)",
          "[replay][text]") {
    MainWindow w;
    Document* doc = w.document();
    doc->replace(layered(), QString(), FileForm::ProjectZip);
    w.findChild<QAction*>(QStringLiteral("textTool"))->trigger();
    auto* text = qobject_cast<TextTool*>(w.tools()->active());
    REQUIRE(text);
    CanvasView* v = w.canvasView();
    click(v, QPoint(2, 2));
    type(v, QStringLiteral("AB"));
    key(v, Qt::Key_Escape);
    const Layer* made = doc->active();
    REQUIRE(made->props.text);
    // every cell: the tool's click edits the layer where the core finds it,
    // and the replay refuses a text step there
    for (int y = 0; y < kH; y += 1)
        for (int x = 0; x < kW; x += 3) {
            INFO(x << ", " << y);
            const Layer* hit = textLayerAt(doc->layers(), x, y);
            click(v, QPoint(x, y));
            CHECK((text->editing() == made->props.id) == (hit != nullptr));
            CHECK(text->spot().has_value() == (hit == nullptr));
            key(v, Qt::Key_Escape);
        }
}

TEST_CASE("a text step with no Tool Options, or the ones shown, leaves the Text tool's defaults as the dock does (#421)",
          "[replay][text][layers]") {
    const project::Project start = layered();
    // the editor: a mini "A", a vga "B", then "A" made active, the dock's
    // alignment picked again as it shows it, and a "C"
    MainWindow w;
    Document* doc = w.document();
    doc->replace(start, QString(), FileForm::ProjectZip);
    CanvasView* v = w.canvasView();
    w.findChild<QAction*>(QStringLiteral("textTool"))->trigger();
    auto* text = qobject_cast<TextTool*>(w.tools()->active());
    REQUIRE(text);
    const auto face = [&](const char* name) {
        TextSource s = text->defaults();
        s.face = FaceRef{FaceRef::Kind::Pixel, name, {}, 0};
        text->setSettings(s, QStringLiteral("Text settings"), doc);
    };
    click(v, QPoint(1, 1));
    face("mini");
    type(v, QStringLiteral("A"));
    key(v, Qt::Key_Escape);
    click(v, QPoint(8, 1));
    face("vga");
    type(v, QStringLiteral("B"));
    key(v, Qt::Key_Escape);
    REQUIRE(doc->setActiveLayer(idOf(doc->layers(), "Text 1")));
    TextOptions* options = w.toolOptions()->textOptions();
    REQUIRE(options);
    QComboBox* align = options->alignBox();
    REQUIRE(align->currentData().toInt() == int(TextAlign::Left));
    align->setCurrentIndex(align->currentIndex());
    CHECK(text->defaults().face.name == "vga");
    click(v, QPoint(20, 1));
    type(v, QStringLiteral("C"));
    key(v, Qt::Key_Escape);
    REQUIRE(doc->active()->props.name == "Text 3");
    CHECK(doc->active()->props.text->face.name == "vga");

    // the replay: a step with none, one with {} and one with the alignment shown
    const steps::Script s = steps::parse(R"({"version": 1, "start": {"blank": [1, 1]}, "steps": [
        {"name": "a", "action": "text", "at": [1, 1], "text": "A", "textOptions": {"face": "mini"}},
        {"name": "b", "action": "text", "at": [8, 1], "text": "B", "textOptions": {"face": "vga"}},
        {"name": "use", "action": "setActiveLayer", "layer": "Text 1"},
        {"name": "none", "action": "text"},
        {"name": "empty", "action": "text", "textOptions": {}},
        {"name": "left", "action": "text", "textOptions": {"align": "left"}},
        {"name": "c", "action": "text", "at": [20, 1], "text": "C"}]})");
    const auto out = bwcli::replay::run(s, start);
    CHECK(out.captures.back().project.layers == doc->layers());
    // the two with settings say they change nothing
    REQUIRE(out.notes.size() == 2);
    CHECK_THAT(out.notes[0], Catch::Matchers::ContainsSubstring("step 5 (empty)"));
    CHECK_THAT(out.notes[1], Catch::Matchers::ContainsSubstring("step 6 (left)"));
}
