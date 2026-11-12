// Generator layers in the app (PLAN §7, #17): the parameter form in the
// Layers dock, a changed parameter running the generator again off the UI
// thread, the trust asked before a project's generators first run, undo
// and redo putting back the params and cells without a run, and opening,
// saving and exporting running nothing. tests/data/generators/bars draws
// `count` `glyph`s in colour `ink`, and fails when `fail` is set.

#include "canvas_view.h"
#include "document.h"
#include "generator_form.h"
#include "generator_runner.h"
#include "layers_dock.h"
#include "main_window.h"
#include "message_box.h"
#include "scratch_settings.h"
#include "select_tool.h"
#include "settings.h"

#include "blockwright/canvas_size.h"
#include "blockwright/generator.h"
#include "blockwright/project.h"

#include "box_title.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <memory>
#include <vector>

#include <filesystem>

using namespace bw;
namespace fs = std::filesystem;

namespace {

fs::path fpath(const QString& p) { return fs::path(QFile::encodeName(p).toStdString()); }

template <typename F>
bool waitFor(F done, int ms = 10000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// The bars generator's files; with `sleep`, it sleeps that many seconds
// before it draws, so a test can act while its run is in flight.
std::map<std::string, std::string> barsFiles(int sleep = 0) {
#ifdef _WIN32
    SKIP("the bars generator is a shell script; test_generator_win.cpp runs generators on Windows");
#endif
    const fs::path dir = bwtest::dataDir() / "generators" / "bars";
    std::string script = bwtest::readFile(dir / "gen.sh");
    if (sleep > 0)
        script = "sleep " + std::to_string(sleep) + "\n" + script;
    return {{"generator.json", bwtest::readFile(dir / "generator.json")}, {"gen.sh", script}};
}

// A 12x2 canvas with the bars generator on its one cells layer, run once
// (three red '#'s), written as a zip at `path`; the generator's id. The
// first run is the plain generator's, so `sleep` slows only the runs after.
LayerId writeBars(const QString& path, int sleep = 0) {
    project::Project p = project::fromAnsi(parseAnsi("x"));
    p.sauce.reset();
    p.layers = LayerStack(12, 2);
    p.layers.add(makeLayer(LayerKind::Cells, 12, 2, "ground"));
    const LayerId id = generator::add(p, barsFiles(), "bars");
    if (sleep > 0)
        for (const auto& [name, bytes] : barsFiles(sleep))
            p.files[generator::directory(id) + name] = bytes;
    project::writeZip(p, fpath(path));
    return id;
}

// The bars generator alone on a 12x2 canvas, run once, so it is the layer
// a document taking it makes active; with `spec`, its generator.json is
// that instead, and with `paramsJson`, its params.json.
project::Project barsProject(LayerId* id, const std::string& spec = {}, const std::string& paramsJson = {}) {
    project::Project p = project::fromAnsi(parseAnsi("x"));
    p.sauce.reset();
    p.layers = LayerStack(12, 2);
    *id = generator::add(p, barsFiles(), "bars");
    if (!spec.empty())
        p.files[generator::directory(*id) + "generator.json"] = spec;
    if (!paramsJson.empty())
        p.files[generator::directory(*id) + "params.json"] = paramsJson;
    return p;
}

// The first row of the layer's cells, as text.
std::string row(const Document& d, LayerId id) {
    const Layer* l = d.layers().find(id);
    REQUIRE(l);
    std::string s;
    for (int x = 0; x < l->cells.width(); ++x)
        s += char(l->cells.at(x, 0).ch.codePoint().value());
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

std::string params(const Document& d, LayerId id) {
    return d.keptFile(generator::directory(id) + "params.json").value_or("(none)");
}

double count(const Document& d, LayerId id) {
    return std::get<double>(generator::paramValues(*d.generatorSpec(id), params(d, id)).at("count"));
}

// Answers each trust prompt that comes with `button`, counting them.
struct Truster {
    QMessageBox::StandardButton button;
    int asked = 0;
    QTimer timer;

    explicit Truster(QMessageBox::StandardButton b) : button(b) {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box || box->objectName() != QStringLiteral("trustGenerators"))
                return;
            ++asked;
            box->button(button)->click();
        });
        timer.start();
    }
};

QSpinBox* countSpin(GeneratorForm* form) {
    QWidget* field = form->field("count");
    REQUIRE(field);
    auto* spin = field->findChild<QSpinBox*>();
    REQUIRE(spin);
    return spin;
}

bool idle(MainWindow& w) { return !w.generatorRunner()->busy(); }

struct BarsWindow {
    QTemporaryDir tmp;
    QString path = tmp.filePath(QStringLiteral("bars.bwp"));
    LayerId id;
    MainWindow w;

    explicit BarsWindow(int sleep = 0) : id(writeBars(path, sleep)) {
        QString error;
        REQUIRE(w.openFile(path, &error));
        w.generatorRunner()->setDebounce(0);
        REQUIRE(w.document()->setActiveLayer(id));
    }
};

} // namespace

TEST_CASE("the generator form shows the declared params, under the layers, for a generator layer only") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    REQUIRE(form);
    CHECK(form->isVisibleTo(o.w.layersWidget()));
    CHECK(form->layer() == o.id);
    CHECK(countSpin(form)->value() == 3);
    CHECK(countSpin(form)->minimum() == 1);
    CHECK(countSpin(form)->maximum() == 8);
    CHECK(form->field("count")->findChild<QSlider*>());
    auto* ink = qobject_cast<QComboBox*>(form->field("ink"));
    REQUIRE(ink);
    CHECK(ink->count() == 16);
    CHECK(ink->currentIndex() == 4);
    CHECK(qobject_cast<QCheckBox*>(form->field("bright")));
    auto* glyph = qobject_cast<QLineEdit*>(form->field("glyph"));
    REQUIRE(glyph);
    CHECK(glyph->text() == QStringLiteral("#"));
    CHECK_FALSE(form->field("nope"));

    // a field let go unchanged is no step, with no params.json yet too
    REQUIRE(params(d, o.id) == "(none)");
    emit glyph->editingFinished();
    CHECK(d.undoStack()->count() == 0);
    CHECK(row(d, o.id) == "###");

    // the cells layer has no form
    REQUIRE(d.setActiveLayer(d.layers()[0].props.id));
    CHECK_FALSE(form->isVisibleTo(o.w.layersWidget()));
    CHECK(form->layer() == 0);
    REQUIRE(d.setActiveLayer(o.id));
    CHECK(form->isVisibleTo(o.w.layersWidget()));
    CHECK(o.w.generatorRunner()->started() == 0);
}

