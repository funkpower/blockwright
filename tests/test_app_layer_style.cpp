// Layer styles in the app (PLAN §15): the Layer Style dialog's rows and the
// style they give, its preview, IRC bytes, lint and confusables, OK as one
// undo step and Cancel as none, Save as Preset, Bake of text, cells and
// pixels layers, and the Layers dock's Style column.

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "layer_style_dialog.h"
#include "layers_dock.h"
#include "main_window.h"
#include "select_tool.h"

#include "blockwright/composite.h"
#include "blockwright/face.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/style_draw.h"
#include "blockwright/target.h"
#include "blockwright/text.h"
#include "blockwright/text_style.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>

using namespace bw;

namespace {

constexpr int kW = 20, kH = 8;

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

const Cell kBlock{kFullBlock, Color::indexed(Grey), Color::indexed(Black), 0, false};

// A cells or pixels layer the canvas's size, transparent but for one cell
// (a grey block, or a red top pixel) at (5, 3).
Layer artLayer(LayerKind kind) {
    Layer l;
    l.props.kind = kind;
    l.props.name = kind == LayerKind::Pixels ? "pixels" : "cells";
    l.cells = Canvas(kW, kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            l.cells.at(x, y) = transparentCell();
    l.cells.at(5, 3) = kind == LayerKind::Pixels ? pixelLayerCell(LayerPixels{Red, std::nullopt}) : kBlock;
    return l;
}

// Opens a project of `layer` in the window, iCE `ice`; the layer's id.
LayerId open(MainWindow& w, Layer layer, bool ice = true) {
    project::Project p;
    p.ice = ice;
    p.layers = LayerStack(kW, kH);
    const LayerId id = p.layers.add(std::move(layer));
    w.document()->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
    w.document()->setActiveLayer(id);
    REQUIRE(w.document()->activeLayer() == id);
    return id;
}

const Layer& layerOf(Document* d, LayerId id) {
    const Layer* l = d->layers().find(id);
    REQUIRE(l);
    return *l;
}

TextStyle outlined() { return TextStyle{{Effect{true, OutlineEffect{OutlineForm::Cells, Blue}}}}; }

// The dialog the window has open.
LayerStyleDialog* openDialog(MainWindow& w) {
    auto* d = w.findChild<LayerStyleDialog*>();
    REQUIRE(d);
    return d;
}

void closeDialogs() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

} // namespace

TEST_CASE("the dialog's rows are one effect of each kind, the style's own or a default switched off", "[style]") {
    const std::vector<Effect> none = styleRows(std::nullopt);
    REQUIRE(none.size() == TextStyle::kMaxEffects);
    for (std::size_t k = 0; k < none.size(); ++k) {
        CHECK(none[k].kind() == EffectKind(k));
        CHECK_FALSE(none[k].enabled);
    }
    CHECK(styleFromRows(std::nullopt, none) == std::nullopt);

    // switching one on gives a style of it
    std::vector<Effect> rows = none;
    rows[std::size_t(EffectKind::Outline)].enabled = true;
    REQUIRE(styleFromRows(std::nullopt, rows));
    CHECK(*styleFromRows(std::nullopt, rows) == TextStyle{{Effect{true, OutlineEffect{}}}});

    // a row with settings changed but switched off is kept, off
    rows = none;
    rows[std::size_t(EffectKind::Plate)].settings = PlateEffect{Red, 4};
    CHECK(*styleFromRows(std::nullopt, rows) == TextStyle{{Effect{false, PlateEffect{Red, 4}}}});

    // the style's effects keep their order (and a switched-off one stays);
    // a new one goes after them
    const TextStyle original{{Effect{true, GlowEffect{"fire", 2, 2}}, Effect{false, PlateEffect{Red, 4}}}};
    rows = styleRows(original);
    CHECK(rows[std::size_t(EffectKind::Glow)] == original.effects[0]);
    CHECK(rows[std::size_t(EffectKind::Plate)] == original.effects[1]);
    CHECK(styleFromRows(original, rows) == original);
    rows[std::size_t(EffectKind::Plate)].enabled = true;
    rows[std::size_t(EffectKind::Shadow)].enabled = true;
    CHECK(*styleFromRows(original, rows) == TextStyle{{Effect{true, GlowEffect{"fire", 2, 2}},
                                                        Effect{true, PlateEffect{Red, 4}},
                                                        Effect{true, ShadowEffect{}}}});
    // an empty style stays a style
    CHECK(styleFromRows(TextStyle{}, styleRows(TextStyle{})) == TextStyle{});
}

TEST_CASE("Layer Style OK is one undo step on a cells layer, Cancel and an unchanged OK none", "[style]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = open(w, artLayer(LayerKind::Cells));
    QAction* style = act(w, QStringLiteral("layerStyle"));
    REQUIRE(style->isEnabled());
    const Canvas before = doc->canvas();
    const int steps = doc->undoStack()->count();

    SECTION("Cancel") {
        style->trigger();
        LayerStyleDialog* d = openDialog(w);
        CHECK(d->isVisible());
        d->setRow(Effect{true, OutlineEffect{OutlineForm::Cells, Blue}});
        // the document isn't touched while the dialog is open
        CHECK(doc->canvas() == before);
        d->buttons()->button(QDialogButtonBox::Cancel)->click();
        closeDialogs();
        CHECK_FALSE(w.findChild<LayerStyleDialog*>());
        CHECK(doc->undoStack()->count() == steps);
        CHECK_FALSE(layerOf(doc, id).props.style);
        CHECK(doc->canvas() == before);
    }
    SECTION("OK without a change") {
        style->trigger();
        openDialog(w)->buttons()->button(QDialogButtonBox::Ok)->click();
        closeDialogs();
        CHECK(doc->undoStack()->count() == steps);
    }
    SECTION("OK") {
        style->trigger();
        LayerStyleDialog* d = openDialog(w);
        // through the widgets: Outline switched on, its form set to cells
        QListWidget* list = d->effectList();
        list->setCurrentRow(int(EffectKind::Outline));
        CHECK(d->pages()->currentIndex() == int(EffectKind::Outline));
        list->item(int(EffectKind::Outline))->setCheckState(Qt::Checked);
        auto* form = d->findChild<QComboBox*>(QStringLiteral("outlineForm"));
        REQUIRE(form);
        form->setCurrentIndex(form->findData(int(OutlineForm::Cells)));
        CHECK(d->style() == outlined());
        d->buttons()->button(QDialogButtonBox::Ok)->click();
        closeDialogs();
        CHECK(doc->undoStack()->count() == steps + 1);
        CHECK(doc->undoStack()->undoText() == QStringLiteral("Layer Style"));
        CHECK(layerOf(doc, id).props.style == outlined());
        CHECK(doc->canvas().at(4, 3).bg.paletteIndex() == Blue);

        doc->undoStack()->undo();
        CHECK_FALSE(layerOf(doc, id).props.style);
        CHECK(doc->canvas() == before);
        doc->undoStack()->redo();
        CHECK(layerOf(doc, id).props.style == outlined());

        // the dialog opens on the style the layer has
        style->trigger();
        d = openDialog(w);
        CHECK(d->rows()[std::size_t(EffectKind::Outline)] == outlined().effects[0]);
        CHECK(d->effectList()->item(int(EffectKind::Outline))->checkState() == Qt::Checked);
        CHECK(d->effectList()->currentRow() == int(EffectKind::Outline));
        d->reject();
        closeDialogs();
    }
}

TEST_CASE("changing an effect's settings switches it on, and the widgets follow setRow", "[style]") {
    MainWindow w;
    open(w, artLayer(LayerKind::Cells));
    LayerStyleDialog d(w.document(), w.document()->activeLayer());
    auto* padding = d.findChild<QSpinBox*>(QStringLiteral("platePadding"));
    REQUIRE(padding);
    padding->setValue(3);
    CHECK(d.rows()[std::size_t(EffectKind::Plate)] == Effect{true, PlateEffect{Blue, 3}});
    CHECK(d.effectList()->item(int(EffectKind::Plate))->checkState() == Qt::Checked);

    d.setRow(Effect{false, ShadowEffect{ShadowForm::Extrude, Compass::SW, 2, Red}});
    CHECK(d.findChild<QComboBox*>(QStringLiteral("shadowForm"))->currentData().toInt() == int(ShadowForm::Extrude));
    CHECK(d.findChild<QComboBox*>(QStringLiteral("shadowDirection"))->currentData().toInt() == int(Compass::SW));
    CHECK(d.findChild<QSpinBox*>(QStringLiteral("shadowDistance"))->value() == 2);
    CHECK(d.findChild<QComboBox*>(QStringLiteral("shadowColour"))->currentData().toInt() == Red);
    CHECK(d.effectList()->item(int(EffectKind::Shadow))->checkState() == Qt::Unchecked);

    // a flat fill reads its colour, the others their ramp
    auto* mode = d.findChild<QComboBox*>(QStringLiteral("fillMode"));
    auto* colour = d.findChild<QComboBox*>(QStringLiteral("fillColour"));
    auto* ramp = d.findChild<QComboBox*>(QStringLiteral("fillRamp"));
    REQUIRE(mode);
    CHECK(colour->isEnabled());
    CHECK_FALSE(ramp->isEnabled());
    mode->setCurrentIndex(mode->findData(int(FillMode::Rows)));
    CHECK_FALSE(colour->isEnabled());
    CHECK(ramp->isEnabled());

    // a glow's brightness has its own box beside the width
    d.setRow(Effect{false, GlowEffect{"toxic", 2, 7}});
    auto* width = d.findChild<QSpinBox*>(QStringLiteral("glowWidth"));
    auto* brightness = d.findChild<QSpinBox*>(QStringLiteral("glowBrightness"));
    REQUIRE(width);
    REQUIRE(brightness);
    CHECK(width->value() == 2);
    CHECK(brightness->value() == 7);
    brightness->setValue(5);
    CHECK(d.rows()[std::size_t(EffectKind::Glow)] == Effect{true, GlowEffect{"toxic", 2, 5}});
}

TEST_CASE("the dialog previews the layer with the style, its IRC bytes and its lint, before OK", "[style]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = open(w, artLayer(LayerKind::Cells));
    LayerStyleDialog d(doc, id);
    // no style: the layer as it is
    CHECK(d.previewCells().width() == kW);
    CHECK(d.previewCells().at(5, 3) == kBlock);
    CHECK(d.previewCells().at(4, 3) == Cell{});

    d.setRow(outlined().effects[0]);
    const Canvas& shown = d.previewCells();
    CHECK(shown.at(4, 3).bg.paletteIndex() == Blue);
    CHECK(shown.at(6, 3).bg.paletteIndex() == Blue);
    CHECK_FALSE(layerOf(doc, id).props.style);  // the layer is untouched
    // what the compositor will show for the layer
    Layer styled = layerOf(doc, id);
    styled.props.style = outlined();
    LayerStack alone(kW, kH);
    alone.add(styled);
    CHECK(shown == composite(alone, {doc->ansi().ice, Cell{}}));

    // the widest IRC line as the ascii script sends it, against the
    // project's budget
    std::size_t widest = 0;
    for (const std::string& line : irc::encodeCanvas(shown))
        widest = std::max(widest, irc::txtSentBytes(line, irc::kEfnetWeechatBudget));
    CHECK(d.widestLineBytes() == widest);
    CHECK(d.ircLabel()->text().contains(QString::number(widest)));
    CHECK(d.ircLabel()->text().contains(QString::number(irc::kEfnetWeechatBudget)));

    // the lint, as the Lint dock's default runs it
    lint::LintOptions o;
    o.ramps = namedRamps();
    o.craftPairs = true;
    o.ice = doc->ansi().ice;
    const lint::LintReport want = lint::lintCanvas(composite(alone, {doc->ansi().ice, Cell{}}), o);
    CHECK(d.lintReport().findings == want.findings);
    CHECK(d.lintText()->toPlainText().startsWith(QString::fromStdString(lint::summary(want))));
    CHECK_FALSE(d.previewLabel()->pixmap().isNull());
    // a cells layer has no face, so no confusables
    CHECK(d.confusables().isEmpty());
}

TEST_CASE("under IRC the dialog's lint marks ans-loss as a note, and the summary is clean", "[style]") {
    MainWindow w;
    Document* doc = w.document();
    // black-on-black blocks, so IRC has no blank rows to trim, and an X in
    // mIRC 77
    Layer l = artLayer(LayerKind::Cells);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            l.cells.at(x, y) = Cell{kFullBlock, Color::indexed(Black), Color::indexed(Black), 0, false};
    l.cells.at(5, 3) = Cell{U'X', swatchColor(77), Color::indexed(Black), 0, false};
    const LayerId id = open(w, std::move(l));
    REQUIRE(doc->setTarget(Target::Irc));
    LayerStyleDialog d(doc, id);
    REQUIRE(d.lintReport().findings.size() == 1);
    REQUIRE(d.lintReport().findings[0].kind == lint::Finding::Kind::AnsLoss);
    const QStringList lines = d.lintText()->toPlainText().split(QLatin1Char('\n'));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].startsWith(QStringLiteral("clean, 1 note (1 ans-loss)")));
    CHECK(lines[1] == QStringLiteral("5,3  note: ") + QString::fromStdString(d.lintReport().findings[0].message));
}

