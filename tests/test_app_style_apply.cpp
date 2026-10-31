// Library text styles put to use (PLAN §12, §15): double-clicking one in the
// dock applies it to the active live text, cells or pixels layer as one undo
// step, dropping one on the canvas makes a new text layer in it as one step,
// the Layer Style dialog's Preset box offers them, a style that needs iCE
// colours is refused or shown disabled while the document has them off, and
// toggling iCE redraws a live text layer's style in the same step. The Text
// tool's caret and clicks go by the letters, inside the style's margins.

#include "canvas_view.h"
#include "document.h"
#include "layer_style_dialog.h"
#include "library_dock.h"
#include "main_window.h"
#include "text_tool.h"
#include "tool.h"

#include "blockwright/face.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/text.h"
#include "blockwright/text_style.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QUndoStack>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

const fs::path kBundled = BW_BUNDLED_LIBRARY;
constexpr int kW = 30, kH = 12;

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

library::TextStyleEntry preset(const char* file) {
    return library::readTextStyleEntry(kBundled / "text-styles" / file);
}

// Neon under another name, needing iCE colours.
library::TextStyleEntry hotNeon() {
    library::TextStyleEntry t = preset("neon.style.json");
    t.name = "Hot Neon";
    t.iceRequired = true;
    return t;
}

FaceRef mini() {
    FaceRef r;
    r.name = "mini";
    return r;
}

TextSource source(std::u32string text = U"HOT") {
    TextSource s;
    s.face = mini();
    s.text = std::move(text);
    return s;
}

// A Fill down the fire ramp, row by row: with iCE off its neighbouring rows
// step a rung at a time, so it draws differently with iCE on and off.
TextStyle fireRows() { return TextStyle{{Effect{true, FillEffect{FillMode::Rows, Grey, "fire"}}}}; }

const Cell kBlock{kFullBlock, Color::indexed(Grey), Color::indexed(Black), 0, false};