TEST_CASE("opening, saving and exporting a project with generators runs none and asks nothing") {
    BarsWindow o;
    Document& d = *o.w.document();
    Truster trust(QMessageBox::Yes);
    const Canvas cells = d.layers().find(o.id)->cells;
    QString error;
    REQUIRE(o.w.save());
    REQUIRE(o.w.saveFile(o.tmp.filePath(QStringLiteral("flat.ans")), &error, true));
    REQUIRE(o.w.exportIrcText(o.tmp.filePath(QStringLiteral("bars.txt")), &error, true));
    REQUIRE(o.w.openFile(o.path, &error));
    QApplication::processEvents();
    CHECK(trust.asked == 0);
    CHECK(o.w.generatorRunner()->started() == 0);
    CHECK(d.layers().find(o.id)->cells == cells);
    CHECK(project::read(fpath(o.path)).layers.find(o.id)->cells == cells);
}

// On macOS a box's window title isn't shown, so there the trust prompt's
// title is its heading, and its question and what runs follow (app/message_box.h).
TEST_CASE("the Run Generators prompt shows its title, as the heading where boxes have no title",
          "[message-box]") {
    const bool headings = GENERATE(false, true);
    const bool platform = headingBoxes();
    useHeadingBoxes(headings);
    const auto restore = qScopeGuard([&] { useHeadingBoxes(platform); });
    BarsWindow o;
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    const QString question = QStringLiteral("Run the generators in %1?").arg(QDir::toNativeSeparators(o.path));

    bool asked = false;
    QString heading, informative, title, text, windowTitle;
    QList<QMessageBox::StandardButton> buttons;
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || box->objectName() != QStringLiteral("trustGenerators") || asked)
            return;
        asked = true;
        heading = box->text();
        informative = box->informativeText();
        windowTitle = box->windowTitle();
        title = boxTitle(box);
        text = boxText(box);
        for (QAbstractButton* b : box->buttons())
            buttons << box->standardButton(b);
        defaultButton = box->standardButton(box->defaultButton());
        CHECK(box->icon() == QMessageBox::Warning);
        box->button(QMessageBox::No)->click();
    });
    timer.start();
    form->runButton()->click();
    REQUIRE(waitFor([&] { return asked; }));
    timer.stop();
    CHECK(title == bwtest::shownTitle(headings, QStringLiteral("Run Generators")));
    CHECK(windowTitle == bwtest::shownWindowTitle(QStringLiteral("Run Generators")));
    CHECK(text == question);
    CHECK(heading == (headings ? QStringLiteral("Run Generators") : question));
    // the question comes before what runs, under the heading on a Mac
    CHECK(informative.startsWith(headings ? question + QStringLiteral("\n\nA generator is a program")
                                          : QStringLiteral("A generator is a program")));
    CHECK(informative.contains(QStringLiteral("\n\nThis one runs: ")));
    // the buttons, the default and the answer as before
    CHECK(buttons.size() == 2);
    CHECK(buttons.contains(QMessageBox::Yes));
    CHECK(buttons.contains(QMessageBox::No));
    CHECK(defaultButton == QMessageBox::No);
    QApplication::processEvents();
    CHECK_FALSE(o.w.generatorsTrusted());
    CHECK(o.w.generatorRunner()->started() == 0);
}

TEST_CASE("declining the trust prompt runs nothing and keeps the cells; accepting runs") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    {
        Truster trust(QMessageBox::No);
        countSpin(form)->setValue(5);
        REQUIRE(waitFor([&] { return trust.asked == 1; }));
        QApplication::processEvents();
        CHECK(o.w.generatorRunner()->started() == 0);
        CHECK_FALSE(o.w.generatorsTrusted());
        CHECK(row(d, o.id) == "###");
        CHECK(count(d, o.id) == 5);
        CHECK(o.w.statusBar()->currentMessage().contains(QStringLiteral("not trusted")));

        // asked again next time, while it is declined
        form->runButton()->click();
        CHECK(trust.asked == 2);
        CHECK(o.w.generatorRunner()->started() == 0);
    }
    Truster trust(QMessageBox::Yes);
    form->runButton()->click();
    CHECK(trust.asked == 1);
    CHECK(o.w.generatorsTrusted());
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 1 && idle(o.w); }));
    CHECK(row(d, o.id) == "#####");
    CHECK(o.w.statusBar()->currentMessage().contains(QStringLiteral("Ran generator bars")));

    // trusted for the rest of the session: once more, and after reopening
    countSpin(form)->setValue(2);
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 2 && idle(o.w); }));
    CHECK(row(d, o.id) == "##");
    REQUIRE(o.w.save());
    QString error;
    REQUIRE(o.w.openFile(o.path, &error));
    CHECK(o.w.generatorsTrusted());
    REQUIRE(d.setActiveLayer(o.id));
    countSpin(form)->setValue(4);
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 3 && idle(o.w); }));
    CHECK(row(d, o.id) == "####");
    CHECK(trust.asked == 1);

    // but not another project, nor a new document
    const QString other = o.tmp.filePath(QStringLiteral("other.bwp"));
    writeBars(other);
    REQUIRE(o.w.openFile(other, &error));
    CHECK_FALSE(o.w.generatorsTrusted());
    REQUIRE(o.w.openFile(o.path, &error));
    CHECK(o.w.generatorsTrusted());
    o.w.newFile();
    CHECK_FALSE(o.w.generatorsTrusted());
}