TEST_CASE("in IRC 16 colours the dialog's lint notes the folded cells, as the Lint dock does", "[style]") {
    MainWindow w;
    Document* doc = w.document();
    Layer l = artLayer(LayerKind::Cells);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            l.cells.at(x, y) = Cell{kFullBlock, Color::indexed(Black), Color::indexed(Black), 0, false};
    l.cells.at(5, 3) = Cell{U'X', swatchColor(40), Color::indexed(Black), 0, false};
    const LayerId id = open(w, std::move(l));
    REQUIRE(doc->setTarget(Target::Irc));
    const auto noted = [](const lint::LintReport& r) {
        return std::any_of(r.findings.begin(), r.findings.end(),
                           [](const lint::Finding& f) { return f.kind == lint::Finding::Kind::IrcFold; });
    };
    {
        LayerStyleDialog d(doc, id);
        CHECK_FALSE(noted(d.lintReport()));
    }
    REQUIRE(doc->setIrcColours(IrcColours::Mirc16));
    LayerStyleDialog d(doc, id);
    CHECK(noted(d.lintReport()));
}

TEST_CASE("under Micron the dialog's lint reads the document's own palette, as the Lint dock does", "[style]") {
    MainWindow w;
    Document* doc = w.document();
    // an XBIN's palette whose entry 9 isn't twelve-bit (kPalette's is), and
    // an A in it at (5, 3)
    std::array<Rgb, 16> pal = kPalette;
    pal[9] = Rgb{0x28, 0x28, 0xcb};
    AnsiDocument flat;
    flat.canvas = Canvas(kW, kH);
    flat.palette = pal;
    flat.canvas.at(5, 3) = Cell{U'A', Color::indexed(9), Color::indexed(Black), 0, false};
    doc->replace(flat, QString());
    REQUIRE(doc->setTarget(Target::Micron));
    REQUIRE(doc->ansi().palette == pal);
    LayerStyleDialog d(doc, doc->activeLayer());
    const auto& f = d.lintReport().findings;
    const auto note = std::find_if(f.begin(), f.end(),
                                   [](const lint::Finding& x) { return x.kind == lint::Finding::Kind::MicronColour; });
    REQUIRE(note != f.end());
    CHECK(note->x == 5);
    CHECK(note->y == 3);
    CHECK(note->message.starts_with("1 cell is in a colour three hex digits can't write"));
}

