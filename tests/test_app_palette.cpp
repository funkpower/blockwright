// Palette swaps and remaps in the editor (#497, PLAN §8, Palettes): the
// canvas, the palette dock and the 1x preview draw the document's palette;
// Colours > Palette, Edit Palette..., Fit to Palette and Remap Colours...
// are one undo step each; a project keeps the palette, an .ans save reports
// it lost. main() is in test_app.cpp.

#include "util.h"

#include "blockwright/canvas_renderer.h"
#include "blockwright/project.h"
#include "blockwright/text_source.h"
#include "blockwright/text_style.h"
#include "blockwright/render.h"
#include "blockwright/xbin.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "palette_dialogs.h"
#include "palette_widget.h"
#include "shown_palette.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QComboBox>
#include <QIcon>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <functional>

using namespace bw;

namespace {

QAction* action(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

QString fixture() { return QString::fromStdString((bwtest::dataDir() / "xbin/tux-palette-font.xb").string()); }

Cell cell(std::uint8_t fg, std::uint8_t bg, char32_t ch = U'A') {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

void put(Document* d, int x, int y, Cell c) {
    CellEdit e;
    e.changes.push_back({x, y, d->layerCanvas().at(x, y), c});
    REQUIRE(d->edit(e));
}

// What the canvas looks like: the composite drawn in the document's palette.
QImage look(Document* d) { return renderCanvas(d->canvas(), 8, 1, vgaFont(), d->palette()); }

// Opens the modal dialog `trigger` starts, lets `fill` set it, then presses OK.
template <typename D>
void runDialog(QAction* trigger, const std::function<void(D*)>& fill) {
    bool ran = false;
    QTimer::singleShot(0, trigger, [&] {
        auto* d = qobject_cast<D*>(QApplication::activeModalWidget());
        REQUIRE(d);
        fill(d);
        ran = true;
        d->buttons()->button(QDialogButtonBox::Ok)->click();
    });
    trigger->trigger();
    CHECK(ran);
}

} // namespace

TEST_CASE("an XBIN with its own palette draws in it on the canvas, in the preview and in the dock", "[app][palette]") {
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(fixture(), &error));
    const Palette own = *parseXbin(bwtest::readFile(fixture().toStdString())).palette;
    REQUIRE(own != kPalette);
    CHECK(w.document()->palette() == own);
    CHECK(w.renderer().palette() == own);
    CHECK(w.paletteWidget()->colours() == own);
    // the 1x cache the canvas and the preview draw from is the render in it
    auto& r = const_cast<CanvasRenderer&>(w.renderer());
    r.flush();
    const auto* atlas = dynamic_cast<const AtlasRenderer*>(&w.renderer());
    REQUIRE(atlas);
    CHECK(atlas->cache() == renderCanvas(w.document()->canvas(), 8, 1, w.renderer().font(), own));

    // a new document is in the standard colours again
    w.document()->clear();
    CHECK(w.renderer().palette() == kPalette);
    CHECK(w.paletteWidget()->colours() == kPalette);
}

TEST_CASE("Colours > Palette sets a preset as one undo step; the cells stay", "[app][palette]") {
    MainWindow w;
    Document* d = w.document();
    put(d, 0, 0, cell(4, 1));
    const Canvas cells = d->canvas();
    const int steps = d->undoStack()->count();
    action(w, QStringLiteral("palette_xterm"))->trigger();
    const Palette xterm = palettePreset("xterm")->colours;
    CHECK(d->palette() == xterm);
    CHECK(d->ansi().palette == xterm);
    CHECK(d->canvas() == cells);
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Palette"));
    CHECK(w.renderer().palette() == xterm);
    CHECK(w.paletteWidget()->colours() == xterm);
    CHECK(d->isModified());
    // the same again is no step
    action(w, QStringLiteral("palette_xterm"))->trigger();
    CHECK(d->undoStack()->count() == steps + 1);
    d->undoStack()->undo();
    CHECK(d->palette() == kPalette);
    CHECK_FALSE(d->ansi().palette);
    CHECK(w.renderer().palette() == kPalette);
    d->undoStack()->redo();
    CHECK(d->palette() == xterm);
    // the standard preset is no palette at all
    action(w, QStringLiteral("palette_cga"))->trigger();
    CHECK_FALSE(d->ansi().palette);
    // a .ans save of a piece with its own palette is lossy: paletteDropped
    action(w, QStringLiteral("palette_mirc"))->trigger();
    CHECK(d->encode(ArtFormat::Ansi).paletteDropped);
    CHECK(d->encode(ArtFormat::Bin).paletteDropped);
    CHECK_FALSE(d->encode(ArtFormat::Xbin).paletteDropped);
}

TEST_CASE("Palette > From File takes an XBIN's or a palette file's colours", "[app][palette]") {
    MainWindow w;
    QString error;
    REQUIRE(w.loadPaletteFile(fixture(), &error));
    CHECK(w.document()->palette() == *parseXbin(bwtest::readFile(fixture().toStdString())).palette);
    QTemporaryDir tmp;
    QFile f(tmp.filePath(QStringLiteral("short.hex")));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("ff0000\n00ff00\n");
    f.close();
    const Palette was = w.document()->palette();
    CHECK_FALSE(w.loadPaletteFile(f.fileName(), &error));
    CHECK(error == QStringLiteral("the palette has 2 colours; it needs 16"));
    CHECK(w.document()->palette() == was);
    CHECK_FALSE(w.loadPaletteFile(tmp.filePath(QStringLiteral("missing.gpl")), &error));
    CHECK_FALSE(error.isEmpty());
}

TEST_CASE("Edit Palette... changes entries, rounded as an XBIN holds them, as one undo step", "[app][palette]") {
    MainWindow w;
    Document* d = w.document();
    const int steps = d->undoStack()->count();
    runDialog<PaletteEditDialog>(action(w, QStringLiteral("editPalette")), [](PaletteEditDialog* dlg) {
        CHECK(dlg->palette() == kPalette);
        dlg->setColour(3, {0x80, 0x10, 0xfe});
        dlg->setColour(12, {1, 2, 3});
        CHECK(dlg->swatch(3)->toolTip().contains(QStringLiteral("#82")));
    });
    Palette want = kPalette;
    want[3] = xbinColour({0x80, 0x10, 0xfe});
    want[12] = xbinColour({1, 2, 3});
    CHECK(d->palette() == want);
    CHECK(xbinRoundedEntries(d->palette()) == 0);
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Edit Palette"));
    d->undoStack()->undo();
    CHECK(d->palette() == kPalette);
}

TEST_CASE("Remap Colours of a selection and of the document is one undo step each", "[app][palette][remap]") {
    MainWindow w;
    Document* d = w.document();
    for (int x = 0; x < 4; ++x)
        put(d, x, 0, cell(3, 11));
    put(d, 0, 1, cell(11, 3));
    const Canvas before = d->canvas();
    const int steps = d->undoStack()->count();

    // the selection: cells 1-2 of row 0
    QString error;
    CHECK(d->remapColours(swapColourMap(3, 11), Document::RemapScope::Selection, CellRect{1, 0, 2, 1}, &error) == 2);
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Remap Colours"));
    CHECK(d->canvas().at(0, 0).fg == Color::indexed(3));
    CHECK(d->canvas().at(1, 0).fg == Color::indexed(11));
    CHECK(d->canvas().at(1, 0).bg == Color::indexed(3));
    CHECK(d->canvas().at(2, 0).fg == Color::indexed(11));
    CHECK(d->canvas().at(3, 0).fg == Color::indexed(3));
    CHECK(d->canvas().at(0, 1).fg == Color::indexed(11));
    CHECK(d->palette() == kPalette);  // a remap keeps the palette
    d->undoStack()->undo();
    CHECK(d->canvas() == before);
    d->undoStack()->redo();
    d->undoStack()->undo();

    // the whole document
    CHECK(d->remapColours(swapColourMap(3, 11), Document::RemapScope::Document) == 5);
    CHECK(d->undoStack()->count() == steps + 1);
    for (int x = 0; x < 4; ++x)
        CHECK(d->canvas().at(x, 0).fg == Color::indexed(11));
    CHECK(d->canvas().at(0, 1).fg == Color::indexed(3));
    d->undoStack()->undo();
    CHECK(d->canvas() == before);

    // nothing to move: no step, and why
    CHECK(d->remapColours(swapColourMap(5, 6), Document::RemapScope::Layer, {}, &error) == 0);
    CHECK(error == QStringLiteral("no cell holds a colour the map moves"));
    CHECK(d->remapColours(swapColourMap(3, 11), Document::RemapScope::Selection, {}, &error) == 0);
    CHECK(error == QStringLiteral("nothing is selected"));
    CHECK(d->undoStack()->index() == steps);
}

TEST_CASE("Remap Colours of every layer maps each cells layer in one step", "[app][palette][remap]") {
    MainWindow w;
    Document* d = w.document();
    put(d, 0, 0, cell(3, 0));
    const LayerId bottom = d->activeLayer();
    const LayerId top = d->addLayer(LayerKind::Cells);
    put(d, 1, 0, cell(3, 0));
    const int steps = d->undoStack()->count();
    int skipped = -1;
    CHECK(d->remapColours(swapColourMap(3, 4), Document::RemapScope::Document, {}, nullptr, &skipped) == 2);
    CHECK(skipped == 0);
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->layers().find(bottom)->cells.at(0, 0).fg == Color::indexed(4));
    CHECK(d->layers().find(top)->cells.at(1, 0).fg == Color::indexed(4));
    CHECK(d->canvas().at(1, 0).fg == Color::indexed(4));
    d->undoStack()->undo();
    CHECK(d->layers().find(bottom)->cells.at(0, 0).fg == Color::indexed(3));
    CHECK(d->layers().find(top)->cells.at(1, 0).fg == Color::indexed(3));
    // the active layer only
    CHECK(d->remapColours(swapColourMap(3, 4), Document::RemapScope::Layer) == 1);
    CHECK(d->layers().find(bottom)->cells.at(0, 0).fg == Color::indexed(3));
    CHECK(d->layers().find(top)->cells.at(1, 0).fg == Color::indexed(4));
}