Layer artLayer(LayerKind kind) {
    Layer l;
    l.props.kind = kind;
    l.props.name = "art";
    l.cells = Canvas(kW, kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            l.cells.at(x, y) = transparentCell();
    l.cells.at(5, 3) = kind == LayerKind::Pixels ? pixelLayerCell(LayerPixels{Red, std::nullopt}) : kBlock;
    return l;
}

// A window whose Library is the repo's (bundled) and a scratch folder (user)
// holding Hot Neon.
struct Fixture {
    QTemporaryDir user;
    MainWindow w;
    LibraryDock* dock;
    Fixture() : dock(w.libraryWidget()) {
        REQUIRE(user.isValid());
        std::ofstream(fspath(user.path()) / "hot-neon.style.json", std::ios::binary)
            << library::textStyleEntryJson(hotNeon());
        dock->setDirectories(kBundled, fspath(user.path()));
    }
    Document* doc() { return w.document(); }
    // Opens a project of `layer`, iCE `ice`, with the layer active.
    LayerId open(Layer layer, bool ice = true) {
        project::Project p;
        p.ice = ice;
        p.layers = LayerStack(kW, kH);
        const LayerId id = p.layers.add(std::move(layer));
        doc()->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
        doc()->setActiveLayer(id);
        REQUIRE(doc()->activeLayer() == id);
        REQUIRE(dock->iceColours() == ice);
        return id;
    }
    QListWidgetItem* item(const QString& name) {
        for (int i = 0; i < dock->grid()->count(); ++i)
            if (dock->grid()->item(i)->text() == name)
                return dock->grid()->item(i);
        FAIL("nothing named " << name.toStdString() << " in the grid");
        return nullptr;
    }
    // Double-clicks the style named `name`.
    void activate(const QString& name) { emit dock->grid()->itemActivated(item(name)); }
    std::unique_ptr<QMimeData> dragged(const QString& name) {
        const QModelIndex index = dock->grid()->model()->index(dock->grid()->row(item(name)), 0);
        return std::unique_ptr<QMimeData>(dock->grid()->model()->mimeData({index}));
    }
    QString status() { return w.statusBar()->currentMessage(); }
};

const Layer& layerOf(Document* d, LayerId id) {
    const Layer* l = d->layers().find(id);
    REQUIRE(l);
    return *l;
}

// Drags `mime` over the canvas and drops it on `cell`; whether the view
// accepted the drop.
bool drop(CanvasView* v, const QMimeData* mime, QPoint cell) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    const QPointF pos = v->toWidget(v->cellRect(cell)).center();
#else
    const QPoint pos = v->toWidget(v->cellRect(cell)).center();
#endif
    QDragEnterEvent enter(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &move);
    QDropEvent d(QPointF(pos), Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(v, &d);
    return d.isAccepted() && d.dropAction() == Qt::CopyAction;
}

// A left click in the middle of canvas cell `cell`.
void click(CanvasView* v, QPoint cell) {
    const QPointF at = v->cellRect(cell).center();
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

TextTool* textTool(MainWindow& w) {
    auto* t = qobject_cast<TextTool*>(w.tools()->find(QStringLiteral("text")));
    REQUIRE(t);
    return t;
}

LayerStyleDialog* openDialog(MainWindow& w) {
    auto* a = w.findChild<QAction*>(QStringLiteral("layerStyle"));
    REQUIRE(a);
    REQUIRE(a->isEnabled());
    a->trigger();
    auto* d = w.findChild<LayerStyleDialog*>();
    REQUIRE(d);
    return d;
}

int presetIndex(const LayerStyleDialog& d, const std::string& name) {
    for (std::size_t i = 0; i < d.presets().size(); ++i)
        if (d.presets()[i].name == name)
            return int(i);
    FAIL("no preset named " << name);
    return -1;
}

// The Preset box's model item for preset `index` (row 0 is "Choose…").
QStandardItem* presetItem(const LayerStyleDialog& d, int index) {
    auto* model = qobject_cast<QStandardItemModel*>(d.presetBox()->model());
    REQUIRE(model);
    return model->item(index + 1);
}

} // namespace

TEST_CASE("double-clicking a Library text style styles the active live text layer, one undo step", "[style][library]") {
    Fixture f;
    const LayerId id = f.open(makeTextLayer(source()));
    const Canvas plain = layerOf(f.doc(), id).cells;
    const library::TextStyleEntry neon = preset("neon.style.json");
    const int steps = f.doc()->undoStack()->count();

    f.activate(QStringLiteral("Neon"));
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(f.doc()->undoStack()->undoText() == QStringLiteral("Text Style Neon"));
    CHECK(layerOf(f.doc(), id).props.style == neon.style);
    CHECK(layerOf(f.doc(), id).props.text == source());
    const Canvas styled = renderStyledText(source(), neon.style, miniFace(), true).cells;
    CHECK(layerOf(f.doc(), id).cells == styled);

    // The same style again is no step.
    f.activate(QStringLiteral("Neon"));
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(f.status().contains(QStringLiteral("the layer has this style already")));

    // Another replaces the whole stack.
    const library::TextStyleEntry outline = preset("outline.style.json");
    f.activate(QString::fromStdString(outline.name));
    CHECK(f.doc()->undoStack()->count() == steps + 2);
    CHECK(layerOf(f.doc(), id).props.style == outline.style);

    f.doc()->undoStack()->undo();
    CHECK(layerOf(f.doc(), id).props.style == neon.style);
    f.doc()->undoStack()->undo();
    CHECK_FALSE(layerOf(f.doc(), id).props.style);
    CHECK(layerOf(f.doc(), id).cells == plain);
    f.doc()->undoStack()->redo();
    CHECK(layerOf(f.doc(), id).props.style == neon.style);
    CHECK(layerOf(f.doc(), id).cells == styled);
}

TEST_CASE("double-clicking a text style styles a cells or pixels layer too", "[style][library]") {
    Fixture f;
    const LayerKind kind = GENERATE(LayerKind::Cells, LayerKind::Pixels);
    const LayerId id = f.open(artLayer(kind));
    const Canvas cells = layerOf(f.doc(), id).cells;
    const Canvas before = f.doc()->canvas();
    const library::TextStyleEntry outline = preset("outline.style.json");
    const int steps = f.doc()->undoStack()->count();

    f.activate(QString::fromStdString(outline.name));
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(layerOf(f.doc(), id).props.style == outline.style);
    // Its own cells stay; the style is drawn as it composites.
    CHECK(layerOf(f.doc(), id).cells == cells);
    CHECK(f.doc()->canvas() != before);
    f.doc()->undoStack()->undo();
    CHECK_FALSE(layerOf(f.doc(), id).props.style);
    CHECK(f.doc()->canvas() == before);
}

TEST_CASE("a text style isn't applied to a layer that can't take one, nor one needing iCE while it's off",
          "[style][library]") {
    Fixture f;
    SECTION("a baked text layer") {
        Layer l = makeTextLayer(source());
        l.props.text.reset();
        f.open(std::move(l));
    }
    SECTION("a generator layer") {
        Layer l = artLayer(LayerKind::Cells);
        l.props.kind = LayerKind::Generator;
        f.open(std::move(l));
    }
    const int steps = f.doc()->undoStack()->count();
    f.activate(QStringLiteral("Neon"));
    CHECK(f.doc()->undoStack()->count() == steps);
    CHECK_FALSE(f.doc()->active()->props.style);
    CHECK(f.status() == QStringLiteral("Text style not applied: a text style goes on a live text, cells or pixels layer"));
}

TEST_CASE("a style needing iCE isn't applied or dropped while iCE is off", "[style][library]") {
    Fixture f;
    const LayerId id = f.open(makeTextLayer(source()), false);
    REQUIRE_FALSE(f.item(QStringLiteral("Hot Neon"))->flags().testFlag(Qt::ItemIsEnabled));
    const int steps = f.doc()->undoStack()->count();
    const std::size_t layers = f.doc()->layers().size();

    // The disabled item can't be double-clicked; asked anyway, it says why.
    f.activate(QStringLiteral("Hot Neon"));
    CHECK(f.doc()->undoStack()->count() == steps);
    CHECK_FALSE(layerOf(f.doc(), id).props.style);
    CHECK(f.status() ==
          QStringLiteral("Text style not applied: Hot Neon needs iCE colours, which are off (View > iCE Colours)"));
    QString error;
    CHECK(f.w.newStyledText(hotNeon(), {2, 2}, &error) == 0);
    CHECK(error == QStringLiteral("Hot Neon needs iCE colours, which are off (View > iCE Colours)"));
    CHECK(f.doc()->layers().size() == layers);
    CHECK(f.doc()->undoStack()->count() == steps);

    // With iCE on it applies.
    f.w.setIceColours(true);
    const int on = f.doc()->undoStack()->count();
    f.activate(QStringLiteral("Hot Neon"));
    CHECK(f.doc()->undoStack()->count() == on + 1);
    CHECK(layerOf(f.doc(), id).props.style == hotNeon().style);
}

TEST_CASE("dropping a text style on the canvas makes a new text layer in it there, one undo step",
          "[style][library][text]") {
    Fixture f;
    f.open(artLayer(LayerKind::Cells));
    const library::TextStyleEntry neon = preset("neon.style.json");
    const std::size_t layers = f.doc()->layers().size();
    const int steps = f.doc()->undoStack()->count();

    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(mime);
    REQUIRE(mime->hasFormat(QString::fromLatin1(kTextStyleMimeType)));
    REQUIRE(drop(f.w.canvasView(), mime.get(), {4, 2}));

    REQUIRE(f.doc()->layers().size() == layers + 1);
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(f.doc()->undoStack()->undoText() == QStringLiteral("New Text Layer"));
    const LayerId id = f.doc()->activeLayer();
    const Layer& l = layerOf(f.doc(), id);
    CHECK(l.props.kind == LayerKind::Text);
    REQUIRE(l.props.text);
    CHECK(l.props.text->text == U"Text");
    CHECK(l.props.style == neon.style);
    const FontSources fonts = f.doc()->fontSources();
    const Face* face = findFace(l.props.text->face, fonts);
    REQUIRE(face);
    CHECK(l.cells == renderStyledText(*l.props.text, neon.style, *face, true).cells);
    // It sits where it was dropped, less the style's margin above and left.
    CHECK(l.props.x <= 4);
    CHECK(l.props.y <= 2);

    // The Text tool is editing it, the caret after the word.
    CHECK(f.w.tools()->active()->id() == QStringLiteral("text"));
    CHECK(textTool(f.w)->editing() == id);
    CHECK(textTool(f.w)->caret() == 4);

    f.doc()->undoStack()->undo();
    CHECK(f.doc()->layers().size() == layers);
    CHECK_FALSE(f.doc()->layers().find(id));
    f.doc()->undoStack()->redo();
    REQUIRE(f.doc()->layers().find(id));
    CHECK(layerOf(f.doc(), id).props.style == neon.style);
}

TEST_CASE("a stamp dropped on the canvas still floats; a style isn't a stamp", "[style][library]") {
    Fixture f;
    f.open(artLayer(LayerKind::Cells));
    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(mime);
    CHECK_FALSE(mime->hasFormat(QString::fromLatin1(kStampMimeType)));
    // A style key the Library doesn't have is refused with a message.
    QMimeData unknown;
    unknown.setData(QString::fromLatin1(kTextStyleMimeType), QByteArray("{}"));
    const std::size_t layers = f.doc()->layers().size();
    CHECK(drop(f.w.canvasView(), &unknown, {1, 1}));
    CHECK(f.doc()->layers().size() == layers);
    CHECK(f.status() == QStringLiteral("No text layer made: not a text style in the Library"));
}

TEST_CASE("the Layer Style dialog's Preset box offers the Library's styles, an iCE-only one disabled while iCE is off",
          "[style][library]") {
    Fixture f;
    const bool ice = GENERATE(false, true);
    const LayerId id = f.open(makeTextLayer(source()), ice);
    LayerStyleDialog* d = openDialog(f.w);
    REQUIRE(d->presets().size() == f.dock->styles().size());
    CHECK(d->presetBox()->count() == int(d->presets().size()) + 1);

    const int hot = presetIndex(*d, "Hot Neon");
    QStandardItem* it = presetItem(*d, hot);
    REQUIRE(it);
    CHECK(it->text() == QStringLiteral("Hot Neon"));
    CHECK(it->isEnabled() == ice);
    if (ice) {
        CHECK(it->toolTip() == QStringLiteral("Needs iCE colours"));
    } else {
        CHECK(it->toolTip() ==
              QStringLiteral("Needs iCE colours, which are off: turn on View > iCE Colours to use it"));
        CHECK_FALSE(d->usePreset(hot));
        CHECK_FALSE(d->style());
    }
    const int neon = presetIndex(*d, "Neon");
    CHECK(presetItem(*d, neon)->isEnabled());
    CHECK(presetItem(*d, neon)->toolTip() == QStringLiteral("Works with iCE colours on or off"));

    // Choosing one loads its effects, in its order.
    const library::TextStyleEntry outline = preset("outline.style.json");
    d->presetBox()->setCurrentIndex(presetIndex(*d, outline.name) + 1);
    emit d->presetBox()->activated(presetIndex(*d, outline.name) + 1);
    CHECK(d->presetBox()->currentIndex() == 0);
    CHECK(d->style() == outline.style);
    REQUIRE(d->usePreset(neon));
    CHECK(d->style() == preset("neon.style.json").style);

    const int steps = f.doc()->undoStack()->count();
    d->buttons()->button(QDialogButtonBox::Ok)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(layerOf(f.doc(), id).props.style == preset("neon.style.json").style);
}

TEST_CASE("toggling iCE redraws a live text layer's style in the same undo step", "[style][text][ice]") {
    const Canvas on = renderStyledText(source(), fireRows(), miniFace(), true).cells;
    const Canvas off = renderStyledText(source(), fireRows(), miniFace(), false).cells;
    REQUIRE(on != off);

    Fixture f;
    Layer styled = makeTextLayer(source());
    REQUIRE(applyLayerProperties(styled, [&] {
        LayerProperties p = styled.props;
        p.style = fireRows();
        return p;
    }()));
    REQUIRE(styled.cells == on);
    // A plain text layer beside it, which iCE doesn't change.
    Layer plainLayer = makeTextLayer(source(U"LO"), "plain", 0, 6);
    const Canvas plain = plainLayer.cells;

    project::Project p;
    p.ice = true;
    p.layers = LayerStack(kW, kH);
    const LayerId id = p.layers.add(std::move(styled));
    const LayerId other = p.layers.add(std::move(plainLayer));
    f.doc()->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
    const Canvas composite = f.doc()->canvas();
    const int steps = f.doc()->undoStack()->count();

    f.w.findChild<QAction*>(QStringLiteral("iceColours"))->trigger();
    REQUIRE_FALSE(f.doc()->ansi().ice);
    CHECK(f.doc()->undoStack()->count() == steps + 1);
    CHECK(layerOf(f.doc(), id).cells == off);
    CHECK(layerOf(f.doc(), id).props.style == fireRows());
    CHECK(layerOf(f.doc(), other).cells == plain);
    CHECK(f.doc()->canvas() != composite);

    f.doc()->undoStack()->undo();
    CHECK(f.doc()->ansi().ice);
    CHECK(layerOf(f.doc(), id).cells == on);
    CHECK(f.doc()->canvas() == composite);
    f.doc()->undoStack()->redo();
    CHECK_FALSE(f.doc()->ansi().ice);
    CHECK(layerOf(f.doc(), id).cells == off);

    // Typing on it with iCE off draws with iCE off.
    TextSource more = source(U"HOTT");
    REQUIRE(f.doc()->setTextSource(id, more, QStringLiteral("Type")));
    CHECK(layerOf(f.doc(), id).cells == renderStyledText(more, fireRows(), miniFace(), false).cells);
    // And back on, with iCE on.
    f.w.setIceColours(true);
    CHECK(layerOf(f.doc(), id).cells == renderStyledText(more, fireRows(), miniFace(), true).cells);
    f.doc()->undoStack()->undo();
    CHECK(layerOf(f.doc(), id).cells == renderStyledText(more, fireRows(), miniFace(), false).cells);
}

TEST_CASE("a dropped style's caret is drawn after the letters, where the style put them", "[style][library][text]") {
    Fixture f;
    REQUIRE(f.doc()->canvas().width() == 80);
    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(drop(f.w.canvasView(), mime.get(), {4, 2}));
    TextTool* t = textTool(f.w);
    REQUIRE(t->caret() == 4);
    // Neon draws a cell round the letters: they start at (5, 3), and "Text"
    // in the default face is 32 columns.
    CHECK(styleMargins(preset("neon.style.json").style) == StyleMargins{1, 1, 1, 1});
    CHECK(t->caretCell() == QPoint(37, 3));
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == f.w.canvasView()->cellRect(QPoint(37, 3)).topLeft());
    // A click on the first letter's left half puts the caret before it.
    click(f.w.canvasView(), {6, 3});
    CHECK(t->editing() == f.doc()->activeLayer());
    CHECK(t->caret() == 0);
    CHECK(t->caretCell() == QPoint(5, 3));
}