TEST_CASE("the preview and its IRC bytes show the cells as the canvas does, with the project's iCE", "[style]") {
    // The colour in the middle of cell (5, 3) in the 1x preview.
    const auto shownBg = [](LayerStyleDialog& d) {
        const QImage img = d.previewLabel()->pixmap().toImage();
        const QRgb px = img.pixel(5 * 8 + 4, 3 * img.height() / kH + 4);
        return Rgb{std::uint8_t(qRed(px)), std::uint8_t(qGreen(px)), std::uint8_t(qBlue(px))};
    };
    SECTION("iCE on: blink is a bright background") {
        MainWindow w;
        Layer l = artLayer(LayerKind::Cells);
        l.cells.at(5, 3) = Cell{U' ', Color::indexed(Grey), Color::indexed(Black), attr::Blink, false};
        const LayerId id = open(w, std::move(l), true);
        LayerStyleDialog d(w.document(), id);
        CHECK(d.previewCells().at(5, 3).bg.paletteIndex() == DarkGrey);
        CHECK(shownBg(d) == kPalette[DarkGrey]);
        CHECK(d.widestLineBytes() > 0);
        CHECK(d.widestLineBytes() == w.ircMeter()->sentBytes(3));
        CHECK(d.widestLineWrittenBytes() == w.ircMeter()->bytes(3));
        // the lint reads the cells as the Lint dock does, the canvas's own
        // with o.ice: a blinking space on Black is a black space to IRC, so
        // the canvas has no art and no blank edge rows
        lint::LintOptions o;
        o.ramps = namedRamps();
        o.craftPairs = true;
        o.ice = true;
        const lint::LintReport dock = lint::lintCanvas(w.document()->canvas(), o);
        CHECK(d.lintReport().findings == dock.findings);
        CHECK(std::ranges::none_of(d.lintReport().findings, [](const lint::Finding& f) {
            return f.kind == lint::Finding::Kind::BlankEdgeRows;
        }));
    }
    SECTION("iCE off: a bright background is blink on the dark colour") {
        MainWindow w;
        Layer l = artLayer(LayerKind::Cells);
        l.cells.at(5, 3) = Cell{U' ', Color::indexed(Grey), Color::indexed(DarkGrey), 0, false};
        const LayerId id = open(w, std::move(l), false);
        LayerStyleDialog d(w.document(), id);
        CHECK(d.previewCells().at(5, 3).bg.paletteIndex() == Black);
        CHECK(d.previewCells().at(5, 3).attrs & attr::Blink);
        CHECK(shownBg(d) == kPalette[Black]);
        CHECK(d.widestLineBytes() == w.ircMeter()->sentBytes(3));
        CHECK(d.widestLineWrittenBytes() == w.ircMeter()->bytes(3));
    }
}