TEST_CASE("the Remap Colours dialog applies its map to the scope it names", "[app][palette][remap]") {
    MainWindow w;
    Document* d = w.document();
    put(d, 0, 0, cell(2, 9));
    const int steps = d->undoStack()->count();
    runDialog<RemapDialog>(action(w, QStringLiteral("remapColours")), [](RemapDialog* dlg) {
        CHECK(dlg->scope() == Document::RemapScope::Layer);  // nothing selected
        CHECK_FALSE(dlg->findChild<QRadioButton*>(QStringLiteral("remapSelection"))->isEnabled());
        dlg->target(2)->setCurrentIndex(9);
        dlg->target(9)->setCurrentIndex(2);
        CHECK(dlg->mapText()->text() == QStringLiteral("2=9,9=2"));
        CHECK(dlg->map() == swapColourMap(2, 9));
    });
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->canvas().at(0, 0).fg == Color::indexed(9));
    CHECK(d->canvas().at(0, 0).bg == Color::indexed(2));

    // the map typed as text sets the lists
    RemapDialog dlg(kPalette, true);
    CHECK(dlg.scope() == Document::RemapScope::Selection);
    dlg.mapText()->setText(QStringLiteral("0=15,15=0"));
    emit dlg.mapText()->textEdited(dlg.mapText()->text());
    CHECK(dlg.map() == swapColourMap(0, 15));
    dlg.mapText()->setText(QStringLiteral("0=16"));
    emit dlg.mapText()->textEdited(dlg.mapText()->text());
    CHECK_FALSE(dlg.buttons()->button(QDialogButtonBox::Ok)->isEnabled());
    CHECK(dlg.map() == swapColourMap(0, 15));
}