// A left-button mouse event with Alt held in the middle of canvas cell
// `cell`: an Alt-drag of the active layer.
void altMouse(CanvasView* v, QEvent::Type type, QPoint cell) {
    const QPointF at = v->toWidget(v->cellRect(cell)).center();
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(v, &e);
}

// What the tool draws over the canvas: the Text tool's dashed box and caret.
QImage overlay(const Tool& t, const CanvasView& v) {
    QImage image(v.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    t.paint(p);
    return image;
}

TEST_CASE("the Text tool's caret and box follow an Alt-drag of its live text layer, and its end",
          "[style][library][text][layer-move]") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(drop(v, mime.get(), {4, 2}));
    TextTool* t = textTool(f.w);
    const LayerId id = f.doc()->activeLayer();
    REQUIRE(t->editing() == id);
    REQUIRE(t->caretCell() == QPoint(37, 3));
    const QImage before = overlay(*t, *v);
    const Canvas atRest = f.doc()->canvas();
    const bool commit = GENERATE(false, true);
    int changed = 0;
    QObject::connect(t, &Tool::changed, &f.w, [&] { ++changed; });

    // Ten columns right, not released: the letters show at x 14, the layer
    // is still at 4, and the caret and box are drawn with the letters.
    altMouse(v, QEvent::MouseButtonPress, {10, 4});
    altMouse(v, QEvent::MouseMove, {20, 4});
    REQUIRE(f.doc()->canvas() != atRest);
    REQUIRE(layerOf(f.doc(), id).props.x == 4);
    CHECK(changed > 0);  // the view repaints the tool's marks
    CHECK(t->editing() == id);
    CHECK(t->caretCell() == QPoint(47, 3));
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == v->cellRect(QPoint(47, 3)).topLeft());
    CHECK(v->inputMethodQuery(Qt::ImCursorRectangle).toRect() == v->toWidget(*t->caretRect()));
    const QImage dragged = overlay(*t, *v);
    CHECK(dragged != before);

    changed = 0;
    if (commit) {
        // The release lands the move where the preview showed it.
        altMouse(v, QEvent::MouseButtonRelease, {20, 4});
        REQUIRE(layerOf(f.doc(), id).props.x == 14);
        CHECK(t->caretCell() == QPoint(47, 3));
        CHECK(overlay(*t, *v) == dragged);
    } else {
        // Escape drops the preview, and the caret and box go back with it.
        key(v, Qt::Key_Escape);
        altMouse(v, QEvent::MouseButtonRelease, {20, 4});
        REQUIRE(layerOf(f.doc(), id).props.x == 4);
        CHECK(t->caretCell() == QPoint(37, 3));
        CHECK(overlay(*t, *v) == before);
    }
    CHECK(changed > 0);
    CHECK(t->editing() == id);
    REQUIRE(t->caretRect());
    CHECK(v->inputMethodQuery(Qt::ImCursorRectangle).toRect() == v->toWidget(*t->caretRect()));
}