TEST_CASE("a changed parameter runs the generator off the UI thread, once for a burst, as one undo step") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    runner->setDebounce(GeneratorRunner::kDebounceMs);
    const Canvas before = d.layers().find(o.id)->cells;
    const std::string paramsBefore = params(d, o.id);

    // each change is its own undo step at once; the run waits for the pause
    auto* ink = qobject_cast<QComboBox*>(form->field("ink"));
    REQUIRE(ink);
    countSpin(form)->setValue(6);
    CHECK(d.undoStack()->count() == 1);
    CHECK(d.undoStack()->text(0) == QStringLiteral("Generator Parameter"));
    CHECK(runner->busy());
    CHECK(runner->started() == 0);
    ink->setCurrentIndex(2);
    emit ink->activated(2);
    CHECK(d.undoStack()->count() == 2);
    REQUIRE(waitFor([&] { return runner->started() == 1 && idle(o.w); }));
    CHECK(runner->started() == 1);
    CHECK(row(d, o.id) == "######");
    CHECK(d.layers().find(o.id)->cells.at(0, 0).fg == Color::indexed(2));
    CHECK(d.canvas().at(0, 0).ch == U'#');
    CHECK(o.w.isWindowModified());
    const Canvas after = d.layers().find(o.id)->cells;
    const std::string paramsAfter = params(d, o.id);

    // undo and redo put the params and the cells back, running nothing
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK(count(d, o.id) == 6);
    CHECK(ink->currentIndex() == 4);
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK(params(d, o.id) == paramsBefore);
    CHECK(d.layers().find(o.id)->cells == before);
    CHECK(countSpin(form)->value() == 3);
    CHECK(d.canvas().at(3, 0).ch != U'#');
    CHECK_FALSE(o.w.isWindowModified());
    d.undoStack()->redo();
    d.undoStack()->redo();
    QApplication::processEvents();
    CHECK(params(d, o.id) == paramsAfter);
    CHECK(d.layers().find(o.id)->cells == after);
    CHECK(countSpin(form)->value() == 6);
    CHECK(ink->currentIndex() == 2);
    CHECK_FALSE(runner->busy());
    CHECK(runner->started() == 1);

    // the same value again is no step
    countSpin(form)->setValue(6);
    CHECK(d.undoStack()->count() == 2);
}

TEST_CASE("a saved project reopens with the params and the cells its last run left") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    auto* glyph = qobject_cast<QLineEdit*>(form->field("glyph"));
    REQUIRE(glyph);
    glyph->setText(QStringLiteral("@"));
    emit glyph->editingFinished();
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 1 && idle(o.w); }));
    CHECK(row(d, o.id) == "@@@");
    REQUIRE(o.w.save());

    const project::Project saved = project::read(fpath(o.path));
    const std::string savedParams = saved.files.at(generator::directory(o.id) + "params.json");
    CHECK(std::get<std::string>(generator::paramValues(*d.generatorSpec(o.id), savedParams).at("glyph")) == "@");
    CHECK(saved.layers.find(o.id)->cells == d.layers().find(o.id)->cells);

    MainWindow again;
    QString error;
    REQUIRE(again.openFile(o.path, &error));
    REQUIRE(again.document()->setActiveLayer(o.id));
    CHECK(row(*again.document(), o.id) == "@@@");
    CHECK(params(*again.document(), o.id) == savedParams);
    auto* shown = qobject_cast<QLineEdit*>(again.layersWidget()->generatorForm()->field("glyph"));
    REQUIRE(shown);
    CHECK(shown->text() == QStringLiteral("@"));
    CHECK(again.generatorRunner()->started() == 0);
    CHECK_FALSE(again.generatorsTrusted());
}

TEST_CASE("a failed run keeps the cells and says why in the status bar") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    auto* fail = qobject_cast<QCheckBox*>(form->field("fail"));
    REQUIRE(fail);
    const Canvas before = d.layers().find(o.id)->cells;
    fail->setChecked(true);
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 1 && idle(o.w); }));
    CHECK(d.layers().find(o.id)->cells == before);
    const QString message = o.w.statusBar()->currentMessage();
    CHECK(message.contains(QStringLiteral("Generator bars failed")));
    CHECK(message.contains(QStringLiteral("asked to fail")));
    CHECK(d.undoStack()->count() == 1);

    // a value the param can't take is refused in the form, with no step
    CHECK_FALSE(d.setGeneratorParam(o.id, "count", 99.0, nullptr));
    QString error;
    CHECK_FALSE(d.setGeneratorParam(o.id, "count", 99.0, &error));
    CHECK(error.contains(QStringLiteral("maximum")));
    CHECK_FALSE(d.setGeneratorParam(o.id, "nope", 1.0, &error));
    CHECK(d.undoStack()->count() == 1);

    // undo puts fail back off with the cells it had, running nothing
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK_FALSE(fail->isChecked());
    CHECK(d.layers().find(o.id)->cells == before);
    CHECK(o.w.generatorRunner()->started() == 1);
}

TEST_CASE("a result for a step that a newer step on its layer superseded is dropped, undone or not") {
    // Document level: the serials the steps ask runs for, and a run's cells
    // made as the runner makes them
    BarsWindow o;
    Document& d = *o.w.document();
    Truster trust(QMessageBox::No);
    std::vector<quint64> serials;
    QObject::connect(&d, &Document::generatorRunWanted, [&](LayerId, quint64 s) { serials.push_back(s); });
    const std::string dir = generator::directory(o.id);
    REQUIRE(d.setGeneratorParam(o.id, "count", 4.0));
    const std::optional<std::string> params4 = d.keptFile(dir + "params.json");
    const std::optional<std::string> spec = d.keptFile(dir + "generator.json");
    const Canvas four = generator::run(d.project(), o.id);
    REQUIRE(d.setGeneratorParam(o.id, "count", 5.0));
    REQUIRE(serials.size() == 2);

    // undoing the newer step makes the older one the newest done, with the
    // params its run read, but the newer step holds the cells from before
    d.undoStack()->undo();
    REQUIRE(d.keptFile(dir + "params.json") == params4);
    CHECK_FALSE(d.applyGeneratorRun(serials[0], o.id, params4, spec, four));
    const Canvas shown = d.layers().find(o.id)->cells;
    CHECK(row(d, o.id) == "###");
    d.undoStack()->redo();
    d.undoStack()->undo();
    CHECK(d.layers().find(o.id)->cells == shown);
    d.undoStack()->undo();
    CHECK(row(d, o.id) == "###");
    CHECK(params(d, o.id) == "(none)");
}

TEST_CASE("a result for a step that a Canvas Size step came after is dropped, undone or not") {
    // Canvas Size holds the whole stack from before it, so a late result
    // can't amend the step under it (the round-1 review's repro)
    BarsWindow o;
    Document& d = *o.w.document();
    Truster trust(QMessageBox::No);
    std::vector<quint64> serials;
    QObject::connect(&d, &Document::generatorRunWanted, [&](LayerId, quint64 s) { serials.push_back(s); });
    const std::string dir = generator::directory(o.id);
    REQUIRE(row(d, o.id) == "###");
    REQUIRE(d.setGeneratorParam(o.id, "count", 4.0));
    REQUIRE(serials.size() == 1);
    const std::optional<std::string> params4 = d.keptFile(dir + "params.json");
    const std::optional<std::string> spec = d.keptFile(dir + "generator.json");
    const Canvas four = generator::run(d.project(), o.id);
    REQUIRE(d.resizeCanvas(14, 3, Anchor::TopLeft, Selection(), Cursor{}, nullptr));

    CHECK_FALSE(d.applyGeneratorRun(serials[0], o.id, params4, spec, four));
    CHECK(row(d, o.id) == "###");
    d.undoStack()->undo();
    CHECK(d.layers().width() == 12);
    CHECK(row(d, o.id) == "###");
    // undone, the resize still holds the stack after it
    CHECK_FALSE(d.applyGeneratorRun(serials[0], o.id, params4, spec, four));
    CHECK(row(d, o.id) == "###");
    d.undoStack()->redo();
    CHECK(d.layers().width() == 14);
    CHECK(row(d, o.id) == "###");
    CHECK(d.keptFile(dir + "params.json") == params4);
}

