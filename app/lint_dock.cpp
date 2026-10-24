#include "lint_dock.h"

#include "document.h"

#include "blockwright/irc_render.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"

#include <QComboBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QScrollArea>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace bw {

FollowsDocument::FollowsDocument(Document* doc, QWidget* parent) :
    QWidget(parent), doc_(doc), timer_(new QTimer(this)), watcher_(new QFutureWatcher<Show>(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(kRefreshDelayMs);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (shown_)
            refresh();
    });
    connect(doc_, &Document::cellsChanged, this, &FollowsDocument::schedule);
    connect(doc_, &Document::reset, this, &FollowsDocument::schedule);
    connect(doc_, &Document::formatChanged, this, &FollowsDocument::schedule);
    connect(watcher_, &QFutureWatcher<Show>::finished, this, &FollowsDocument::finished);
}

// The work holds its own copies, so it may finish after the dock is gone;
// its result is then never shown.
FollowsDocument::~FollowsDocument() = default;

void FollowsDocument::refresh() {
    timer_->stop();
    stale_ = false;
    if (running_)
        again_ = true;
    else
        start();
}

void FollowsDocument::start() {
    running_ = true;
    dropped_ = false;
    watcher_->setFuture(QtConcurrent::run([work = snapshot(), before = beforeWork_] {
        if (before)
            before();
        return work();
    }));
}

void FollowsDocument::finished() {
    if (again_) {
        again_ = false;
        start();
        return;
    }
    running_ = false;
    if (dropped_)
        return;
    const Show result = watcher_->result();
    result();
    emit refreshed();
}

void FollowsDocument::schedule() {
    stale_ = true;
    // a refresh running now read the document before this change
    if (running_)
        dropped_ = true;
    if (shown_)
        timer_->start();
}

void FollowsDocument::setShown(bool on) {
    shown_ = on;
    if (!on)
        timer_->stop();
    else if (stale_)
        refresh();
}

void FollowsDocument::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    setShown(true);
}

void FollowsDocument::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    setShown(false);
}

LintDock::LintDock(Document* doc, QWidget* parent) :
    FollowsDocument(doc, parent), ramps_(new QComboBox(this)), list_(new QTreeWidget(this)), summary_(new QLabel(this)) {
    ramps_->addItem(tr("All named ramps"));
    for (const Ramp& r : namedRamps())
        ramps_->addItem(QString::fromStdString(r.name));
    list_->setColumnCount(4);
    list_->setHeaderLabels({tr("Cell"), tr("Kind"), tr("Ramp"), tr("Message")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->header()->setStretchLastSection(true);
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(ramps_);
    lay->addWidget(list_, 1);
    lay->addWidget(summary_);

    connect(ramps_, &QComboBox::currentIndexChanged, this, &LintDock::refresh);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        emit findingActivated(item->data(0, Qt::UserRole).toPoint());
    });
    connect(list_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        emit findingActivated(item->data(0, Qt::UserRole).toPoint());
    });
}

FollowsDocument::Work LintDock::snapshot() {
    lint::LintOptions o;
    const int pick = ramps_->currentIndex();
    if (pick <= 0) {
        o.ramps = namedRamps();
        o.craftPairs = true;
    } else {
        o.ramps = {namedRamps()[std::size_t(pick - 1)]};
    }
    o.ice = doc_->ansi().ice;
    return [this, canvas = doc_->canvas(), o]() -> Show {
        auto report = std::make_shared<lint::LintReport>(lint::lintCanvas(canvas, o));
        return [this, report] { showReport(std::move(*report)); };
    };
}

void LintDock::showReport(lint::LintReport report) {
    report_ = std::move(report);
    list_->clear();
    const std::size_t listed = std::min(report_.findings.size(), std::size_t(kMaxLintRows));
    QList<QTreeWidgetItem*> items;
    items.reserve(qsizetype(listed));
    for (std::size_t i = 0; i < listed; ++i) {
        const lint::Finding& f = report_.findings[i];
        auto* item = new QTreeWidgetItem({QStringLiteral("%1,%2").arg(f.x).arg(f.y),
                                          QString::fromUtf8(lint::kindName(f.kind).data(), qsizetype(lint::kindName(f.kind).size())),
                                          f.ramp.empty() ? QStringLiteral("-") : QString::fromStdString(f.ramp),
                                          QString::fromStdString(f.message)});
        item->setData(0, Qt::UserRole, QPoint(f.x, f.y));
        item->setToolTip(3, item->text(3));
        items.append(item);
    }
    list_->addTopLevelItems(items);
    QString text = QString::fromStdString(lint::summary(report_));
    if (listed < report_.findings.size())
        text += QLatin1Char('\n') + tr("The first %1 are listed.").arg(listed);
    summary_->setText(text);
}

IrcViewDock::IrcViewDock(Document* doc, QWidget* parent) :
    FollowsDocument(doc, parent), picture_(new QLabel), status_(new QLabel(this)) {
    picture_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(picture_);
    scroll->setWidgetResizable(false);
    scroll->setBackgroundRole(QPalette::Dark);
    status_->setWordWrap(true);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(scroll, 1);
    lay->addWidget(status_);
    // open, New and replace come as Document::reset, which FollowsDocument
    // follows; a new budget is redrawn at once, not after the edits' delay
    connect(doc_, &Document::ircBudgetChanged, this, [this] {
        if (shown())
            refresh();
        else
            schedule();
    });
}

std::size_t IrcViewDock::budget() const { return doc_->extras().ircBudget.bytes; }

FollowsDocument::Work IrcViewDock::snapshot() {
    return [this, canvas = doc_->canvas(), budget = budget(), glyph = glyphWidth(doc_->ansi())]() -> Show {
        auto view = std::make_shared<irc::View>(irc::ircView(canvas, budget));
        QImage image = renderIrcView(*view, glyph);
        return [this, view, image, budget] {
            view_ = std::move(*view);
            image_ = image;
            picture_->setPixmap(QPixmap::fromImage(image_));
            picture_->resize(image_.size());
            const int cut = view_.cutRows();
            status_->setText(cut == 0   ? tr("Every line fits %1 bytes").arg(budget)
                             : cut == 1 ? tr("1 row cut at %1 bytes").arg(budget)
                                        : tr("%1 rows cut at %2 bytes").arg(cut).arg(budget));
        };
    };
}

} // namespace bw
