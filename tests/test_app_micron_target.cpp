// Format > Target > Micron: the 99 swatches and the colour dialog, the
// meter put away, and the canvas as a page shows it (PLAN §17).

#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "main_window.h"
#include "micron_colour_dialog.h"
#include "palette_widget.h"
#include "preferences.h"
#include "settings.h"
#include "tool.h"

#include "blockwright/micron.h"
#include "blockwright/target.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QUndoStack>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const char* name) {
    auto* a = w.findChild<QAction*>(QString::fromLatin1(name));
    REQUIRE(a);
    return a;
}

void click(QWidget* w, QPoint at, Qt::MouseButton button) {
    const QPointF global = w->mapToGlobal(QPointF(at));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), global, button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), global, button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

struct DefaultTarget {
    Target was = defaultTargetSetting();
    ~DefaultTarget() { setDefaultTargetSetting(was); }
};

// Runs `fill` on the Micron colour dialog when it opens, then presses OK or
// Cancel. Returns whether a dialog opened.
bool onDialog(QObject* context, std::function<void(MicronColourDialog*)> fill, bool accept, bool& ran) {
    ran = false;
    QTimer::singleShot(0, context, [fill, accept, &ran] {
        auto* d = qobject_cast<MicronColourDialog*>(QApplication::activeModalWidget());
        if (!d)
            return;
        ran = true;
        fill(d);
        auto* box = d->findChild<QDialogButtonBox*>();
        box->button(accept ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
    });
    return true;
}

} // namespace

TEST_CASE("Format > Target > Micron is one undo step that changes no cell", "[app][target][micron]") {
    MainWindow w;
    Document* doc = w.document();
    QAction* micron = act(w, "targetMicron");
    QAction* ansi = act(w, "targetAnsi");
    const Canvas before = doc->canvas();
    CHECK_FALSE(w.renderer().rgbBackgroundsKept());

    micron->trigger();
    CHECK(doc->target() == Target::Micron);
    CHECK(doc->undoStack()->undoText() == QStringLiteral("Target"));
    CHECK(micron->isChecked());
    CHECK_FALSE(ansi->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("Micron"));
    CHECK(w.paletteWidget()->target() == Target::Micron);
    CHECK(w.renderer().rgbBackgroundsKept());
    CHECK(doc->canvas() == before);

    doc->undoStack()->undo();
    CHECK(doc->target() == Target::Ansi);
    CHECK_FALSE(micron->isChecked());
    CHECK(w.targetLabel()->text() == QStringLiteral("ANSI"));
    CHECK_FALSE(w.renderer().rgbBackgroundsKept());

    doc->setTarget(Target::Irc);
    CHECK_FALSE(w.renderer().rgbBackgroundsKept());
}

TEST_CASE("the IRC meter is put away under Micron and comes back after", "[app][target][micron]") {
    MainWindow w;
    w.resize(1200, 800);
    w.show();
    IrcMeter* m = w.ircMeter();
    QAction* shown = act(w, "ircMeterShown");
    REQUIRE(m->isVisible());

    w.document()->setTarget(Target::Micron);
    CHECK_FALSE(m->isVisible());
    CHECK(shown->isChecked());  // the choice is kept for later
    w.document()->setTarget(Target::Irc);
    CHECK(m->isVisible());

    // turned off under Micron, it stays off after
    w.document()->setTarget(Target::Micron);
    shown->setChecked(false);
    shown->setChecked(true);
    CHECK_FALSE(m->isVisible());
    shown->setChecked(false);
    w.document()->setTarget(Target::Ansi);
    CHECK_FALSE(m->isVisible());
    shown->setChecked(true);
    CHECK(m->isVisible());
}

TEST_CASE("the colour pickers are offered under Micron only", "[app][target][micron]") {
    MainWindow w;
    QAction* fg = act(w, "pickForegroundColour");
    QAction* bg = act(w, "pickBackgroundColour");
    CHECK_FALSE(fg->isEnabled());
    CHECK_FALSE(bg->isEnabled());
    w.document()->setTarget(Target::Micron);
    CHECK(fg->isEnabled());
    CHECK(bg->isEnabled());
    w.document()->setTarget(Target::Irc);
    CHECK_FALSE(fg->isEnabled());
    CHECK_FALSE(bg->isEnabled());
}