TEST_CASE("a slow run overtaken by a newer step that is then undone leaves undo and redo consistent") {
    // the review's repro, through the runner
    BarsWindow o(1);
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    countSpin(form)->setValue(4);
    REQUIRE(waitFor([&] { return runner->started() == 1; }));
    countSpin(form)->setValue(5);
    d.undoStack()->undo();
    REQUIRE(waitFor([&] { return idle(o.w); }));
    const std::string shown = row(d, o.id);
    CHECK(shown == "###");
    d.undoStack()->redo();
    QApplication::processEvents();
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK(row(d, o.id) == shown);
    CHECK(count(d, o.id) == 4);
}

TEST_CASE("an overtaken manual run preserves the undone parameter edit") {
    // round 3's repro: a Run in flight, then a step made and undone, which
    // puts back the params the Run read
    BarsWindow o(1);
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    GeneratorRunner* runner = o.w.generatorRunner();
    {
        Truster decline(QMessageBox::No);
        countSpin(form)->setValue(4); // params 4; cached cells still ###
    }
    Truster accept(QMessageBox::Yes);
    form->runButton()->click();
    REQUIRE(waitFor([&] { return runner->started() == 1; }));
    countSpin(form)->setValue(5);
    d.undoStack()->undo();
    REQUIRE(d.undoStack()->canRedo());
    REQUIRE(waitFor([&] { return idle(o.w); }));
    CHECK(d.undoStack()->canRedo());
    CHECK(row(d, o.id) == "###");
    d.undoStack()->redo();
    CHECK(count(d, o.id) == 5);
}

TEST_CASE("a manual run keeps an edit undone while it ran and lands over one that wasn't") {
    BarsWindow o(1);
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    GeneratorRunner* runner = o.w.generatorRunner();
    {
        Truster decline(QMessageBox::No);
        countSpin(form)->setValue(4);
    }
    Truster accept(QMessageBox::Yes);
    LayerId ground = 0;
    for (std::size_t i = 0; i < d.layers().size(); ++i)
        if (d.layers()[i].props.name == "ground")
            ground = d.layers()[i].props.id;
    REQUIRE(ground != 0);

    SECTION("an edit on another layer, undone while the run is in flight, can still be redone") {
        form->runButton()->click();
        REQUIRE(waitFor([&] { return runner->started() == 1; }));
        REQUIRE(d.renameLayer(ground, QStringLiteral("floor")));
        d.undoStack()->undo();
        REQUIRE(waitFor([&] { return idle(o.w); }));
        CHECK(row(d, o.id) == "###");
        REQUIRE(d.undoStack()->canRedo());
        d.undoStack()->redo();
        CHECK(d.layers().find(ground)->props.name == "floor");
    }
    SECTION("an edit made while the run is in flight and left done doesn't stop it") {
        form->runButton()->click();
        REQUIRE(waitFor([&] { return runner->started() == 1; }));
        REQUIRE(d.renameLayer(ground, QStringLiteral("floor")));
        REQUIRE(waitFor([&] { return idle(o.w); }));
        CHECK(row(d, o.id) == "####");
        CHECK(d.undoStack()->undoText() == QStringLiteral("Run Generator"));
    }
    SECTION("a Run asked for with an edit already undone replaces it, as any edit does") {
        REQUIRE(d.renameLayer(ground, QStringLiteral("floor")));
        d.undoStack()->undo();
        REQUIRE(d.undoStack()->canRedo());
        form->runButton()->click();
        REQUIRE(waitFor([&] { return runner->started() == 1; }));
        REQUIRE(waitFor([&] { return idle(o.w); }));
        CHECK(row(d, o.id) == "####");
        CHECK_FALSE(d.undoStack()->canRedo());
    }
}

TEST_CASE("a run landing keeps a field's edit that isn't committed yet") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    auto* glyph = qobject_cast<QLineEdit*>(form->field("glyph"));
    REQUIRE(glyph);

    // typed into the text while a run is on its way
    countSpin(form)->setValue(4);
    glyph->setText(QStringLiteral("@"));
    REQUIRE(waitFor([&] { return runner->started() == 1 && idle(o.w); }));
    CHECK(row(d, o.id) == "####");
    CHECK(glyph->text() == QStringLiteral("@"));

    // and into a spin box
    auto* countText = countSpin(form)->findChild<QLineEdit*>();
    REQUIRE(countText);
    countText->setText(QStringLiteral("7"));
    auto* ink = qobject_cast<QComboBox*>(form->field("ink"));
    REQUIRE(ink);
    ink->setCurrentIndex(2);
    emit ink->activated(2);
    REQUIRE(waitFor([&] { return runner->started() == 2 && idle(o.w); }));
    CHECK(d.layers().find(o.id)->cells.at(0, 0).fg == Color::indexed(2));
    CHECK(countText->text() == QStringLiteral("7"));
    CHECK(glyph->text() == QStringLiteral("@"));

    // the kept text commits when it is let go
    emit glyph->editingFinished();
    REQUIRE(waitFor([&] { return runner->started() == 3 && idle(o.w); }));
    CHECK(row(d, o.id) == "@@@@");

    // undo still puts a field's committed value back over what it shows
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK(glyph->text() == QStringLiteral("#"));
}