TEST_CASE("Fit to Palette keeps the look as one undo step, and a BIN save then loses no palette",
          "[app][palette][fit]") {
    MainWindow w;
    Document* d = w.document();
    QString error;
    REQUIRE(w.openFile(fixture(), &error));
    REQUIRE(d->encode(ArtFormat::Bin).paletteDropped);
    const QImage was = look(d);
    const Canvas cells = d->canvas();
    const Palette own = d->palette();
    action(w, QStringLiteral("fitPalette_cga"))->trigger();
    CHECK_FALSE(d->ansi().palette);
    CHECK(w.renderer().palette() == kPalette);
    CHECK(d->undoStack()->count() == 1);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Fit to Palette"));
    // every cell shows the standard colour nearest the one it showed
    const ColourMap m = fitColourMap(own, kPalette);
    for (int y = 0; y < cells.height(); ++y)
        for (int x = 0; x < cells.width(); ++x)
            CHECK(d->canvas().at(x, y).fg == remapCell(cells.at(x, y), m).fg);
    CHECK_FALSE(d->encode(ArtFormat::Bin).paletteDropped);
    d->undoStack()->undo();
    CHECK(d->palette() == own);
    CHECK(d->canvas() == cells);
    CHECK(look(d) == was);

    // a palette that holds every colour of the old one: the look is kept exactly
    Palette rev{};
    for (std::size_t i = 0; i < 16; ++i)
        rev[i] = own[15 - i];
    CHECK(d->fitToPalette(rev));
    CHECK(d->palette() == rev);
    CHECK(look(d) == was);
    // and again: nothing to do
    CHECK_FALSE(d->fitToPalette(rev));
}

