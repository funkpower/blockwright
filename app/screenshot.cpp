#include "screenshot.h"

#include "canvas_view.h"
#include "draw_tools.h"
#include "lint_dock.h"
#include "main_window.h"
#include "preferences.h"
#include "select_tool.h"
#include "settings.h"
#include "text_options.h"
#include "text_tool.h"
#include "tool_options.h"
#include "tool.h"

#include "blockwright/canvas_renderer.h"
#include "blockwright/canvas_size.h"
#include "blockwright/composite.h"
#include "blockwright/library.h"
#include "blockwright/samples.h"
#include "read_file.h"
#include "replay.h"
#include "save_all.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QScreen>
#include <QWindow>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QBuffer>
#include <QDockWidget>
#include <QFile>
#include <QTemporaryDir>
#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <iostream>
#include <utility>

namespace bw::screenshot {

namespace fs = std::filesystem;
using bwcli::Failure;
using steps::Screenshot;

namespace {

[[noreturn]] void fail(const steps::Step& step, const std::string& why) {
    throw Failure{steps::stepLabel(step) + ": " + why};
}

std::string utf8(const QString& s) { return s.toStdString(); }

std::string size(int w, int h) { return std::to_string(w) + " x " + std::to_string(h); }

// Every queued layout, paint and timer-free event handled, so a grab sees
// what the window would show.
void settle() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

// Each dock in `of` that follows the document (Lint, IRC View) and is
// shown, brought up to the document as it is now: its refresh started at
// once rather than a moment from now and waited for, and started again when
// the document moved past it, so the picture never holds an older result.
void current(QWidget* of) {
    QList<FollowsDocument*> docks = of->findChildren<FollowsDocument*>();
    if (auto* f = qobject_cast<FollowsDocument*>(of))
        docks.prepend(f);
    for (FollowsDocument* d : docks) {
        if (!d->shown())
            continue;
        while (d->stale()) {
            if (!d->busy())
                d->refresh();
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
        }
    }
    settle();
}

// What "of" names: the window, a dock by its object name (with or without
// the "Dock" its name ends in, so "layers" is the Layers dock), or any
// widget in the window by its object name.
QWidget* target(MainWindow& w, const steps::Step& step, const std::string& of) {
    if (of == "window")
        return &w;
    const QString name = QString::fromStdString(of);
    for (QDockWidget* d : w.docks())
        if (d->objectName() == name || d->objectName() == name + QStringLiteral("Dock"))
            return d;
    if (auto* found = w.findChild<QWidget*>(name))
        return found;
    std::string docks;
    for (QDockWidget* d : w.docks())
        docks += (docks.empty() ? "" : ", ") + utf8(d->objectName());
    fail(step, "the window has no dock or widget \"" + of + "\"; \"of\" is \"window\", a dock (" + docks +
                   ") or a widget's object name");
}

// The part of `of` the crop asks for, in its pixels.
QRect cropOf(MainWindow& w, const steps::Step& step, const Screenshot& shot, QWidget* of) {
    if (shot.crop == Screenshot::Crop::None)
        return of->rect();
    const std::string rect = "[" + std::to_string(shot.x) + ", " + std::to_string(shot.y) + ", " +
                             std::to_string(shot.width) + ", " + std::to_string(shot.height) + "]";
    if (shot.crop == Screenshot::Crop::Pixels) {
        const QRect r(shot.x, shot.y, shot.width, shot.height);
        if (!of->rect().contains(r))
            fail(step, "the crop's pixels " + rect + " are off the " + size(of->width(), of->height()) + " \"" +
                           shot.of + "\"");
        return r;
    }
    CanvasView* view = w.canvasView();
    if (of != view && !of->isAncestorOf(view))
        fail(step, "a crop in cells needs \"of\" to hold the canvas, and \"" + shot.of + "\" doesn't");
    const Canvas& cv = w.document()->canvas();
    if (shot.x + shot.width > cv.width() || shot.y + shot.height > cv.height())
        fail(step, "the crop's cells " + rect + " are off the " + size(cv.width(), cv.height()) + " canvas");
    const QRect cells = view->toWidget(view->cellRect(QPoint(shot.x, shot.y))
                                           .united(view->cellRect(QPoint(shot.x + shot.width - 1,
                                                                         shot.y + shot.height - 1))));
    // What of the canvas its scroll area shows
    QRect shown = view->rect();
    if (QWidget* port = view->parentWidget())
        shown &= QRect(view->mapFrom(port, QPoint(0, 0)), port->size());
    if (!shown.contains(cells))
        fail(step, "the crop's cells " + rect + " are not all in view in the " + size(w.width(), w.height()) +
                       " window");
    return QRect(view->mapTo(of, cells.topLeft()), cells.size());
}

// Where "pointer": true puts the arrow's tip in the picture of `of` cropped
// to `r`: the middle of the cell steps::pointerAt gives, or of the half of
// it a half-block pixel is. It has to be on the canvas the viewport shows,
// and in the picture.
// `at` is inside `area`, its right and bottom edges excluded: an arrow
// whose tip is on them is drawn wholly outside it.
bool holds(const QRectF& area, QPointF at) {
    return at.x() >= area.left() && at.y() >= area.top() && at.x() < area.right() && at.y() < area.bottom();
}

QPointF pointerTip(MainWindow& w, const bwcli::replay::Moment& m, QWidget* of, const QRect& r) {
    const steps::Step& step = m.step;
    const steps::Point p = *steps::pointerAt(step, m.before);
    CanvasView* view = w.canvasView();
    if (of != view && !of->isAncestorOf(view))
        fail(step, "a pointer needs \"of\" to hold the canvas, and \"" + step.screenshot->of + "\" doesn't");
    const bool pixel = !step.rect && steps::pixelAction(step.action);
    const int row = pixel ? (p.y >= 0 ? p.y / 2 : (p.y - 1) / 2) : p.y;
    QRectF cell(view->toWidget(view->cellRect(QPoint(p.x, row))));
    if (pixel) {
        cell.setHeight(cell.height() / 2);
        if (p.y != row * 2)
            cell.translate(0, cell.height());
    }
    const QPointF at = cell.center();
    const std::string where = (pixel ? "pixel [" : "cell [") + std::to_string(p.x) + ", " + std::to_string(p.y) + "]";
    const Canvas& cv = w.document()->canvas();
    if (p.x < 0 || row < 0 || p.x >= cv.width() || row >= cv.height())
        fail(step, "the pointer's " + where + " is off the " +
                       (pixel ? size(cv.width(), cv.height() * 2) + " pixel" : size(cv.width(), cv.height())) + " canvas");
    QRect shown = view->rect();
    if (QWidget* port = view->parentWidget())
        shown &= QRect(view->mapFrom(port, QPoint(0, 0)), port->size());
    if (!holds(QRectF(shown), at))
        fail(step, "the pointer's " + where + " is not in view in the " + size(w.width(), w.height()) + " window");
    const QPointF tip = view->mapTo(of, at) - QPointF(r.topLeft());
    if (!holds(QRectF(QPointF(0, 0), QSizeF(r.size())), tip))
        fail(step, "the pointer's " + where + " is outside the picture");
    return tip;
}

template <class T> T* toolNamed(MainWindow& w, const char* id) {
    return qobject_cast<T*>(w.tools()->find(QString::fromLatin1(id)));
}

// The replay's Tool Options and pen put where the editor keeps them (the
// view and the tools), which Tool Options and the palette follow.
void showSettings(MainWindow& w, const bwcli::replay::ToolSettings& s) {
    CanvasView* v = w.canvasView();
    v->setPen(s.pen);
    v->setBrushSettings(s.brush);
    v->setRamp(s.ramp);
    v->setSymmetry(s.symmetry);
    v->setFormOptions(s.form);
    v->setHardness(s.hardness);
    v->setOutlineShape(s.outline);
    v->setTextureOptions(s.texture);
    v->setPencilConnectivity(s.pencil);
    v->setSampleMerged(s.sampleMerged);
    if (auto* filled = w.findChild<QAction*>(QStringLiteral("filledShapes")))
        filled->setChecked(s.filled);
    if (auto* t = toolNamed<GlyphTool>(w, "glyph"))
        t->setGlyph(s.glyph);
    if (auto* t = toolNamed<RimLightTool>(w, "rimLight"))
        t->setKeepLint(s.keepLint);
    if (auto* t = toolNamed<GradientTool>(w, "gradient")) {
        t->setShape(s.gradientShape);
        t->setBanding(s.gradientBanding);
        t->setRungs(s.gradientFrom, s.gradientTo);
    }
    if (auto* t = toolNamed<SprayTool>(w, "spray")) {
        t->setKind(s.spray.kind);
        t->setDensity(s.spray.density);
        t->setSeed(s.spray.seed);
    }
    w.selectTool()->setPasteMode(s.pasteMode);
    // after the document: a new one forgets the clone source
    if (auto* t = toolNamed<CloneTool>(w, "clone"))
        t->restore(s.clone);
    // the Text tool's defaults only: the text layers are in the document
    if (auto* t = toolNamed<TextTool>(w, "text")) {
        t->setSettings(s.text, QString(), nullptr);
        // a TheDraw face the steps named comes from its file, which the
        // face picker then lists (#422)
        if (s.text.face.kind == FaceRef::Kind::Tdf)
            t->keepFont(s.fonts.file(s.text.face.file));
        if (TextOptions* o = w.toolOptions()->textOptions())
            o->sync();
    }
}

static_assert(steps::kMinZoom == CanvasView::kMinZoom && steps::kMaxZoom == CanvasView::kMaxZoom,
              "a view step's zoom is the view's");

// What the view steps so far set (#398), put on the window as the editor
// sets it: the zoom as Zoom In and Zoom Out set it, View > IRC's toggles
// checked or not (the row meter and the IRC View follow their actions),
// then, at that zoom, the scroll bars put the cell at the top left of the
// viewport, as far as they go. A field no view step has set is left as the
// window has it. None of it is the document's: no undo step, no edit.
void showView(MainWindow& w, const steps::View& v) {
    CanvasView* view = w.canvasView();
    if (v.zoom)
        view->setZoom(*v.zoom);
    const auto check = [&](const char* name, const std::optional<bool>& on) {
        if (!on)
            return;
        if (auto* a = w.findChild<QAction*>(QString::fromLatin1(name)))
            a->setChecked(*on);
    };
    check("ircFit", v.ircFit);
    check("ircPad", v.ircPad);
    check("ircSwapShades", v.ircSwapShades);
    if (v.scrollTo) {
        // the view resized to the zoom before it is scrolled
        settle();
        const QPoint want(v.scrollTo->x, v.scrollTo->y);
        const QRect cell = view->toWidget(view->cellRect(want));
        QScrollArea* scroll = w.canvasScroll();
        // The bars hold whole widget pixels. Each is set to the last value
        // that keeps the cell's whole rect in view, so its rect starts at the
        // viewport's corner and a crop of it is taken. At a whole device pixel
        // ratio the corner is then on the cell, as the IRC View reads it; off
        // one a cell's edge can fall inside a widget pixel, and the cell
        // before may show less than that pixel at the edge. The grid's offset
        // moves with the bar, so the rect is measured again at each value.
        const auto place = [&](QScrollBar* bar, int start, int (QPoint::*axis)() const) {
            const auto gap = [&] {
                const QPoint top = view->mapFrom(scroll->viewport(), QPoint(0, 0));
                return (view->toWidget(view->cellRect(want)).topLeft().*axis)() - (top.*axis)();
            };
            bar->setValue(start);
            while (gap() < 0 && bar->value() > bar->minimum())
                bar->setValue(bar->value() - 1);
            while (bar->value() < bar->maximum()) {
                bar->setValue(bar->value() + 1);
                if (gap() < 0) {
                    bar->setValue(bar->value() - 1);
                    break;
                }
            }
        };
        place(scroll->horizontalScrollBar(), cell.left(), &QPoint::x);
        place(scroll->verticalScrollBar(), cell.top(), &QPoint::y);
    }
}

// The window on the screen a picture at `scale` is taken on: the primary
// screen for 1, the first at device pixel ratio 2 for 2. It is moved there
// at its size in its own pixels and laid out again at that screen's ratio.
void putOnScreenFor(MainWindow& w, const steps::Step& step, int scale) {
    QScreen* to = QGuiApplication::primaryScreen();
    if (scale != 1) {
        to = nullptr;
        for (QScreen* s : QGuiApplication::screens())
            if (s->devicePixelRatio() == scale) {
                to = s;
                break;
            }
        if (!to)
            fail(step, "no screen draws at scale " + std::to_string(scale));
    }
    if (w.screen() == to)
        return;
    w.move(to->geometry().topLeft());
    if (QWindow* h = w.windowHandle(); h && h->screen() != to)
        h->setScreen(to);
    settle();
    if (w.screen() != to)
        fail(step, "the window couldn't be moved to the screen at scale " + std::to_string(scale));
}

// The replay at `m` put into the window: its project, active layer and
// selection, the step's tool, if it has one, chosen as the Tools menu
// chooses it, the Tool Options and pen the step is applied with, and the
// view the view steps so far set.
void show(MainWindow& w, const bwcli::replay::Moment& m) {
    Document* doc = w.document();
    doc->replace(m.project, QString(), FileForm::ProjectZip);
    doc->setActiveLayer(m.active);
    if (Tool* t = w.tools()->find(QString::fromStdString(m.step.action)))
        w.tools()->setActive(t);
    showSettings(w, m.settings);
    w.canvasView()->setSelection(m.selection);
    settle();
    showView(w, m.view);
    settle();
}

} // namespace

QPainterPath pointerArrow() {
    QPainterPath arrow;
    arrow.addPolygon(QPolygonF{{0, 0}, {0, 16}, {4, 12}, {7, 18}, {9, 17}, {6, 11}, {11, 11}, {0, 0}});
    arrow.closeSubpath();
    return arrow;
}

QRect pointerBounds() {
    // the path's box, its white edge, the half pixel it is moved by and
    // a pixel of antialiasing
    return pointerArrow().boundingRect().adjusted(-2, -2, 3, 3).toAlignedRect();
}

void drawPointer(QPixmap& picture, QPointF tip) {
    QPainter p(&picture);
    p.setRenderHint(QPainter::Antialiasing);
    // on the middle of the pixel, so its one-pixel edge covers whole pixels
    p.translate(tip + QPointF(0.5, 0.5));
    p.setPen(QPen(Qt::white, 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.setBrush(Qt::black);
    p.drawPath(pointerArrow());
}

QByteArray renamedFont(QByteArray font, std::string_view family) {
    if (family.size() != std::string_view("Noto Sans").size())
        return {};
    const auto u16 = [&](qsizetype at) { return (quint8(font[at]) << 8) | quint8(font[at + 1]); };
    const auto u32 = [&](qsizetype at) { return (quint32(u16(at)) << 16) | quint32(u16(at + 2)); };
    if (font.size() < 12)
        return {};
    qsizetype offset = 0, length = 0;
    for (int i = 0, tables = u16(4); i < tables && 12 + 16 * (i + 1) <= font.size(); ++i) {
        const qsizetype record = 12 + 16 * i;
        if (font.mid(record, 4) == QByteArrayLiteral("name") && u32(record + 8) + u32(record + 12) <= quint32(font.size())) {
            offset = u32(record + 8);
            length = u32(record + 12);
        }
    }
    if (length == 0)
        return {};
    const auto utf16 = [](const QByteArray& s) {
        QByteArray out;
        for (char c : s)
            out.append('\0').append(c);
        return out;
    };
    const QByteArray spaced(family.data(), qsizetype(family.size()));
    const QByteArray joined = QByteArray(spaced).replace(' ', "");
    const std::pair<QByteArray, QByteArray> renames[] = {{"Noto Sans", spaced},
                                                         {"NotoSans", joined},
                                                         {utf16("Noto Sans"), utf16(spaced)},
                                                         {utf16("NotoSans"), utf16(joined)}};
    QByteArray table = font.mid(offset, length);
    for (const auto& [from, to] : renames)
        table.replace(from, to);
    if (table.size() != length)
        return {};
    font.replace(offset, length, table);
    return font;
}

std::string useBundledFont() {
    for (const char* file : {":/ui-font/NotoSans-Regular.ttf", ":/ui-font/NotoSans-Bold.ttf"}) {
        QFile f(QString::fromLatin1(file));
        if (!f.open(QIODevice::ReadOnly))
            return std::string(file) + ": " + f.errorString().toStdString();
        const int id = QFontDatabase::addApplicationFontFromData(renamedFont(f.readAll(), kUiFontFamily));
        if (QFontDatabase::applicationFontFamilies(id) != QStringList{QString::fromLatin1(kUiFontFamily)})
            return std::string(file) + ": the font database didn't take it";
    }
    QFont font(QString::fromLatin1(kUiFontFamily), kUiFontPointSize);
    // what the machine's font configuration would choose, fixed: light
    // hinting and greyscale antialiasing
    font.setHintingPreference(QFont::PreferVerticalHinting);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::NoSubpixelAntialias));
    QApplication::setFont(font);
    return {};
}

QByteArray FixedScreens::configuration() {
    return QByteArrayLiteral(
        "{\"windowFrameMargins\": false, \"screens\": [\n"
        " {\"name\": \"scale1\", \"x\": 0, \"y\": 0, \"width\": 4096, \"height\": 4096,"
        " \"logicalDpi\": 96, \"logicalBaseDpi\": 96, \"dpr\": 1},\n"
        " {\"name\": \"scale2\", \"x\": 4096, \"y\": 0, \"width\": 8192, \"height\": 8192,"
        " \"logicalDpi\": 192, \"logicalBaseDpi\": 96, \"dpr\": 1}]}\n");
}

static_assert(steps::kMaxWindowSide <= 4096, "the fixed screens hold the largest window");

namespace {

// The platform's arguments are split at ':' and its list of platforms at ';'.
bool platformCanHold(const QString& path) {
    return !path.contains(QLatin1Char(':')) && !path.contains(QLatin1Char(';'));
}

} // namespace

FixedScreens::FixedScreens() {
    const QByteArray json = configuration();
    QStringList folders{QDir::tempPath()};
#ifdef Q_OS_UNIX
    if (folders.front() != QLatin1String("/tmp"))
        folders << QStringLiteral("/tmp");
#endif
    std::string why;
    for (const QString& folder : folders) {
        file_.setFileTemplate(folder + QStringLiteral("/blockwright-screens-XXXXXX.json"));
        // A path with a ':' (a drive letter) may do relative to the current folder.
        QString path = file_.fileTemplate();
        if (!platformCanHold(path))
            path = QDir::current().relativeFilePath(path);
        if (!platformCanHold(path)) {
            why = folder.toStdString() + " has a ':' or ';' in its path, which the platform's arguments can't hold";
            continue;
        }
        if (!file_.open() || file_.write(json) != json.size() || !file_.flush()) {
            why = folder.toStdString() + ": " + file_.errorString().toStdString();
            file_.remove();
            continue;
        }
        path_ = file_.fileName();
        if (!platformCanHold(path_))
            path_ = QDir::current().relativeFilePath(path_);
        break;
    }
    if (path_.isEmpty()) {
        error_ = "the screens' configuration file can't be written: " + why;
        return;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + QFile::encodeName(path_));
    for (const char* v : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_AUTO_SCREEN_SCALE_FACTOR",
                          "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY", "QT_FONT_DPI",
                          "QT_USE_PHYSICAL_DPI", "QT_DPI_ADJUSTMENT_POLICY"})
        qunsetenv(v);
}