TEST_CASE("a document replaced by another copy drops a field's edit not committed yet") {
    QTemporaryDir tmp;
    Document d;
    LayerId id = 0;
    d.replace(barsProject(&id), tmp.filePath(QStringLiteral("one.bwp")), FileForm::ProjectZip);
    REQUIRE(d.active() == d.layers().find(id));
    GeneratorForm form(&d);
    auto* glyph = qobject_cast<QLineEdit*>(form.field("glyph"));
    REQUIRE(glyph);
    REQUIRE(glyph->text() == QStringLiteral("#"));
    glyph->setText(QStringLiteral("@"));

    // the same generator with the same values, in another document
    LayerId again = 0;
    d.replace(barsProject(&again), tmp.filePath(QStringLiteral("two.bwp")), FileForm::ProjectZip);
    REQUIRE(again == id);
    CHECK(form.layer() == id);
    glyph = qobject_cast<QLineEdit*>(form.field("glyph"));
    REQUIRE(glyph);
    CHECK(glyph->text() == QStringLiteral("#"));
    emit glyph->editingFinished();
    CHECK(d.undoStack()->count() == 0);
    CHECK(params(d, id) == "(none)");
}

TEST_CASE("a number field holds every value its declaration takes; one it can't hold is refused") {
    QTemporaryDir tmp;
    Document d;
    LayerId id = 0;
    d.replace(barsProject(&id, R"({"command": ["sh", "gen.sh"], "params": [
        {"name": "tiny", "type": "number", "min": 0.000001, "max": 0.000003, "step": 0.000001, "default": 0.000002},
        {"name": "big", "type": "number", "default": -1e12},
        {"name": "half", "type": "number", "min": -1, "step": 0.5}]})",
                          R"({"half": 0.123456})"),
              tmp.filePath(QStringLiteral("one.bwp")), FileForm::ProjectZip);
    GeneratorForm form(&d);
    CHECK_FALSE(form.message()->isVisibleTo(&form));
    auto* tiny = qobject_cast<QDoubleSpinBox*>(form.field("tiny"));
    auto* big = qobject_cast<QDoubleSpinBox*>(form.field("big"));
    auto* half = qobject_cast<QDoubleSpinBox*>(form.field("half"));
    REQUIRE(tiny);
    REQUIRE(big);
    REQUIRE(half);
    CHECK(tiny->maximum() > tiny->minimum());
    CHECK(tiny->minimum() == 0.000001);
    CHECK(tiny->value() == 0.000002);
    CHECK(big->value() == -1e12);
    CHECK(big->minimum() <= -1e12);
    CHECK(big->maximum() >= 1e12);
    // a stored value finer than the step still shows as it is
    CHECK(half->value() == 0.123456);
    CHECK(half->minimum() == -1);
    CHECK(half->singleStep() == 0.5);

    // one past them is refused with its reason, and has no field
    d.replace(barsProject(&id, R"({"command": ["sh", "gen.sh"], "params": [
        {"name": "n", "type": "number", "min": 0.00000001, "max": 0.00000002, "step": 0.000000001,
         "default": 0.000000015}]})"),
              tmp.filePath(QStringLiteral("two.bwp")), FileForm::ProjectZip);
    CHECK_FALSE(form.field("n"));
    CHECK(form.message()->isVisibleTo(&form));
    CHECK(form.message()->text().contains(QStringLiteral("six decimal places")));
    CHECK_FALSE(form.runButton()->isEnabled());
}

TEST_CASE("a run that changes no cells leaves a document saved during its pause clean") {
    BarsWindow o;
    Document& d = *o.w.document();
    GeneratorForm* form = o.w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    auto* ink = qobject_cast<QComboBox*>(form->field("ink"));
    auto* bright = qobject_cast<QCheckBox*>(form->field("bright"));
    REQUIRE(ink);
    REQUIRE(bright);
    ink->setCurrentIndex(14);
    emit ink->activated(14);
    REQUIRE(waitFor([&] { return runner->started() == 1 && idle(o.w); }));
    REQUIRE(o.w.save());
    runner->setDebounce(GeneratorRunner::kDebounceMs);

    // ink 14 is bright already, so the run gives the cells the save holds
    bright->setChecked(true);
    REQUIRE(runner->busy());
    REQUIRE(o.w.save());
    CHECK_FALSE(o.w.isWindowModified());
    REQUIRE(waitFor([&] { return runner->started() == 2 && idle(o.w); }));
    CHECK_FALSE(d.isModified());
    CHECK_FALSE(o.w.isWindowModified());

    // a run that does change them leaves the save behind
    countSpin(form)->setValue(5);
    REQUIRE(o.w.save());
    REQUIRE(waitFor([&] { return runner->started() == 3 && idle(o.w); }));
    CHECK(row(d, o.id) == "#####");
    CHECK(d.isModified());
}

TEST_CASE("a run in flight when the document is replaced is dropped") {
    BarsWindow o(1);
    Document& d = *o.w.document();
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();

    // another project with the same layer ids, generator.json and (no)
    // params.json, whose cached cells a run wouldn't give, so only the
    // replacement tells a Run's result is not its
    const QString other = o.tmp.filePath(QStringLiteral("other.bwp"));
    REQUIRE(writeBars(other, 1) == o.id);
    project::Project p = project::read(fpath(other));
    Layer* l = p.layers.find(o.id);
    REQUIRE(l);
    l->cells.at(0, 0).ch = 'x';
    project::writeZip(p, fpath(other));

    o.w.layersWidget()->generatorForm()->runButton()->click();
    REQUIRE(trust.asked == 1);
    REQUIRE(waitFor([&] { return runner->started() == 1; }));
    QString error;
    REQUIRE(o.w.openFile(other, &error));
    REQUIRE(params(d, o.id) == "(none)");
    REQUIRE(waitFor([&] { return idle(o.w); }, 5000));
    CHECK(row(d, o.id) == "x##");
    CHECK(d.undoStack()->count() == 0);
    CHECK_FALSE(d.isModified());
    CHECK(runner->started() == 1);
}

TEST_CASE("closing a window while its run is in flight is safe, and the next window still runs") {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("bars.bwp"));
    const LayerId id = writeBars(path, 1);
    {
        Truster trust(QMessageBox::Yes);
        auto w = std::make_unique<MainWindow>();
        QString error;
        REQUIRE(w->openFile(path, &error));
        w->generatorRunner()->setDebounce(0);
        REQUIRE(w->document()->setActiveLayer(id));
        countSpin(w->layersWidget()->generatorForm())->setValue(4);
        REQUIRE(waitFor([&] { return w->generatorRunner()->started() == 1; }));
        w.reset();
        CHECK(QThreadPool::globalInstance()->waitForDone(10000));
        QApplication::processEvents();
    }
    BarsWindow o;
    Truster trust(QMessageBox::Yes);
    countSpin(o.w.layersWidget()->generatorForm())->setValue(2);
    REQUIRE(waitFor([&] { return o.w.generatorRunner()->started() == 1 && idle(o.w); }));
    CHECK(row(*o.w.document(), o.id) == "##");
}