namespace {

// A 6x3 project: a black background layer, and above it a cells layer
// with an 'A' (white on black) at 2,1 in `style`.
LayerId styledPiece(Document* d, const TextStyle& style) {
    project::Project p;
    p.layers = LayerStack(6, 3);
    Layer ground;
    ground.props.name = "ground";
    ground.cells = Canvas(6, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x)
            ground.cells.at(x, y) = cell(7, 0, U' ');
    p.layers.add(std::move(ground));
    Layer l;
    l.props.name = "art";
    l.cells = Canvas(6, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x)
            l.cells.at(x, y).transparent = true;
    l.cells.at(2, 1) = cell(15, 0);
    l.props.style = style;
    const LayerId id = p.layers.add(std::move(l));
    d->replace(p, QStringLiteral("s.bwp"), FileForm::ProjectZip);
    return id;
}

// The standard palette backwards: it holds every standard colour.
Palette reversed() {
    Palette rev{};
    for (std::size_t i = 0; i < 16; ++i)
        rev[i] = kPalette[15 - i];
    return rev;
}

} // namespace

TEST_CASE("Fit to Palette maps the colours a layer's style names with its cells, one undo step",
          "[app][palette][fit]") {
    MainWindow w;
    Document* d = w.document();
    const LayerId id = styledPiece(d, TextStyle{{Effect{true, PlateEffect{Blue, 1}}}});
    const QImage was = look(d);
    int lost = -1, skipped = -1;
    REQUIRE(d->fitToPalette(reversed(), &lost, &skipped));
    CHECK(look(d) == was);
    CHECK(lost == 0);
    CHECK(skipped == 0);
    CHECK(std::get<PlateEffect>(d->layers().find(id)->props.style->effects[0].settings).colour == 15 - Blue);
    CHECK(d->undoStack()->undoText() == QStringLiteral("Fit to Palette"));
    d->undoStack()->undo();
    CHECK_FALSE(d->ansi().palette);
    CHECK(std::get<PlateEffect>(d->layers().find(id)->props.style->effects[0].settings).colour == Blue);
    CHECK(look(d) == was);
    d->undoStack()->redo();
    CHECK(look(d) == was);
}

TEST_CASE("Fit to Palette counts the cells it can't keep: a style's ramps, a live text layer", "[app][palette][fit]") {
    MainWindow w;
    Document* d = w.document();
    // a glow draws from a named ramp of indices, which the fit leaves: under
    // the reversed palette its cells show other colours, the 'A' doesn't
    styledPiece(d, TextStyle{{Effect{true, GlowEffect{"fire", 1, 3}}}});
    const Canvas before = d->canvas();
    int glow = 0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x)
            glow += before.at(x, y) != cell(7, 0, U' ') && !(x == 2 && y == 1);
    REQUIRE(glow > 0);
    int lost = -1, skipped = -1;
    REQUIRE(d->fitToPalette(reversed(), &lost, &skipped));
    CHECK(lost == glow);
    CHECK(skipped == 0);
    CHECK(renderCanvas(d->canvas(), 8, 1, vgaFont(), d->palette()).copy(16, 16, 8, 16) ==
          renderCanvas(before, 8, 1, vgaFont(), kPalette).copy(16, 16, 8, 16));

    // a live text layer draws its own cells: left, and its letters counted
    MainWindow w2;
    Document* d2 = w2.document();
    TextSource text;
    text.text = U"C";
    text.face.name = "mini";
    REQUIRE(d2->addTextLayer(text, 0, 0));
    int ink = 0;
    for (int y = 0; y < d2->canvas().height(); ++y)
        for (int x = 0; x < d2->canvas().width(); ++x)
            ink += d2->canvas().at(x, y).ch != U' ';
    REQUIRE(ink > 0);
    REQUIRE(d2->fitToPalette(reversed(), &lost, &skipped));
    CHECK(skipped == 1);
    CHECK(lost >= ink);

    // the status bar says so rather than that every cell looks as it did
    MainWindow w3;
    Document* d3 = w3.document();
    REQUIRE(d3->addTextLayer(text, 0, 0));
    REQUIRE(d3->setPalette(reversed(), QStringLiteral("Palette")));
    action(w3, QStringLiteral("fitPalette_cga"))->trigger();
    const QString msg = w3.statusBar()->currentMessage();
    INFO(msg.toStdString());
    CHECK(msg.contains(QStringLiteral("show another colour")));
    CHECK(msg.contains(QStringLiteral("1 layer left as they are")));
}