TEST_CASE("the dialog measures the widest line as the ascii script sends it, as the meter does (#364)", "[style]") {
    // 80 ★ in bright red and red by turns on black: 402 bytes written, 483
    // as the ascii script sends it, which the meter and Export hold to 407
    MainWindow w;
    Layer l;
    l.props.kind = LayerKind::Cells;
    l.props.name = "stars";
    l.cells = Canvas(80, 1);
    for (int x = 0; x < 80; ++x)
        l.cells.at(x, 0) = Cell{U'★', Color::indexed(x % 2 ? Red : LightRed), Color::indexed(Black), 0, false};
    project::Project p;
    p.layers = LayerStack(80, 1);
    const LayerId id = p.layers.add(std::move(l));
    w.document()->replace(p, QStringLiteral("stars.bwp"), FileForm::ProjectZip);
    REQUIRE(w.document()->extras().ircBudget.bytes == irc::kEfnetWeechatBudget);
    REQUIRE(w.ircMeter()->bytes(0) == 402);
    REQUIRE(w.ircMeter()->sentBytes(0) == 483);
    LayerStyleDialog d(w.document(), id);
    CHECK(d.widestLineBytes() == 483);
    CHECK(d.widestLineWrittenBytes() == 402);
    const QString text = d.ircLabel()->text();
    CHECK(text.contains(QStringLiteral("483 of 407 bytes")));
    CHECK(text.contains(QStringLiteral("402 as written")));
    CHECK(text.contains(QStringLiteral("over by 76")));
}

