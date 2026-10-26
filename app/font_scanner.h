#pragma once

#include "blockwright/font_store.h"
#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

namespace bw {

// Each worker owns its store until publication. No worker accesses the
// published catalogue; faces are resolved lazily by the GUI afterwards.
class FontScanner : public QObject {
    Q_OBJECT
public:
    using Cancelled = std::function<bool()>;
    using Scan = std::function<std::shared_ptr<const FontCatalogue>(const QString&, const Cancelled&)>;
    explicit FontScanner(QObject* parent = nullptr, Scan scan = {});
    ~FontScanner() override;
    void start(const QString& folder);
    void cancel();
    bool scanning() const { return scanning_; }
    std::shared_ptr<const FontCatalogue> catalogue() const { return catalogue_; }
signals:
    void changed();
private:
    Scan scan_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<const FontCatalogue> catalogue_;
    bool scanning_ = false;
};

} // namespace bw