TEST_CASE("with no edit open, the Text tool's caret rectangle follows an Alt-drag of the active live text layer",
          "[style][library][text][layer-move]") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(drop(v, mime.get(), {4, 2}));
    TextTool* t = textTool(f.w);
    const LayerId id = f.doc()->activeLayer();
    key(v, Qt::Key_Escape);  // ends the edit; the layer stays active
    REQUIRE(t->editing() == 0);
    REQUIRE(f.doc()->activeLayer() == id);
    REQUIRE(t->caretRect());
    REQUIRE(t->caretRect()->topLeft() == v->cellRect(QPoint(37, 3)).topLeft());

    altMouse(v, QEvent::MouseButtonPress, {10, 4});
    altMouse(v, QEvent::MouseMove, {20, 4});
    REQUIRE(layerOf(f.doc(), id).props.x == 4);
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == v->cellRect(QPoint(47, 3)).topLeft());
    CHECK(v->inputMethodQuery(Qt::ImCursorRectangle).toRect() == v->toWidget(*t->caretRect()));
    key(v, Qt::Key_Escape);
    altMouse(v, QEvent::MouseButtonRelease, {20, 4});
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == v->cellRect(QPoint(37, 3)).topLeft());
}

TEST_CASE("an Alt-drag repaints the Text tool's marks only for the layer they are on",
          "[style][library][text][layer-move]") {
    Fixture f;
    CanvasView* v = f.w.canvasView();
    TextTool* t = textTool(f.w);
    const bool text = GENERATE(false, true);
    std::unique_ptr<QMimeData> mime;
    if (text) {
        mime = f.dragged(QStringLiteral("Neon"));
        REQUIRE(drop(v, mime.get(), {4, 2}));
        REQUIRE(t->editing() == f.doc()->activeLayer());
    } else {
        f.w.tools()->setActive(t);
        REQUIRE(!f.doc()->active()->props.text);
        REQUIRE(!t->caretRect());
    }
    REQUIRE(f.w.tools()->active() == t);
    const LayerId id = f.doc()->activeLayer();
    const int x = layerOf(f.doc(), id).props.x;
    int changed = 0;
    QObject::connect(t, &Tool::changed, &f.w, [&] { ++changed; });

    // Twenty one-column steps: a repaint each for the edited layer's box and
    // caret, none for a layer the tool draws nothing on.
    altMouse(v, QEvent::MouseButtonPress, {10, 4});
    for (int c = 11; c <= 30; ++c)
        altMouse(v, QEvent::MouseMove, {c, 4});
    CHECK(changed == (text ? 20 : 0));

    // The release lands the move: a repaint for each signal its undo step
    // sends the tool (the index, and the clean state if that changes), and
    // none from the drag's end.
    changed = 0;
    int told = 0;
    QObject::connect(f.doc()->undoStack(), &QUndoStack::indexChanged, &f.w, [&] { ++told; });
    QObject::connect(f.doc()->undoStack(), &QUndoStack::cleanChanged, &f.w, [&] { ++told; });
    altMouse(v, QEvent::MouseButtonRelease, {30, 4});
    REQUIRE(layerOf(f.doc(), id).props.x == x + 20);
    REQUIRE(told > 0);
    CHECK(changed == told);
}