TEST_CASE("a style the layer can't draw is named and OK is off", "[style]") {
    const Ramp* small = nullptr;
    for (const Ramp& r : namedRamps())
        if (r.topRung() < TextStyle::kMaxGlow && (!small || r.topRung() < small->topRung()))
            small = &r;
    REQUIRE(small);
    MainWindow w;
    const LayerId id = open(w, artLayer(LayerKind::Cells));
    LayerStyleDialog d(w.document(), id);
    CHECK(d.problem().isEmpty());
    d.setRow(Effect{true, GlowEffect{small->name, small->topRung() + 1, small->topRung() + 1}});
    CHECK_FALSE(d.problem().isEmpty());
    CHECK(d.problemLabel()->text() == d.problem());
    CHECK_FALSE(d.buttons()->button(QDialogButtonBox::Ok)->isEnabled());
    const int steps = w.document()->undoStack()->count();
    CHECK_FALSE(d.apply());
    CHECK(w.document()->undoStack()->count() == steps);
    d.setRow(Effect{true, GlowEffect{small->name, small->topRung(), small->topRung()}});
    CHECK(d.problem().isEmpty());
    CHECK(d.buttons()->button(QDialogButtonBox::Ok)->isEnabled());
}

TEST_CASE("Layer Style on a live text layer redraws it with the style, one step, and warns of confusables",
          "[style][text]") {
    FaceRef mini;
    mini.name = "mini";
    const Face* face = findFace(mini);
    REQUIRE(face);
    TextSource source;
    source.face = mini;
    source.text = U"SO50";
    REQUIRE_FALSE(confusablesIn(*face, source.text).empty());

    MainWindow w;
    Document* doc = w.document();
    project::Project p;
    p.layers = LayerStack(kW, kH);
    const LayerId id = p.layers.add(makeTextLayer(source));
    doc->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
    doc->setActiveLayer(id);
    REQUIRE(act(w, QStringLiteral("layerStyle"))->isEnabled());
    const Canvas cells = layerOf(doc, id).cells;

    LayerStyleDialog d(doc, id);
    QStringList pairs;
    for (const ConfusablePair& c : confusablesIn(*face, source.text))
        pairs << QString::fromStdU32String(std::u32string{c.a, c.b});
    CHECK(d.confusables() == pairs.join(QLatin1Char(' ')));
    CHECK(d.confusableLabel()->text().contains(d.confusables()));

    const TextStyle plate{{Effect{true, PlateEffect{Blue, 1}}}};
    d.setRow(plate.effects[0]);
    const Canvas drawn = renderStyledText(source, plate, *face, doc->ansi().ice).cells;
    CHECK(d.previewCells().width() == drawn.width());
    CHECK(d.previewCells().height() == drawn.height());

    const int steps = doc->undoStack()->count();
    REQUIRE(d.apply());
    CHECK(doc->undoStack()->count() == steps + 1);
    CHECK(layerOf(doc, id).props.style == plate);
    CHECK(layerOf(doc, id).cells == drawn);
    CHECK(d.previewCells() == layerOf(doc, id).cells);  // a plate leaves no transparent cell
    doc->undoStack()->undo();
    CHECK_FALSE(layerOf(doc, id).props.style);
    CHECK(layerOf(doc, id).cells == cells);
}