std::vector<Shot> take(MainWindow& window, const steps::Script& script, project::Project start,
                       const std::function<void(const Shot&)>& taken) {
    // As openProject: the renderer takes the canvas first, at its cell
    // width, so one too large to display is refused before the window has it.
    {
        const Canvas flat = composite(start.layers, {start.ice, Cell{}});
        AtlasRenderer probe;
        try {
            probe.setCanvas(&flat, canvasCellWidth(start.sauce));
        } catch (const std::exception&) {
            throw Failure{"start: the canvas is too large to display"};
        }
    }
    window.resize(script.windowWidth ? QSize(*script.windowWidth, *script.windowHeight) : kDefaultWindowSize);
    window.canvasView()->setCursorShown(false);
    window.show();
    settle();

    std::vector<Shot> shots;
    const int count = int(script.steps.size());
    QSize sizeBefore;
    bwcli::replay::run(script, std::move(start), [&](const bwcli::replay::Moment& m) {
        // a canvasSize step is refused as the dialog refuses it
        // (MainWindow::resizeCanvas) when the renderer can't take the new
        // canvas at its cell width, whether or not the step asks for a
        // picture; at the canvas's own size it does nothing, as the dialog
        const QSize size(m.project.layers.width(), m.project.layers.height());
        if (m.before)
            sizeBefore = size;
        else if (m.step.action == "canvasSize" && size != sizeBefore) {
            const Canvas candidate(m.project.layers.width(), m.project.layers.height());
            AtlasRenderer probe;
            try {
                probe.setCanvas(&candidate, canvasCellWidth(m.project.sauce));
            } catch (const std::exception&) {
                fail(m.step, "the canvas is too large to display");
            }
        }
        if (!m.step.screenshot)
            return;
        const Screenshot& shot = *m.step.screenshot;
        using When = Screenshot::When;
        if (shot.when == (m.before ? When::After : When::Before))
            return;
        putOnScreenFor(window, m.step, shot.scale);
        show(window, m);
        QWidget* of = target(window, m.step, shot.of);
        // a dock in a tab behind another is brought to the front, as
        // clicking its tab would
        if (auto* dock = qobject_cast<QDockWidget*>(of)) {
            dock->raise();
            settle();
        }
        current(of);
        if (!of->isVisible())
            fail(m.step, "\"" + shot.of + "\" is hidden in the window");
        const QRect r = cropOf(window, m.step, shot, of);
        std::optional<QPointF> tip;
        if (shot.pointer)
            tip = pointerTip(window, m, of, r);
        std::string stem = bwcli::replay::stepNumber(m.step.number, count) + "-" + m.step.name;
        if (m.before)
            stem += "-before";
        else if (shot.when == When::Both || m.step.capture == steps::Capture::BeforeAfter)
            stem += "-after";
        QPixmap picture = of->grab(r);
        if (tip)
            drawPointer(picture, *tip);
        shots.push_back({stem, picture.toImage(), tip});
        if (taken)
            taken(shots.back());
    });
    return shots;
}

