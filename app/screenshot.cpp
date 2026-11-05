#include "screenshot.h"

#include "canvas_view.h"
#include "lint_dock.h"
#include "main_window.h"
#include "settings.h"
#include "tool.h"

#include "blockwright/canvas_renderer.h"
#include "blockwright/composite.h"
#include "blockwright/library.h"
#include "blockwright/samples.h"
#include "read_file.h"
#include "replay.h"
#include "save_all.h"

#include <QApplication>
#include <QBuffer>
#include <QDockWidget>
#include <QFile>
#include <QTemporaryDir>

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

// The replay at `m` put into the window: its project, active layer and
// selection, and the step's tool, if it has one, chosen as the Tools menu
// chooses it.
void show(MainWindow& w, const bwcli::replay::Moment& m) {
    Document* doc = w.document();
    doc->replace(m.project, QString(), FileForm::ProjectZip);
    doc->setActiveLayer(m.active);
    if (Tool* t = w.tools()->find(QString::fromStdString(m.step.action)))
        w.tools()->setActive(t);
    w.canvasView()->setSelection(m.selection);
    settle();
}

} // namespace

std::vector<Shot> take(MainWindow& window, const steps::Script& script, project::Project start) {
    // As openProject: the renderer takes the canvas first, so one too large
    // to display is refused before the window has it. No step changes the
    // canvas's size.
    {
        const Canvas flat = composite(start.layers, {start.ice, Cell{}});
        AtlasRenderer probe;
        try {
            probe.setCanvas(&flat, 8);
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
    bwcli::replay::run(script, std::move(start), [&](const bwcli::replay::Moment& m) {
        if (!m.step.screenshot)
            return;
        const Screenshot& shot = *m.step.screenshot;
        using When = Screenshot::When;
        if (shot.when == (m.before ? When::After : When::Before))
            return;
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
        std::string stem = bwcli::replay::stepNumber(m.step.number, count) + "-" + m.step.name;
        if (m.before)
            stem += "-before";
        else if (shot.when == When::Both || m.step.capture == steps::Capture::BeforeAfter)
            stem += "-after";
        shots.push_back({stem, of->grab(r).toImage()});
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
        project::Project start = bwcli::replay::start(script, fs::path(path).parent_path(), samplesDir);

        // Default keys and layout whatever the user changed, and nothing
        // the window writes kept.
        ScratchSettings scratch;
        if (!scratch.valid())
            throw Failure{"no scratch settings file could be made"};
        // The Library dock shows the bundled Library and an empty user one,
        // never the user's own stamps.
        QTemporaryDir library;
        if (!library.isValid())
            throw Failure{"no scratch Library folder could be made"};
        qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(library.path()));
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