TEST_CASE("the confusable warning screens the text as its case draws it", "[style][text]") {
    FaceRef mini;
    mini.name = "mini";
    const Face* face = findFace(mini);
    REQUIRE(face);
    TextSource source;
    source.face = mini;
    source.text = U"s5";
    source.textCase = TextCase::Capitals;
    REQUIRE_FALSE(confusablesIn(*face, U"S5").empty());

    MainWindow w;
    Document* doc = w.document();
    project::Project p;
    p.layers = LayerStack(kW, kH);
    const LayerId id = p.layers.add(makeTextLayer(source));
    doc->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
    doc->setActiveLayer(id);

    LayerStyleDialog d(doc, id);
    CHECK(d.confusables().contains(QStringLiteral("S5")));
}

TEST_CASE("a tall layer's 1x preview scrolls inside a dialog that fits the screen", "[style]") {
    constexpr int w80 = 80, h200 = 200;
    Layer l;
    l.props.kind = LayerKind::Cells;
    l.props.name = "tall";
    l.cells = Canvas(w80, h200);
    for (int y = 0; y < h200; ++y)
        for (int x = 0; x < w80; ++x)
            l.cells.at(x, y) = transparentCell();
    l.cells.at(2, 0) = kBlock;            // a mark at the top
    l.cells.at(77, h200 - 1) = kBlock;    // and one at the bottom

    MainWindow w;
    project::Project p;
    p.layers = LayerStack(w80, h200);
    const LayerId id = p.layers.add(std::move(l));
    w.document()->replace(p, QStringLiteral("tall.bwp"), FileForm::ProjectZip);

    LayerStyleDialog d(w.document(), id);
    const QSize image = d.previewLabel()->pixmap().size();
    CHECK(image == QSize(w80 * 8, h200 * 16));  // still 1x
    // the dialog asks for no more than a 1080p screen holds
    CHECK(d.sizeHint().width() <= 1920);
    CHECK(d.sizeHint().height() <= 1000);

    d.resize(d.sizeHint());
    d.show();
    QCoreApplication::processEvents();
    QScrollArea* area = d.previewArea();
    REQUIRE(area);
    CHECK(area->widget() == d.previewLabel());
    QScrollBar* bar = area->verticalScrollBar();
    REQUIRE(bar->maximum() > 0);
    // both ends are reachable: the scroll range covers the whole image
    bar->setValue(bar->minimum());
    CHECK(d.previewLabel()->mapTo(area->viewport(), QPoint(0, 0)).y() == 0);
    bar->setValue(bar->maximum());
    const int bottom = d.previewLabel()->mapTo(area->viewport(), QPoint(0, d.previewLabel()->height())).y();
    CHECK(bottom == area->viewport()->height());
    CHECK(d.previewLabel()->height() >= image.height());
    d.hide();
}

