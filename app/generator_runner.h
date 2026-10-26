#pragma once
// Runs generator layers for the app (PLAN §7), off the UI thread: a run
// asked for (request) waits out a short pause, so a burst of changes runs
// once, then runs on a thread of its own on a copy of the project as it is
// then (Document::project), and its cells land through
// Document::applyGeneratorRun, which drops a result gone out of date. One
// run at a time; a layer asked for again while it waits or runs runs once
// more after, for its newest step. Opening or starting a new document drops
// what waits and what is running.
//
// It runs whatever it is asked to: asking first is the caller's
// (MainWindow).

#include "blockwright/canvas.h"
#include "blockwright/layer.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

template <typename T>
class QFutureWatcher;

namespace bw {

class Document;

// Kills the generator running now, and lets none start after, when the app
// is about to quit (QCoreApplication::aboutToQuit, generator::killRunning):
// a run outlives the window that started it, and quitting would otherwise
// wait for it. Kept while `context` lives; main() hooks it to the app.
void killGeneratorsOnQuit(QObject* context);

class GeneratorRunner : public QObject {
    Q_OBJECT
public:
    static constexpr int kDebounceMs = 300;

    explicit GeneratorRunner(Document* doc, QObject* parent = nullptr);
    ~GeneratorRunner() override;

    // Runs layer `id`'s generator after the pause, for the step `serial`
    // (Document::generatorRunWanted).
    void request(LayerId id, quint64 serial);
    // A run waiting or running.
    bool busy() const;
    // How many runs have started.
    int started() const { return started_; }
    // The pause before a run, in milliseconds.
    void setDebounce(int ms) { debounce_.setInterval(ms); }

    // What a run gave: its cells, or why it gave none.
    struct Result {
        LayerId id = 0;
        quint64 serial = 0;
        unsigned generation = 0;
        std::optional<std::string> params, spec;
        std::optional<Canvas> cells;
        QString error;
    };

signals:
    void runStarted(bw::LayerId id);
    // A run ended: `applied` when its cells became the layer's, else
    // `error` says why it gave none (empty for a result out of date).
    void runFinished(bw::LayerId id, bool applied, const QString& error);

private:
    void dispatch();
    void finished();

    Document* doc_;
    QTimer debounce_;
    std::map<LayerId, quint64> pending_;
    QFutureWatcher<Result>* watcher_;
    bool running_ = false;
    unsigned generation_ = 0;
    int started_ = 0;
};

} // namespace bw