TEST_CASE("a trusted project saved under another name stays trusted there; an untrusted one doesn't") {
    BarsWindow o;
    Truster trust(QMessageBox::Yes);
    QString error;
    const QString untrusted = o.tmp.filePath(QStringLiteral("untrusted.bwp"));
    REQUIRE(o.w.saveFile(untrusted, &error));
    CHECK(o.w.document()->path() == untrusted);
    CHECK_FALSE(o.w.generatorsTrusted());

    REQUIRE(o.w.openFile(o.path, &error));
    REQUIRE(o.w.document()->setActiveLayer(o.id));
    o.w.layersWidget()->generatorForm()->runButton()->click();
    REQUIRE(trust.asked == 1);
    REQUIRE(waitFor([&] { return idle(o.w); }));
    const QString copy = o.tmp.filePath(QStringLiteral("copy.bwp"));
    REQUIRE(o.w.saveFile(copy, &error));
    CHECK(o.w.document()->path() == copy);
    CHECK(o.w.generatorsTrusted());

    o.w.newFile();
    CHECK_FALSE(o.w.generatorsTrusted());
    REQUIRE(o.w.openFile(copy, &error));
    CHECK(o.w.generatorsTrusted());
    REQUIRE(o.w.openFile(untrusted, &error));
    CHECK_FALSE(o.w.generatorsTrusted());
    CHECK(trust.asked == 1);
}

TEST_CASE("quitting the app kills a running generator rather than waiting for it") {
    BarsWindow o(20);
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    killGeneratorsOnQuit(&o.w);
    QString failure;
    QObject::connect(runner, &GeneratorRunner::runFinished,
                     [&](LayerId, bool, const QString& error) { failure = error; });
    countSpin(o.w.layersWidget()->generatorForm())->setValue(4);
    REQUIRE(waitFor([&] { return runner->started() == 1; }));

    QElapsedTimer t;
    t.start();
    QTimer::singleShot(200, qApp, &QCoreApplication::quit);
    QApplication::exec();
    REQUIRE(waitFor([&] { return idle(o.w); }, 10000));
    CHECK(t.elapsed() < 10000);
    CHECK_FALSE(failure.isEmpty());
    CHECK(row(*o.w.document(), o.id) == "###");
    generator::testing::allowRuns();
}

TEST_CASE("File > Open Scene while a generator runs refuses before it asks or runs anything") {
    // On Windows the Python lookup runs programs, so it waits for the
    // runner as the import does; the refusal is the first thing shown,
    // before the unsaved-changes question, the file dialog or the trust
    // question.
    BarsWindow o(20);
    Truster trust(QMessageBox::Yes);
    GeneratorRunner* runner = o.w.generatorRunner();
    countSpin(o.w.layersWidget()->generatorForm())->setValue(4);
    REQUIRE(waitFor([&] { return runner->started() == 1; }));
    REQUIRE(runner->busy());
    REQUIRE(o.w.document()->isModified());
    auto* open = o.w.findChild<QAction*>(QStringLiteral("openScene"));
    REQUIRE(open);

    QStringList shown;  // each modal window, by title and text
    QTimer answer;
    answer.setInterval(10);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible())
            return;
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            if (box->objectName() == QStringLiteral("trustGenerators"))
                return;  // the Truster's
            shown << boxTitle(box) + QStringLiteral(": ") + boxText(box);
            // on a Mac, which shows a box no title, its title is its text
            CHECK((box->text() == boxTitle(box)) == headingBoxes());
            box->reject();
        } else if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            shown << dialog->windowTitle();
            dialog->reject();
        }
    });
    answer.start();
    const int starts = runner->started();
    open->trigger();
    answer.stop();
    REQUIRE_FALSE(shown.isEmpty());
    CHECK(shown.front() == QStringLiteral("Open Scene: A generator is running; open the scene when it ends."));
    CHECK(shown.size() == 1);
    CHECK(runner->started() == starts);

    generator::killRunning();
    REQUIRE(waitFor([&] { return idle(o.w); }, 10000));
    generator::testing::allowRuns();
}

TEST_CASE("a generator layer's form fits the right column at its minimum width", "[window-height]") {
    const QString scratch = QFileInfo(bwtest::scratchSettingsFile()).absolutePath();
    REQUIRE(bwtest::settingsInScratch(scratch));
    appSettings()->clear();
    const bool shownFirst = GENERATE(false, true);
    // the bars generator's own form, and one whose long label makes it wider
    // than the column's other panels
    const bool wide = GENERATE(false, true);
    CAPTURE(shownFirst, wide);
    const std::string spec = wide ? R"({"command": ["sh", "gen.sh"], "params": [
        {"name": "count", "type": "integer", "label": "How many bars to draw across the canvas", "min": 1, "max": 8}]})"
                                  : std::string();
    QTemporaryDir tmp;
    LayerId id = 0;
    {
        MainWindow w;
        const auto open = [&] {
            w.document()->replace(barsProject(&id, spec), tmp.filePath(QStringLiteral("bars.bwp")), FileForm::ProjectZip);
            QApplication::processEvents();
        };
        if (!shownFirst)
            open();
        w.resize(1024, 708); // a 1024x768 screen: the right column falls to its native minimum
        w.show();
        QApplication::processEvents();
        if (shownFirst)
            open();
        GeneratorForm* form = w.layersWidget()->generatorForm();
        REQUIRE(form->layer() == id);
        REQUIRE(form->isVisibleTo(w.layersWidget()));
        auto* scroll = qobject_cast<QScrollArea*>(w.layersDock()->widget());
        REQUIRE(scroll);
        // a form shown later widens the column through a few layout requests
        waitFor([&] { return scroll->widget()->width() <= scroll->viewport()->width(); }, 2000);
        CAPTURE(w.layersDock()->width(), scroll->viewport()->width(), scroll->widget()->minimumSizeHint().width());
        CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
        CHECK(scroll->widget()->width() <= scroll->viewport()->width());
        CHECK(form->width() >= form->minimumSizeHint().width());
    }
    appSettings()->clear();
}