TEST_CASE("the Micron colour dialog offers the 4096 twelve-bit colours", "[app][micron]") {
    CHECK(MicronColourDialog::nearestDigit(0) == 0);
    CHECK(MicronColourDialog::nearestDigit(8) == 0);
    CHECK(MicronColourDialog::nearestDigit(9) == 1);
    CHECK(MicronColourDialog::nearestDigit(255) == 15);
    CHECK(MicronColourDialog::nearestDigit(-4) == 0);
    CHECK(MicronColourDialog::nearestDigit(300) == 15);
    for (int d = 0; d < 16; ++d)
        CHECK(MicronColourDialog::nearestDigit(d * 17) == d);

    MicronColourDialog dialog({0x12, 0x34, 0x56}, QStringLiteral("Foreground Colour"));
    CHECK(dialog.windowTitle() == QStringLiteral("Foreground Colour"));
    CHECK(dialog.digits() == std::array<int, 3>{1, 3, 5});
    CHECK(dialog.code() == QStringLiteral("135"));
    CHECK(dialog.colour() == micron::twelveBit(1, 3, 5));

    auto* red = dialog.findChild<QSpinBox*>(QStringLiteral("red"));
    auto* redSlider = dialog.findChild<QSlider*>(QStringLiteral("redSlider"));
    auto* blueSlider = dialog.findChild<QSlider*>(QStringLiteral("blueSlider"));
    auto* blue = dialog.findChild<QSpinBox*>(QStringLiteral("blue"));
    auto* code = dialog.findChild<QLineEdit*>(QStringLiteral("code"));
    REQUIRE(red);
    REQUIRE(redSlider);
    REQUIRE(blueSlider);
    REQUIRE(blue);
    REQUIRE(code);
    CHECK(red->minimum() == 0);
    CHECK(red->maximum() == 15);
    CHECK(redSlider->maximum() == 15);
    CHECK(code->text() == QStringLiteral("135"));

    red->setValue(15);
    CHECK(redSlider->value() == 15);
    CHECK(code->text() == QStringLiteral("f35"));
    CHECK(dialog.colour() == Rgb{0xff, 0x33, 0x55});
    blueSlider->setValue(0);
    CHECK(blue->value() == 0);
    CHECK(dialog.code() == QStringLiteral("f30"));

    code->setText(QStringLiteral("A5f"));
    emit code->textEdited(code->text());
    CHECK(dialog.digits() == std::array<int, 3>{10, 5, 15});
    CHECK(red->value() == 10);
    CHECK(blueSlider->value() == 15);
    CHECK(dialog.colour() == Rgb{0xaa, 0x55, 0xff});
    // a part-typed code changes nothing yet
    code->setText(QStringLiteral("0"));
    emit code->textEdited(code->text());
    CHECK(dialog.digits() == std::array<int, 3>{10, 5, 15});

    dialog.setDigits(2, 4, 6);
    CHECK(code->text() == QStringLiteral("246"));
    CHECK(micron::colourCode(dialog.colour()) == "246");
}

TEST_CASE("under Micron a click on the pen opens the colour dialog", "[app][target][micron]") {
    MainWindow w;
    w.show();
    PaletteWidget* p = w.paletteWidget();
    CanvasView* v = w.canvasView();
    w.document()->setTarget(Target::Micron);
    CHECK(p->swatchRect(98).isValid());
    CHECK_FALSE(p->swatchRect(99).isValid());
    p->resize(p->sizeHint());

    bool ran = false;
    std::array<int, 3> opened{};
    onDialog(&w, [&](MicronColourDialog* d) {
        opened = d->digits();
        d->setDigits(1, 2, 3);
    }, true, ran);
    click(p, p->foregroundRect().topLeft() + QPoint(2, 2), Qt::LeftButton);
    CHECK(ran);
    // it opened on the pen's colour, light grey
    CHECK(opened == std::array<int, 3>{10, 10, 10});
    CHECK(v->pen().fg == Color::fromRgb(micron::twelveBit(1, 2, 3)));
    CHECK(swatchOf(v->pen().fg, Target::Micron) == -1);

    onDialog(&w, [](MicronColourDialog* d) { d->setDigits(15, 0, 0); }, true, ran);
    click(p, p->backgroundRect().bottomRight() - QPoint(2, 2), Qt::LeftButton);
    CHECK(ran);
    CHECK(v->pen().bg == Color::fromRgb({0xff, 0, 0}));
    CHECK(v->pen().fg == Color::fromRgb(micron::twelveBit(1, 2, 3)));

    // Cancel keeps the pen
    const Pen kept = v->pen();
    onDialog(&w, [](MicronColourDialog* d) { d->setDigits(0, 15, 0); }, false, ran);
    act(w, "pickForegroundColour")->trigger();
    CHECK(ran);
    CHECK(v->pen().fg == kept.fg);
    CHECK(v->pen().bg == kept.bg);

    // a swatch still picks its colour
    click(p, p->swatchRect(77).center(), Qt::LeftButton);
    CHECK(v->pen().fg == swatchColor(77));

    // under IRC the pen swatch opens nothing
    w.document()->setTarget(Target::Irc);
    onDialog(&w, [](MicronColourDialog*) {}, true, ran);
    click(p, p->foregroundRect().topLeft() + QPoint(2, 2), Qt::LeftButton);
    QApplication::processEvents();
    CHECK_FALSE(ran);
}