void write(const std::vector<Shot>& shots, const fs::path& dir, const std::function<void()>& beforeCommit) {
    std::vector<std::pair<std::string, std::string>> files;
    for (const Shot& s : shots) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!s.image.save(&buf, "PNG"))
            throw Failure{s.stem + ".png: can't encode the picture"};
        files.push_back({(dir / (s.stem + ".png")).string(), std::string(png.constData(), std::size_t(png.size()))});
    }
    // refused before <dir> is made, as cli replay refuses it
    if (const auto limit = bwcli::tooFewOpenFiles(files.size()))
        throw Failure{"not written: " + std::to_string(files.size()) + " pictures need more open files at once than the "
                      "open-file limit of " + std::to_string(*limit)};
    std::error_code ec;
    if (fs::exists(dir, ec) ? !fs::is_directory(dir, ec) : !fs::create_directories(dir, ec))
        throw Failure{dir.string() + ": " + (ec ? ec.message() : std::string("not a directory"))};
    bwcli::saveAll(files, beforeCommit);
}

Scratch::Scratch() {
    // Default keys and layout whatever the user changed, and nothing the
    // window writes kept.
    if (!settings_.valid()) {
        error_ = "no scratch settings file could be made";
        return;
    }
    // No fonts folder, set as Clear sets it: unset, the default
    // ~/tdfiglet/fonts would be scanned on a worker no picture waits for,
    // and listed once it finished (#459).
    setFontsFolderSetting(QString());
    // The Library dock shows the bundled Library and an empty user one,
    // never the user's own stamps.
    if (!library_.isValid()) {
        error_ = "no scratch Library folder could be made";
        return;
    }
    hadLibrary_ = qEnvironmentVariableIsSet("BLOCKWRIGHT_USER_LIBRARY");
    libraryBefore_ = qgetenv("BLOCKWRIGHT_USER_LIBRARY");
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(library_.path()));
}