TEST_CASE("Save as Preset writes the style to the user library as a new .style.json", "[style][library]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::filesystem::path lib = std::filesystem::path(dir.path().toStdString()) / "user";
    MainWindow w;
    LayerStyleDialog d(w.document(), open(w, artLayer(LayerKind::Cells)));
    QString error;
    // no folder, or no style: nothing to save
    CHECK_FALSE(d.presetButton()->isEnabled());
    CHECK(d.savePreset(QStringLiteral("Neon"), QStringLiteral("neon"), {}, &error).isEmpty());
    d.setPresetDir(lib, {QStringLiteral("neon")});
    CHECK_FALSE(d.presetButton()->isEnabled());
    d.setRow(outlined().effects[0]);
    CHECK(d.presetButton()->isEnabled());

    QStringList saved;
    QObject::connect(&d, &LayerStyleDialog::presetSaved, [&](const QString& p) { saved << p; });
    const QString first = d.savePreset(QStringLiteral("Neon"), QStringLiteral("neon"), {QStringLiteral("blue")}, &error);
    INFO(error.toStdString());
    REQUIRE_FALSE(first.isEmpty());
    CHECK(std::filesystem::path(first.toStdString()) == lib / "neon.style.json");
    const library::TextStyleEntry e = library::readTextStyleEntry(first.toStdString());
    CHECK(e.name == "Neon");
    CHECK(e.category == "neon");
    CHECK(e.tags == std::vector<std::string>{"blue"});
    CHECK(e.style == outlined());
    CHECK_FALSE(e.iceRequired);  // an outline needs no iCE
    // the library finds it
    const library::ScanResult found = library::scan(lib);
    REQUIRE(found.styles.size() == 1);
    CHECK(found.styles[0].style == outlined());

    // the same name again is a second file, never over the first
    const QString second = d.savePreset(QStringLiteral("Neon"), QStringLiteral("neon"), {}, &error);
    CHECK(std::filesystem::path(second.toStdString()) == lib / "neon-2.style.json");
    CHECK(library::readTextStyleEntry(first.toStdString()).tags == std::vector<std::string>{"blue"});
    CHECK(saved == QStringList{first, second});

    // a stack whose look needs iCE is saved marked so (#310): a rows
    // gradient on fire reaches its LightRed–Yellow rungs; one on steel never
    // needs a bright background
    d.setRow(Effect{true, FillEffect{FillMode::Rows, Grey, "fire"}});
    REQUIRE(d.style());
    REQUIRE(styleNeedsIce(*d.style()));
    const QString hot = d.savePreset(QStringLiteral("Hot"), QStringLiteral("neon"), {}, &error);
    REQUIRE_FALSE(hot.isEmpty());
    CHECK(library::readTextStyleEntry(hot.toStdString()).iceRequired);
    d.setRow(Effect{true, FillEffect{FillMode::Rows, Grey, "steel"}});
    const QString cold = d.savePreset(QStringLiteral("Cold"), QStringLiteral("neon"), {}, &error);
    REQUIRE_FALSE(cold.isEmpty());
    CHECK_FALSE(library::readTextStyleEntry(cold.toStdString()).iceRequired);

    // a tag the library refuses is an error, nothing written
    CHECK(d.savePreset(QStringLiteral("Bad"), QStringLiteral("neon"), {QStringLiteral("a\tb")}, &error).isEmpty());
    CHECK_FALSE(error.isEmpty());
    CHECK_FALSE(std::filesystem::exists(lib / "bad.style.json"));
}

TEST_CASE("Bake Layer bakes a styled cells or pixels layer's styled cells, one step undone whole", "[style]") {
    for (const LayerKind kind : {LayerKind::Cells, LayerKind::Pixels}) {
        for (const bool ice : {true, false}) {
            INFO((kind == LayerKind::Pixels ? "pixels" : "cells") << (ice ? " iCE" : " no iCE"));
            MainWindow w;
            Document* doc = w.document();
            const LayerId id = open(w, artLayer(kind), ice);
            QAction* bake = act(w, QStringLiteral("bakeLayer"));
            CHECK_FALSE(bake->isEnabled());  // nothing to bake without a style
            CHECK_FALSE(doc->bakeLayer(id));

            // a glow differs with iCE on and off: the document's setting is used
            LayerProperties p = layerOf(doc, id).props;
            p.style = TextStyle{{Effect{true, GlowEffect{"fire", 12, 12}}}};
            REQUIRE(doc->setLayerProperties(id, p, QStringLiteral("Style")));
            REQUIRE(bake->isEnabled());
            const Layer styled = layerOf(doc, id);
            const Canvas canvas = doc->canvas();
            const Canvas want = styledCells(styled, ice);
            REQUIRE_FALSE(want == styledCells(styled, !ice));
            const int steps = doc->undoStack()->count();

            bake->trigger();
            CHECK(doc->undoStack()->count() == steps + 1);
            CHECK(doc->undoStack()->undoText() == QStringLiteral("Bake Layer Style"));
            const Layer& baked = layerOf(doc, id);
            CHECK(baked.props.kind == LayerKind::Cells);
            CHECK_FALSE(baked.props.style);
            CHECK(baked.cells == want);
            CHECK(doc->canvas() == canvas);  // it shows as it did
            CHECK_FALSE(bake->isEnabled());

            doc->undoStack()->undo();
            CHECK(layerOf(doc, id) == styled);
            CHECK(doc->canvas() == canvas);
            doc->undoStack()->redo();
            CHECK(layerOf(doc, id).cells == want);
        }
    }
}

