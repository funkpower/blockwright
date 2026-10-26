#include "font_scanner.h"

#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrentRun>

namespace bw {

FontScanner::FontScanner(QObject* parent, Scan scan) : QObject(parent), scan_(std::move(scan)) {
    if (!scan_)
        scan_ = [](const QString& folder, const Cancelled& cancelled) {
            return std::make_shared<const FontCatalogue>(bw::scanFontsFolder(
                std::filesystem::path(QFile::encodeName(folder).toStdString()), cancelled));
        };
}

FontScanner::~FontScanner() { cancel(); }

void FontScanner::cancel() {
    if (cancelled_)
        *cancelled_ = true;
    scanning_ = false;
}

void FontScanner::start(const QString& folder) {
    cancel();
    cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = cancelled_;
    catalogue_.reset();
    scanning_ = !folder.isEmpty();
    emit changed();
    if (!scanning_)
        return;
    auto* watcher = new QFutureWatcher<std::shared_ptr<const FontCatalogue>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, cancelled] {
        if (!*cancelled) {
            catalogue_ = watcher->result();
            scanning_ = false;
            emit changed();
        }
        watcher->deleteLater();
    });
    // Capture values only: closing a window or dialog never waits for disk IO.
    watcher->setFuture(QtConcurrent::run([scan = scan_, folder, cancelled] {
        return scan(folder, [cancelled] { return cancelled->load(); });
    }));
}

} // namespace bw