TEST_CASE("the Text tool's caret and clicks follow a live text layer's style margins, however it got its style",
          "[style][library][text]") {
    Fixture f;
    const char* file = GENERATE("neon.style.json", "shadow.style.json");
    const bool dialog = GENERATE(false, true);
    const library::TextStyleEntry entry = preset(file);
    const StyleMargins m = styleMargins(entry.style);
    REQUIRE((m.left > 0 && m.top > 0));
    const QPoint shift(m.left, m.top);
    CanvasView* v = f.w.canvasView();
    TextTool* t = textTool(f.w);

    // A plain text layer typed at (10, 4).
    f.w.tools()->setActive(t);
    click(v, {10, 4});
    for (const QChar ch : QStringLiteral("Text"))
        key(v, ch.toUpper().unicode(), QString(ch));
    const LayerId id = f.doc()->activeLayer();
    REQUIRE(t->editing() == id);
    REQUIRE_FALSE(layerOf(f.doc(), id).props.style);
    const int width = layerOf(f.doc(), id).cells.width();
    const int row = 4 + layerOf(f.doc(), id).cells.height() / 2;
    const QPoint end = t->caretCell();
    // The caret each click along a row of the letters puts.
    std::vector<int> plain;
    std::vector<QPoint> plainCells;
    for (int x = 10; x < 10 + width; ++x) {
        click(v, {x, row});
        plain.push_back(t->caret());
        plainCells.push_back(t->caretCell());
    }
    REQUIRE(plain.front() == 0);
    REQUIRE(plain.back() == 4);
    click(v, {10 + width / 2, row});
    const int caret = t->caret();
    const QPoint at = t->caretCell();

    // Styled mid-edit: the caret stays on its character, which moved with
    // the letters.
    if (dialog) {
        LayerStyleDialog* d = openDialog(f.w);
        REQUIRE(d->usePreset(presetIndex(*d, entry.name)));
        d->buttons()->button(QDialogButtonBox::Ok)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    } else {
        f.activate(QString::fromStdString(entry.name));
    }
    REQUIRE(layerOf(f.doc(), id).props.style == entry.style);
    REQUIRE(layerOf(f.doc(), id).props.x == 10);
    REQUIRE(t->editing() == id);
    CHECK(t->caret() == caret);
    CHECK(t->caretCell() == at + shift);
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == v->cellRect(at + shift).topLeft());

    // Each click on the letters puts the caret it put before they moved.
    for (int x = 10; x < 10 + width; ++x) {
        INFO("column " << x - 10 << " of the letters");
        click(v, QPoint(x, row) + shift);
        CHECK(t->caret() == plain[std::size_t(x - 10)]);
        CHECK(t->caretCell() == plainCells[std::size_t(x - 10)] + shift);
    }

    // With the edit ended the input method's caret is at the end of the
    // letters.
    key(v, Qt::Key_Escape);
    REQUIRE(t->editing() == 0);
    REQUIRE(f.doc()->activeLayer() == id);
    REQUIRE(t->caretRect());
    CHECK(t->caretRect()->topLeft() == v->cellRect(end + shift).topLeft());
    // A key resumes the edit there.
    key(v, Qt::Key_End);
    CHECK(t->editing() == id);
    CHECK(t->caretCell() == end + shift);
}