namespace {

// writeBars, with the generator layer moved right by two, chars-only and
// alpha-locked, so a bake can be seen to keep its place and properties.
LayerId writeMovedBars(const QString& path) {
    project::Project p = project::fromAnsi(parseAnsi("x"));
    p.sauce.reset();
    p.layers = LayerStack(12, 2);
    Layer ground = makeLayer(LayerKind::Cells, 12, 2, "ground");
    for (int x = 0; x < 12; ++x) {
        ground.cells.at(x, 0) = Cell{};
        ground.cells.at(x, 0).bg = Color::indexed(1);
    }
    p.layers.add(std::move(ground));
    const LayerId id = generator::add(p, barsFiles(), "bars");
    Layer* l = p.layers.find(id);
    l->props.x = 2;
    l->props.mode = LayerMode::CharsOnly;
    l->props.alphaLocked = true;
    project::writeZip(p, fpath(path));
    return id;
}

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

QString kindShown(MainWindow& w, LayerId id) {
    QTreeWidget* tree = w.layersWidget()->tree();
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (LayersDock::layerOf(tree->topLevelItem(i)) == id)
            return tree->topLevelItem(i)->text(LayersDock::KindColumn);
    return {};
}

} // namespace

TEST_CASE("Bake Layer bakes a generator layer into a cells layer of its output; undo gives the generator back") {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("bars.bwp"));
    const LayerId id = writeMovedBars(path);
    MainWindow w;
    QString error;
    REQUIRE(w.openFile(path, &error));
    w.generatorRunner()->setDebounce(0);
    Document& d = *w.document();
    REQUIRE(d.setActiveLayer(id));
    GeneratorForm* form = w.layersWidget()->generatorForm();
    Truster trust(QMessageBox::Yes);

    // a parameter changed and run first, so the generator has params.json
    REQUIRE(d.setGeneratorParam(id, "count", 5.0));
    REQUIRE(waitFor([&] { return w.generatorRunner()->started() == 1 && idle(w); }));
    REQUIRE(row(d, id) == "#####");
    const std::string dir = generator::directory(id);
    const std::optional<std::string> spec = d.keptFile(dir + "generator.json");
    const std::string paramsBefore = params(d, id);
    REQUIRE(spec);
    REQUIRE(paramsBefore != "(none)");
    const Layer before = *d.layers().find(id);
    const Canvas shown = d.canvas();
    const int steps = d.undoStack()->count();

    QAction* bake = act(w, QStringLiteral("bakeLayer"));
    CHECK(Document::canBake(&before));
    REQUIRE(bake->isEnabled());
    CHECK(kindShown(w, id) == QStringLiteral("Generator"));
    CHECK(form->isVisibleTo(&w));
    bake->trigger();
    QApplication::processEvents();

    // one step; a cells layer of the same cells and properties, the
    // composite as it was
    CHECK(d.undoStack()->count() == steps + 1);
    CHECK(d.undoStack()->undoText() == QStringLiteral("Bake Generator"));
    const Layer& baked = *d.layers().find(id);
    LayerProperties want = before.props;
    want.kind = LayerKind::Cells;
    CHECK(baked.props == want);
    CHECK(baked.props.name == "bars");
    CHECK(baked.props.x == 2);
    CHECK(baked.props.mode == LayerMode::CharsOnly);
    CHECK(baked.props.visible);
    CHECK(baked.cells == before.cells);
    CHECK(d.canvas() == shown);
    CHECK(d.canvas().at(2, 0).ch == U'#');
    CHECK(d.canvas().at(2, 0).bg == Color::indexed(1));
    // the dock shows a cells layer, the form goes, nothing more to bake or run
    CHECK(kindShown(w, id) == QStringLiteral("Cells"));
    CHECK_FALSE(form->isVisibleTo(&w));
    CHECK_FALSE(bake->isEnabled());
    CHECK_FALSE(d.bakeLayer(id));
    CHECK_FALSE(d.requestGeneratorRun(id));
    CHECK_FALSE(d.setGeneratorParam(id, "count", 2.0));
    CHECK(d.editable());
    // the generator's files stay kept, for undo
    CHECK(d.keptFile(dir + "generator.json") == spec);
    CHECK(params(d, id) == paramsBefore);

    // undo gives back the generator with its params, form and cells
    d.undoStack()->undo();
    QApplication::processEvents();
    CHECK(*d.layers().find(id) == before);
    CHECK(d.canvas() == shown);
    CHECK(params(d, id) == paramsBefore);
    CHECK(kindShown(w, id) == QStringLiteral("Generator"));
    REQUIRE(form->isVisibleTo(&w));
    CHECK(countSpin(form)->value() == 5);
    CHECK(bake->isEnabled());
    CHECK_FALSE(d.editable());
    d.undoStack()->redo();
    QApplication::processEvents();
    CHECK(d.layers().find(id)->props == want);
    CHECK(d.layers().find(id)->cells == before.cells);
    CHECK(w.generatorRunner()->started() == 1);
}

TEST_CASE("a baked generator layer saves and reopens as a cells layer; its kept files stay unused") {
    QTemporaryDir tmp;
    const QString path = tmp.filePath(QStringLiteral("bars.bwp"));
    const LayerId id = writeMovedBars(path);
    Canvas cells, shown;
    std::optional<std::string> spec;
    {
        MainWindow w;
        QString error;
        REQUIRE(w.openFile(path, &error));
        Document& d = *w.document();
        REQUIRE(d.setActiveLayer(id));
        spec = d.keptFile(generator::directory(id) + "generator.json");
        REQUIRE(d.bakeLayer(id));
        cells = d.layers().find(id)->cells;
        shown = d.canvas();
        REQUIRE(w.save());
        CHECK_FALSE(w.isWindowModified());
    }

    // the file: a cells layer of the same cells, the generator's files kept
    // byte for byte and run by nothing
    const project::Project saved = project::read(fpath(path));
    const Layer* l = saved.layers.find(id);
    REQUIRE(l);
    CHECK(l->props.kind == LayerKind::Cells);
    CHECK(l->props.x == 2);
    CHECK(l->props.mode == LayerMode::CharsOnly);
    CHECK(l->cells == cells);
    CHECK(saved.files.at(generator::directory(id) + "generator.json") == *spec);
    CHECK(generator::generatorLayers(saved.layers).empty());
    CHECK_THROWS_AS(generator::run(saved, id), generator::GeneratorError);
    // the directory form reads back the same
    const fs::path dir = fpath(tmp.filePath(QStringLiteral("bars-dir")));
    project::writeDirectory(saved, dir);
    const project::Project fromDir = project::read(dir);
    CHECK(fromDir.layers.find(id)->props == l->props);
    CHECK(fromDir.layers.find(id)->cells == cells);
    CHECK(fromDir.files == saved.files);

    // reopened: the same composite, a cells layer the tools paint, no form
    MainWindow again;
    QString error;
    REQUIRE(again.openFile(path, &error));
    Document& d = *again.document();
    REQUIRE(d.setActiveLayer(id));
    CHECK(d.layers().find(id)->props.kind == LayerKind::Cells);
    CHECK(d.layers().find(id)->cells == cells);
    CHECK(d.canvas() == shown);
    CHECK(kindShown(again, id) == QStringLiteral("Cells"));
    CHECK_FALSE(again.layersWidget()->generatorForm()->isVisibleTo(&again));
    CHECK_FALSE(act(again, QStringLiteral("bakeLayer"))->isEnabled());
    CHECK(d.editable());
    CHECK(again.generatorRunner()->started() == 0);
    // saved again unchanged, it writes the same project
    const QString path2 = tmp.filePath(QStringLiteral("again.bwp"));
    REQUIRE(again.saveFile(path2, &error));
    const project::Project resaved = project::read(fpath(path2));
    CHECK(resaved.layers.find(id)->props == l->props);
    CHECK(resaved.layers.find(id)->cells == cells);
    CHECK(resaved.files == saved.files);
}