TEST_CASE("a project keeps the palette; an .ans save drops it from the document", "[app][palette][project]") {
    QTemporaryDir tmp;
    const QString bwp = tmp.filePath(QStringLiteral("p.bwp"));
    QString error;
    Palette xterm = palettePreset("xterm")->colours;
    {
        MainWindow w;
        Document* d = w.document();
        put(d, 0, 0, cell(4, 1));
        REQUIRE(d->setPalette(xterm, QStringLiteral("Palette")));
        REQUIRE(d->saveProject(bwp, FileForm::ProjectZip, &error));
        CHECK(d->palette() == xterm);  // a project keeps it
    }
    MainWindow w;
    Document* d = w.document();
    REQUIRE(w.openFile(bwp, &error));
    CHECK(d->palette() == xterm);
    CHECK(w.renderer().palette() == xterm);
    // a flat document saved as .ans no longer has it
    d->clear();
    put(d, 0, 0, cell(4, 1));
    REQUIRE(d->setPalette(xterm, QStringLiteral("Palette")));
    const QString ans = tmp.filePath(QStringLiteral("p.ans"));
    const AnsiOutput out = d->encode(ArtFormat::Ansi);
    CHECK(out.paletteDropped);
    REQUIRE(d->save(ans, out, &error));
    CHECK_FALSE(d->ansi().palette);
    CHECK(w.renderer().palette() == kPalette);
}

TEST_CASE("an XBIN's own font draws on the canvas until a save drops it", "[app][font]") {
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(fixture(), &error));
    const AnsiDocument xb = parseXbin(bwtest::readFile(fixture().toStdString()));
    REQUIRE(xb.font);
    CHECK(w.renderer().cellSize() == QSize(8, xb.font->height));
    auto& r = const_cast<CanvasRenderer&>(w.renderer());
    r.flush();
    const auto* atlas = dynamic_cast<const AtlasRenderer*>(&w.renderer());
    REQUIRE(atlas);
    CHECK(atlas->cache() == renderDocument(xb));
    CHECK(w.canvasView()->sizeHint().height() >= w.document()->canvas().height() * xb.font->height);
    // a new document is in the VGA font again
    w.document()->clear();
    CHECK(w.renderer().cellSize() == QSize(8, 16));
    // reopened, then saved as .ans: the font is lost, and the canvas draws VGA
    REQUIRE(w.openFile(fixture(), &error));
    QTemporaryDir tmp;
    REQUIRE(w.saveFile(tmp.filePath(QStringLiteral("t.ans")), &error, true));
    CHECK_FALSE(w.document()->ansi().font);
    CHECK(w.renderer().cellSize() == QSize(8, 16));
}

TEST_CASE("the colour and ramp lists draw in the document's palette", "[app][palette]") {
    MainWindow w;
    const auto first = [](const QIcon& icon) { return icon.pixmap(QSize(64, 16)).toImage().pixel(1, 1) | 0xff000000u; };
    auto* ramp = w.findChild<QComboBox*>(QStringLiteral("brushRamp"));
    auto* text = w.findChild<QComboBox*>(QStringLiteral("textColour"));
    REQUIRE(ramp);
    REQUIRE(text);
    const Ramp r0 = *findRamp(ramp->itemData(0).toString().toStdString());
    const auto rgb = [](Rgb c) { return qRgb(c.r, c.g, c.b) | 0xff000000u; };
    CHECK(first(ramp->itemIcon(0)) == rgb(kPalette[r0.colours[0]]));
    CHECK(first(text->itemIcon(4)) == rgb(kPalette[4]));
    action(w, QStringLiteral("palette_xterm"))->trigger();
    const Palette& x = palettePreset("xterm")->colours;
    CHECK(shownPalette() == x);
    CHECK(first(ramp->itemIcon(0)) == rgb(x[r0.colours[0]]));
    CHECK(first(text->itemIcon(4)) == rgb(x[4]));
    CHECK(first(swatchIcon(9)) == rgb(x[9]));
    w.document()->undoStack()->undo();
    CHECK(shownPalette() == kPalette);
    CHECK(first(text->itemIcon(4)) == rgb(kPalette[4]));
}