TEST_CASE("the Layers dock shows a layer's style and a double-click there opens the dialog", "[style][layers]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = open(w, artLayer(LayerKind::Cells));
    auto* dock = w.findChild<LayersDock*>();
    REQUIRE(dock);
    QTreeWidget* tree = dock->tree();
    REQUIRE(tree->columnCount() == 4);
    QTreeWidgetItem* row = tree->topLevelItem(0);
    REQUIRE(LayersDock::layerOf(row) == id);
    CHECK(row->text(LayersDock::StyleColumn).isEmpty());
    CHECK(row->toolTip(LayersDock::StyleColumn).contains(QStringLiteral("double-click")));

    LayerProperties p = layerOf(doc, id).props;
    p.style = TextStyle{{Effect{true, OutlineEffect{}}, Effect{false, GlowEffect{}}}};
    REQUIRE(doc->setLayerProperties(id, p, QStringLiteral("Style")));
    row = tree->topLevelItem(0);
    CHECK(row->text(LayersDock::StyleColumn) == QStringLiteral("fx"));
    CHECK(row->toolTip(LayersDock::StyleColumn).contains(QStringLiteral("outline")));
    CHECK_FALSE(row->toolTip(LayersDock::StyleColumn).contains(QStringLiteral("glow")));

    // a double-click on the Style cell: the dialog, not an editor
    w.show();
    tree->scrollToItem(row);
    const QPoint at = tree->visualItemRect(row).topLeft() +
                      QPoint(tree->columnViewportPosition(LayersDock::StyleColumn) + 4, 4);
    QList<LayerId> asked;
    QObject::connect(dock, &LayersDock::styleRequested, [&](LayerId l) { asked << l; });
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick,
                                    QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, QPointF(at), tree->viewport()->mapToGlobal(QPointF(at)), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton),
                      Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &e);
    }
    CHECK(asked == QList<LayerId>{id});
    CHECK(tree->findChildren<QLineEdit*>().isEmpty());  // no editor opened
    LayerStyleDialog* d = openDialog(w);
    CHECK(d->layer() == id);
    d->reject();
    closeDialogs();

    // Bake takes the style away and the cell empties
    REQUIRE(doc->bakeLayer(id));
    CHECK(tree->topLevelItem(0)->text(LayersDock::StyleColumn).isEmpty());
}

TEST_CASE("the undo of Bake Layer Style under a lift drops the float, as the layer's cells changed under it (#433)",
          "[style]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = open(w, artLayer(LayerKind::Cells));
    LayerProperties p = layerOf(doc, id).props;
    p.style = TextStyle{{Effect{true, GlowEffect{"fire", 12, 12}}}};
    REQUIRE(doc->setLayerProperties(id, p, QStringLiteral("Style")));
    const Canvas raw = layerOf(doc, id).cells;
    act(w, QStringLiteral("bakeLayer"))->trigger();
    REQUIRE_FALSE(layerOf(doc, id).props.style);
    const Canvas baked = layerOf(doc, id).cells;
    REQUIRE_FALSE(baked == raw);  // the glow is in the layer's cells now
    SelectTool* s = w.selectTool();
    w.tools()->setActive(s);
    const CellRect source{4, 2, 3, 3};  // the block and its baked glow
    s->select(source);
    const auto key = [&](int k, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
        QApplication::sendEvent(w.canvasView(), &press);
    };
    SECTION("undo") {
        key(Qt::Key_Right);  // lifts the block and its glow a cell right
        REQUIRE(s->floating());
        doc->undoStack()->undo();  // Bake Layer Style undone: the raw block and its style again
        REQUIRE(layerOf(doc, id).props.style);
        REQUIRE(layerOf(doc, id).cells == raw);
        // dropped, as for any undo that changes the cells under it: the
        // layer as the undo left it, the selection on the lift's source
        CHECK_FALSE(s->floating());
        CHECK(s->selection().bounds() == source);
        const int steps = doc->undoStack()->count();
        key(Qt::Key_Return, QStringLiteral("\r"));
        CHECK(layerOf(doc, id).cells == raw);
        CHECK(doc->undoStack()->count() == steps);
    }
    SECTION("redo") {
        doc->undoStack()->undo();
        REQUIRE(layerOf(doc, id).cells == raw);
        key(Qt::Key_Right);  // lifts the raw block a cell right
        REQUIRE(s->floating());
        doc->undoStack()->redo();  // baked again: the glow in the layer's cells
        REQUIRE(layerOf(doc, id).cells == baked);
        CHECK_FALSE(s->floating());
        CHECK(s->selection().bounds() == source);
        const int steps = doc->undoStack()->count();
        key(Qt::Key_Return, QStringLiteral("\r"));
        CHECK(layerOf(doc, id).cells == baked);
        CHECK(doc->undoStack()->count() == steps);
    }
}