TEST_CASE("a dropped text layer emptied and left goes, with its steps and nothing to redo", "[style][library][text]") {
    Fixture f;
    const LayerId before = f.open(artLayer(LayerKind::Cells));
    const std::size_t layers = f.doc()->layers().size();
    QUndoStack* undo = f.doc()->undoStack();
    const int count = undo->count(), index = undo->index();
    const std::unique_ptr<QMimeData> mime = f.dragged(QStringLiteral("Neon"));
    REQUIRE(drop(f.w.canvasView(), mime.get(), {4, 2}));
    const LayerId id = f.doc()->activeLayer();
    REQUIRE(id != before);
    REQUIRE(textTool(f.w)->editing() == id);

    for (int i = 0; i < 4; ++i)
        key(f.w.canvasView(), Qt::Key_Backspace);
    REQUIRE(layerOf(f.doc(), id).props.text->text.empty());
    REQUIRE(undo->count() > count + 1);

    SECTION("Escape") { key(f.w.canvasView(), Qt::Key_Escape); }
    SECTION("another tool") {
        f.w.tools()->setActive(QStringLiteral("fill"));
        REQUIRE(f.w.tools()->active()->id() == QStringLiteral("fill"));
    }
    CHECK_FALSE(f.doc()->layers().find(id));
    CHECK(f.doc()->layers().size() == layers);
    CHECK(f.doc()->activeLayer() == before);
    CHECK(undo->count() == count);
    CHECK(undo->index() == index);
    CHECK_FALSE(undo->canRedo());
    CHECK(textTool(f.w)->editing() == 0);
}