TEST_CASE("a generator baked while its run waits or runs keeps the cells it baked, and no run fails") {
    const int sleep = GENERATE(0, 1);
    CAPTURE(sleep);
    BarsWindow o(sleep);
    Document& d = *o.w.document();
    GeneratorRunner* runner = o.w.generatorRunner();
    Truster trust(QMessageBox::Yes);
    const Canvas before = d.layers().find(o.id)->cells;
    const std::string paramsBefore = params(d, o.id);
    int failed = 0;
    QObject::connect(runner, &GeneratorRunner::runFinished, [&](LayerId, bool, const QString& error) {
        failed += !error.isEmpty();
    });
    if (sleep == 0)
        runner->setDebounce(GeneratorRunner::kDebounceMs);  // baked before the run starts
    REQUIRE(d.setGeneratorParam(o.id, "count", 6.0));
    const std::string paramsAfter = params(d, o.id);
    if (sleep > 0)
        REQUIRE(waitFor([&] { return runner->started() == 1; }));  // baked while it runs
    else
        REQUIRE(runner->started() == 0);
    REQUIRE(d.bakeLayer(o.id));
    REQUIRE(waitFor([&] { return idle(o.w); }));
    QApplication::processEvents();
    CHECK(runner->started() == (sleep > 0 ? 1 : 0));
    CHECK(failed == 0);
    CHECK_FALSE(o.w.statusBar()->currentMessage().contains(QStringLiteral("failed")));
    CHECK(d.layers().find(o.id)->props.kind == LayerKind::Cells);
    CHECK(d.layers().find(o.id)->cells == before);
    CHECK(d.undoStack()->undoText() == QStringLiteral("Bake Generator"));

    // undone, the generator has the params the step set and the cells it
    // baked (the step's run was overtaken), until it runs again
    d.undoStack()->undo();
    CHECK(d.layers().find(o.id)->props.kind == LayerKind::Generator);
    CHECK(d.layers().find(o.id)->cells == before);
    CHECK(params(d, o.id) == paramsAfter);
    d.undoStack()->undo();
    CHECK(params(d, o.id) == paramsBefore);
    CHECK(d.layers().find(o.id)->cells == before);
}

TEST_CASE("a run queued behind another isn't lost when its layer goes and comes back before it starts") {
    const int how = GENERATE(0, 1);  // 0: Delete Layer and undo, 1: Bake Layer and undo
    CAPTURE(how);
    BarsWindow o(1);
    Document& d = *o.w.document();
    GeneratorRunner* runner = o.w.generatorRunner();
    Truster trust(QMessageBox::Yes);
    REQUIRE(d.setGeneratorParam(o.id, "count", 4.0));
    REQUIRE(waitFor([&] { return runner->started() == 1; }));  // count 4 runs, 1 s
    REQUIRE(d.setGeneratorParam(o.id, "count", 6.0));           // queued behind it
    const bool gone = how == 0 ? d.removeLayer(o.id) : d.bakeLayer(o.id);
    REQUIRE(gone);
    waitFor([] { return false; }, 100);  // the pause (0 ms) ends while the layer is gone or baked
    REQUIRE(runner->started() == 1);     // the first run is still going
    d.undoStack()->undo();               // the generator is back, count 6
    REQUIRE(d.layers().find(o.id));
    REQUIRE(d.layers().find(o.id)->props.kind == LayerKind::Generator);
    REQUIRE(count(d, o.id) == 6);
    REQUIRE(waitFor([&] { return idle(o.w); }));
    CHECK(runner->started() == 2);
    CHECK(row(d, o.id) == "######");
}

TEST_CASE("a float on a Generator layer the undo of Bake Layer gives back is refused for the layer's reason, not lost (#433)") {
    BarsWindow o;
    Document& d = *o.w.document();
    REQUIRE(waitFor([&] { return idle(o.w); }));
    REQUIRE(row(d, o.id) == "###");
    REQUIRE(d.bakeLayer(o.id));
    REQUIRE(d.layers().find(o.id)->props.kind == LayerKind::Cells);
    SelectTool* s = o.w.selectTool();
    o.w.tools()->setActive(s);
    REQUIRE(o.w.tools()->active() == s);
    s->select({0, 0, 2, 1});
    const auto key = [&](int k, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
        QApplication::sendEvent(o.w.canvasView(), &press);
        QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
        QApplication::sendEvent(o.w.canvasView(), &release);
    };
    key(Qt::Key_Down);  // lifts the two cells a row down
    REQUIRE(s->floating());
    d.undoStack()->undo();  // the bake undone: a Generator layer again
    REQUIRE(d.layers().find(o.id)->props.kind == LayerKind::Generator);
    REQUIRE(s->floating());
    const Canvas canvas = d.canvas();
    const int steps = d.undoStack()->count();
    key(Qt::Key_Return, QStringLiteral("\r"));
    CHECK(o.w.statusBar()->currentMessage()
          == QStringLiteral("Could not put the floating selection down: a Generator layer is drawn by its generator; "
                            "press Esc to drop it"));
    CHECK(s->floating());
    CHECK(d.canvas() == canvas);
    CHECK(d.undoStack()->count() == steps);
    key(Qt::Key_Escape);
    CHECK(!s->floating());
    CHECK(d.canvas() == canvas);
}