Scratch::~Scratch() {
    if (!settings_.valid() || !library_.isValid())
        return;
    if (hadLibrary_)
        qputenv("BLOCKWRIGHT_USER_LIBRARY", libraryBefore_);
    else
        qunsetenv("BLOCKWRIGHT_USER_LIBRARY");
}

int run(const QString& stepsPath, const QString& outDir) {
    const std::string path = QFile::encodeName(stepsPath).toStdString();
    try {
        steps::Script script;
        try {
            script = steps::parse(bwcli::readFile(path, 16 << 20));
        } catch (const steps::StepsError& e) {
            throw Failure{path + ": " + e.what()};
        }
        const auto native = [](const QString& p) { return fs::path(QFile::encodeName(p).toStdString()); };
        const fs::path samplesDir = samples::bundledDir(library::realBinDir(
            native(QCoreApplication::applicationFilePath()), native(QCoreApplication::applicationDirPath())));
        // what reading the start changed, said as blockwright-cli replay
        // says it: before any step and before the window is made (#383)
        const fs::path stepsDir = fs::path(path).parent_path();
        project::Project start = bwcli::replay::startSaying(script, stepsDir, samplesDir, std::cerr, "blockwright", path);
        // every file a step reads, as blockwright-cli replay reads them (#422)
        bwcli::replay::load(script, stepsDir);

        const Scratch scratch;
        if (!scratch.error().empty())
            throw Failure{scratch.error()};
        std::vector<Shot> shots;
        {
            MainWindow w;
            shots = take(w, script, std::move(start));
        }
        if (shots.empty())
            std::cerr << "blockwright: " << path << ": no step asks for a screenshot\n";
        write(shots, native(outDir));
        return 0;
    } catch (const Failure& f) {
        std::cerr << "blockwright: " << f.message << "\n";
        return 1;
    }
}

} // namespace bw::screenshot
