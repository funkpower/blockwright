#include "generator_runner.h"

#include "document.h"

#include "blockwright/generator.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <exception>
#include <mutex>
#include <utility>

namespace bw {

namespace {

// generator::run takes one run at a time in the process, and a run
// outlives the runner that started it (closing a window doesn't wait).
std::mutex& runLock() {
    static std::mutex m;
    return m;
}

} // namespace

void killGeneratorsOnQuit(QObject* context) {
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, context,
                     [] { generator::killRunning(); });
}

GeneratorRunner::GeneratorRunner(Document* doc, QObject* parent)
    : QObject(parent), doc_(doc), watcher_(new QFutureWatcher<Result>(this)) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &GeneratorRunner::dispatch);
    connect(watcher_, &QFutureWatcher<Result>::finished, this, &GeneratorRunner::finished);
    connect(doc_, &Document::reset, this, [this] {
        pending_.clear();
        debounce_.stop();
        ++generation_;
    });
}

GeneratorRunner::~GeneratorRunner() = default;

void GeneratorRunner::request(LayerId id, quint64 serial) {
    pending_[id] = serial;
    debounce_.start();
}

bool GeneratorRunner::busy() const { return running_ || !pending_.empty(); }

void GeneratorRunner::dispatch() {
    if (running_)
        return;
    // a layer deleted or baked when its run's turn comes has nothing to run
    // (judged now, not while another run is in flight: an undo may yet
    // bring the generator back)
    const auto stale = [this](LayerId id) {
        const Layer* l = doc_->layers().find(id);
        return !l || l->props.kind != LayerKind::Generator;
    };
    while (!pending_.empty() && stale(pending_.begin()->first))
        pending_.erase(pending_.begin());
    if (pending_.empty())
        return;
    const auto [id, serial] = *pending_.begin();
    pending_.erase(pending_.begin());
    project::Project p = doc_->project();
    Result r;
    r.id = id;
    r.serial = serial;
    r.generation = generation_;
    r.params = doc_->keptFile(generator::directory(id) + std::string(generator::kParamsFile));
    r.spec = doc_->keptFile(generator::directory(id) + std::string(generator::kSpecFile));
    running_ = true;
    ++started_;
    emit runStarted(id);
    watcher_->setFuture(QtConcurrent::run([p = std::move(p), r = std::move(r)]() mutable {
        const std::lock_guard lock(runLock());
        try {
            r.cells = generator::run(p, r.id);
        } catch (const std::exception& e) {
            r.error = QString::fromStdString(e.what());
        }
        return r;
    }));
}

void GeneratorRunner::finished() {
    running_ = false;
    Result r = watcher_->result();
    if (r.generation == generation_) {
        const bool applied = r.cells && doc_->applyGeneratorRun(r.serial, r.id, r.params, r.spec, std::move(*r.cells));
        emit runFinished(r.id, applied, r.error);
    }
    if (!debounce_.isActive())
        dispatch();
}

} // namespace bw