TEST_CASE("the Micron colour dialog opens on the document's palette and OK unchanged keeps the pen",
          "[app][target][micron]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    AnsiDocument a = doc->ansi();
    std::array<Rgb, 16> palette = kPalette;
    palette[7] = {0x11, 0x33, 0x55};
    a.palette = palette;
    doc->replace(a, doc->path());
    doc->setTarget(Target::Micron);
    Pen pen = v->pen();
    pen.fg = Color::indexed(7);
    v->setPen(pen);

    bool ran = false;
    std::array<int, 3> opened{};
    onDialog(&w, [&](MicronColourDialog* d) { opened = d->digits(); }, true, ran);
    act(w, "pickForegroundColour")->trigger();
    CHECK(ran);
    // the piece's own grey, as Export Micron writes it, not the standard one
    CHECK(opened == std::array<int, 3>{1, 3, 5});
    // OK with nothing changed leaves the pen indexed
    CHECK(v->pen().fg == Color::indexed(7));

    // an RGB pen no three digits hold stays exact too
    pen.fg = Color::fromRgb({0xff, 0x40, 0x40});
    v->setPen(pen);
    onDialog(&w, [](MicronColourDialog*) {}, true, ran);
    act(w, "pickForegroundColour")->trigger();
    CHECK(ran);
    CHECK(v->pen().fg == Color::fromRgb({0xff, 0x40, 0x40}));

    // a change still sets it
    onDialog(&w, [](MicronColourDialog* d) { d->setDigits(0, 0, 15); }, true, ran);
    act(w, "pickForegroundColour")->trigger();
    CHECK(ran);
    CHECK(v->pen().fg == Color::fromRgb({0, 0, 0xff}));
}

TEST_CASE("a tool's preview draws an RGB background as the Micron canvas does", "[app][target][micron]") {
    MainWindow w;
    w.resize(1000, 700);
    w.show();
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    doc->setTarget(Target::Micron);
    DocumentFormat f = doc->format();
    f.ice = false;
    REQUIRE(doc->setFormat(f, QStringLiteral("iCE")));
    const Rgb rgb{0xff, 0x40, 0x40};
    const QColor want(rgb.r, rgb.g, rgb.b);
    // a cell already on the canvas with an RGB background, inside the span
    // the edit covers
    CellEdit under;
    under.changes.push_back({3, 1, doc->canvas().at(3, 1), Cell{U' ', Color::indexed(7), Color::fromRgb(rgb), 0, false}});
    REQUIRE(doc->edit(under));
    const Cell after{U' ', Color::indexed(7), Color::fromRgb(rgb), 0, false};
    CellEdit e;
    e.changes.push_back({2, 1, doc->canvas().at(2, 1), after});
    e.changes.push_back({4, 1, doc->canvas().at(4, 1), after});
    EditPreview preview;
    preview.set(*v, e);
    QImage img(v->size(), QImage::Format_RGB32);
    img.fill(Qt::black);
    {
        QPainter p(&img);
        preview.paint(p, *v, QRect(QPoint(0, 0), v->size()));
    }
    for (int x : {2, 3, 4}) {
        INFO("cell " << x);
        CHECK(QColor(img.pixel(v->cellRect(QPoint(x, 1)).center())) == want);
    }
    REQUIRE(doc->edit(e));
    QApplication::processEvents();
    const QImage landed = v->grab().toImage();
    for (int x : {2, 3, 4}) {
        INFO("cell " << x);
        CHECK(QColor(landed.pixel(v->toWidget(v->cellRect(QPoint(x, 1))).center())) == want);
    }
}

TEST_CASE("Ctrl+arrows step from a picked colour to the first or last swatch", "[app][target][micron]") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    w.document()->setTarget(Target::Micron);
    Pen pen = v->pen();
    pen.fg = Color::fromRgb(micron::twelveBit(1, 2, 3));
    pen.bg = pen.fg;
    v->setPen(pen);
    act(w, "nextForeground")->trigger();
    CHECK(v->pen().fg == Color::indexed(0));
    act(w, "previousBackground")->trigger();
    CHECK(v->pen().bg == swatchColor(98));
    act(w, "nextForeground")->trigger();
    CHECK(v->pen().fg == Color::indexed(1));
}

TEST_CASE("a new Micron document is UTF-8, and Preferences offers Micron", "[app][target][micron]") {
    DefaultTarget restore;
    setDefaultTargetSetting(Target::Ansi);
    {
        PreferencesDialog d(fontsFolderSetting());
        const int i = d.defaultTargetBox()->findData(int(Target::Micron));
        REQUIRE(i >= 0);
        CHECK(d.defaultTargetBox()->itemText(i) == QStringLiteral("Micron (NomadNet)"));
        d.defaultTargetBox()->setCurrentIndex(i);
        CHECK(d.defaultTarget() == Target::Micron);
    }
    setDefaultTargetSetting(Target::Micron);
    CHECK(defaultTargetSetting() == Target::Micron);
    MainWindow w;
    CHECK(w.document()->target() == Target::Micron);
    CHECK(w.targetLabel()->text() == QStringLiteral("Micron"));
    QString error;
    REQUIRE(w.newCanvas(QSize(20, 5), true, &error));
    CHECK(w.document()->target() == Target::Micron);
    CHECK(w.document()->textEncoding() == TextEncoding::Utf8);
}
