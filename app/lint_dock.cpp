#include "lint_dock.h"

#include "canvas_view.h"
#include "document.h"
#include "irc_meter.h"
#include "row_tree.h"

#include "blockwright/irc_render.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFutureWatcher>
#include <QGestureEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace bw {

FollowsDocument::FollowsDocument(Document* doc, const QString& staleText, QWidget* parent) :
    QWidget(parent), doc_(doc), timer_(new QTimer(this)), staleTimer_(new QTimer(this)),
    staleLabel_(new QLabel(staleText, this)), watcher_(new QFutureWatcher<Show>(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(kRefreshDelayMs);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (shown_)
            refresh();
    });
    staleTimer_->setSingleShot(true);
    staleTimer_->setInterval(kStaleShowMs);
    connect(staleTimer_, &QTimer::timeout, this, [this] { staleLabel_->setVisible(stale()); });
    // the result above it stays where it is when the note comes and goes
    QSizePolicy keep = staleLabel_->sizePolicy();
    keep.setRetainSizeWhenHidden(true);
    staleLabel_->setSizePolicy(keep);
    staleLabel_->hide();
    updateStale();
    connect(doc_, &Document::cellsChanged, this, &FollowsDocument::schedule);
    connect(doc_, &Document::reset, this, &FollowsDocument::schedule);
    connect(doc_, &Document::formatChanged, this, &FollowsDocument::schedule);
    connect(doc_, &Document::targetChanged, this, &FollowsDocument::schedule);
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
    updateStale();
}

void FollowsDocument::start() {
    // the snapshot reads every change so far, a refresh asked for while the
    // last one ran too, so none of them is waiting any more
    timer_->stop();
    stale_ = false;
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
    updateStale();
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
    updateStale();
}

void FollowsDocument::updateStale() {
    if (!stale()) {
        staleTimer_->stop();
        staleLabel_->hide();
    } else if (staleLabel_->isHidden() && !staleTimer_->isActive()) {
        staleTimer_->start();
    }
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
    FollowsDocument(doc, tr("Linting…"), parent), ramps_(new QComboBox(this)), list_(new RowTree(8, this)), summary_(new QLabel(this)) {
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
    lay->addWidget(staleLabel());

    connect(ramps_, &QComboBox::currentIndexChanged, this, &LintDock::refresh);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        emit findingActivated(item->data(0, Qt::UserRole).toPoint());
    });
    connect(list_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        emit findingActivated(item->data(0, Qt::UserRole).toPoint());
    });
}

lint::LintReport dockLint(const Canvas& canvas, bool ice, int rampPick, Target target) {
    lint::LintOptions o;
    if (rampPick <= 0) {
        o.ramps = namedRamps();
        o.craftPairs = true;
    } else {
        o.ramps = {namedRamps()[std::size_t(rampPick - 1)]};
    }
    o.ice = ice;
    o.target = target;
    return lint::lintCanvas(canvas, o);
}

FollowsDocument::Work LintDock::snapshot() {
    return [this, canvas = doc_->canvas(), ice = doc_->ansi().ice, pick = ramps_->currentIndex(),
            target = doc_->target()]() -> Show {
        auto report = std::make_shared<lint::LintReport>(dockLint(canvas, ice, pick, target));
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
        if (lint::noteKind(f.kind)) {
            // a note, no fault: in italics, the message saying so
            item->setText(3, tr("note: %1").arg(item->text(3)));
            QFont italic = item->font(0);
            italic.setItalic(true);
            for (int col = 0; col < 4; ++col)
                item->setFont(col, italic);
        }
        item->setToolTip(3, item->text(3));
        items.append(item);
    }
    list_->addTopLevelItems(items);
    QString text = QString::fromStdString(lint::summary(report_));
    if (listed < report_.findings.size())
        text += QLatin1Char('\n') + tr("The first %1 are listed.").arg(listed);
    summary_->setText(text);
}

namespace {

// The IRC View's scroll bars. An assistive technology (a screen reader, a
// switch) sets a bar's value through its accessible interface, and Qt's
// (QAccessibleScrollBar) sets it as code does, with no slider action, so it
// would not be told from lineUp's. These bars have their own interface,
// saying all Qt's does, that sets a value as a drag does: a SliderMove
// action, then the value.
class DockScrollBar : public QScrollBar {
    Q_OBJECT
public:
    explicit DockScrollBar(Qt::Orientation orientation);

    // A value the user asked for: the slider's own path (actionTriggered
    // with SliderMove, then valueChanged), clamped as setValue clamps.
    void setValueByHand(int value) {
        const bool tracking = hasTracking();
        setTracking(true);
        setSliderPosition(value);
        setTracking(tracking);
    }
};

#if QT_CONFIG(accessibility)
// QAccessibleScrollBar from public API: the same role, value, range, step,
// texts, controlling signal and (from Qt 6.11) orientation attribute.
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
using AccessibleBarBase = QAccessibleWidgetV2;
#else
using AccessibleBarBase = QAccessibleWidget;
#endif

class AccessibleDockScrollBar : public AccessibleBarBase, public QAccessibleValueInterface {
public:
    explicit AccessibleDockScrollBar(DockScrollBar* bar) : AccessibleBarBase(bar, QAccessible::ScrollBar) {
        addControllingSignal(QStringLiteral("valueChanged(int)"));
    }

    void* interface_cast(QAccessible::InterfaceType t) override {
        if (t == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return AccessibleBarBase::interface_cast(t);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    QList<QAccessible::Attribute> attributeKeys() const override {
        QList<QAccessible::Attribute> keys = AccessibleBarBase::attributeKeys();
        keys.append(QAccessible::Attribute::Orientation);
        return keys;
    }
    QVariant attributeValue(QAccessible::Attribute key) const override {
        if (key == QAccessible::Attribute::Orientation)
            return QVariant::fromValue(bar()->orientation());
        return AccessibleBarBase::attributeValue(key);
    }
#endif
    QString text(QAccessible::Text t) const override {
        if (t == QAccessible::Value)
            return QString::number(bar()->value());
        return AccessibleBarBase::text(t);
    }

    QVariant currentValue() const override { return bar()->value(); }
    void setCurrentValue(const QVariant& value) override { bar()->setValueByHand(value.toInt()); }
    QVariant maximumValue() const override { return bar()->maximum(); }
    QVariant minimumValue() const override { return bar()->minimum(); }
    QVariant minimumStepSize() const override { return bar()->singleStep(); }

private:
    DockScrollBar* bar() const { return static_cast<DockScrollBar*>(object()); }
};

QAccessibleInterface* dockScrollBarAccessible(const QString& className, QObject* object) {
    if (className == QLatin1String(DockScrollBar::staticMetaObject.className()))
        if (auto* bar = qobject_cast<DockScrollBar*>(object))
            return new AccessibleDockScrollBar(bar);
    return nullptr;
}
#endif

DockScrollBar::DockScrollBar(Qt::Orientation orientation) : QScrollBar(orientation) {
#if QT_CONFIG(accessibility)
    // once per application: Qt keeps one of each factory, and drops them all
    // when the application ends
    QAccessible::installFactory(dockScrollBarAccessible);
#endif
}

} // namespace

IrcViewDock::IrcViewDock(Document* doc, IrcMeter* meter, QWidget* parent) :
    FollowsDocument(doc, tr("Updating…"), parent), meter_(meter), picture_(new QLabel), status_(new QLabel(this)),
    scroll_(new QScrollArea(this)) {
    picture_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll_->setWidget(picture_);
    scroll_->setWidgetResizable(false);
    scroll_->setBackgroundRole(QPalette::Dark);
    scroll_->setHorizontalScrollBar(new DockScrollBar(Qt::Horizontal));
    scroll_->setVerticalScrollBar(new DockScrollBar(Qt::Vertical));
    status_->setWordWrap(true);
    // Lined up with the canvas, the picture goes back to its cell when the
    // range changes (a new picture size, the dock laid out or resized). A
    // scroll by hand is told by what the user did, not by the value: a
    // slider action (the bars' arrows, page clicks, drags and wheel, the
    // wheel and keys on the picture, the bar's menu's Top, Bottom, Page and
    // Scroll items), an item of a bar's own menu ("Scroll here" sets the
    // value without an action; the menu tells of its choice before the item
    // does its work, whether it runs in its own event loop or is left open),
    // a pan gesture on the picture and an assistive technology's new value
    // (DockScrollBar's accessible interface makes it a slider action). No other change of a bar's value
    // (lineUp, the clamp into a new range, ensureVisible) stops following.
    for (QScrollBar* bar : {scroll_->horizontalScrollBar(), scroll_->verticalScrollBar()}) {
        connect(bar, &QScrollBar::rangeChanged, this, &IrcViewDock::lineUp);
        connect(bar, &QScrollBar::actionTriggered, this, &IrcViewDock::scrolledByHand);
        // the menu is the bar's child, polished as it is first shown
        bar->installEventFilter(this);
    }
    scroll_->viewport()->installEventFilter(this);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(scroll_, 1);
    lay->addWidget(status_);
    lay->addWidget(staleLabel());
    // open, New and replace come as Document::reset, and an iCE change as
    // Document::formatChanged, which FollowsDocument follows; a new budget or
    // encoder option is redrawn at once, not after the edits' delay
    connect(doc_, &Document::ircBudgetChanged, this, &IrcViewDock::follow);
    connect(meter_, &IrcMeter::optionsChanged, this, &IrcViewDock::follow);
}

void IrcViewDock::follow() {
    if (shown())
        refresh();
    else
        schedule();
}

std::size_t IrcViewDock::budget() const { return doc_->extras().ircBudget.bytes; }

void IrcViewDock::followCanvas(CanvasView* view, QAbstractScrollArea* scroll) {
    canvas_ = view;
    canvasScroll_ = scroll;
    // A scroll moves the view in the viewport, a zoom or a new canvas size
    // resizes it, and the scroll area centres one smaller than its viewport;
    // the bars say a scroll even while the window has not been shown, when
    // the view's move events wait.
    view->installEventFilter(this);
    for (QScrollBar* bar : {scroll->horizontalScrollBar(), scroll->verticalScrollBar()})
        connect(bar, &QScrollBar::valueChanged, this, &IrcViewDock::canvasMoved);
    connect(view, &CanvasView::zoomChanged, this, &IrcViewDock::canvasMoved);
    canvasKey_.reset();
    canvasMoved();
}

bool IrcViewDock::eventFilter(QObject* watched, QEvent* e) {
    if (watched == canvas_ && (e->type() == QEvent::Move || e->type() == QEvent::Resize))
        canvasMoved();
    else if (e->type() == QEvent::ChildPolished
             && (watched == scroll_->horizontalScrollBar() || watched == scroll_->verticalScrollBar())) {
        if (auto* menu = qobject_cast<QMenu*>(static_cast<QChildEvent*>(e)->child()))
            connect(menu, &QMenu::triggered, this, &IrcViewDock::scrolledByHand, Qt::UniqueConnection);
    } else if (e->type() == QEvent::Gesture && watched == scroll_->viewport()) {
        // QAbstractScrollArea moves both bars by a pan's delta, after this
        auto* pan = static_cast<QPanGesture*>(static_cast<QGestureEvent*>(e)->gesture(Qt::PanGesture));
        if (pan && !pan->delta().isNull())
            scrolledByHand();
    }
    return FollowsDocument::eventFilter(watched, e);
}

void IrcViewDock::canvasMoved() {
    if (!canvas_ || !canvasScroll_)
        return;
    // Only a scroll (a bar's value, a part of a cell included), a zoom or a
    // new size of the canvas moves it; the scroll area centring it again in
    // a resized viewport leaves those alone, and a scroll by hand with them.
    const CanvasKey key{canvasScroll_->horizontalScrollBar()->value(), canvasScroll_->verticalScrollBar()->value(),
                        canvas_->zoom(), canvas_->size()};
    if (key != canvasKey_)
        following_ = true;
    canvasKey_ = key;
    if (!following_)
        return;
    // The viewport's top left in the view: past the view's own when it is
    // scrolled, before it (the view centred) when it is smaller.
    const QPoint top = canvas_->mapFrom(canvasScroll_->viewport(), QPoint(0, 0));
    const QPoint cell = canvas_->cellAt(canvas_->toView(QPointF(std::max(0, top.x()), std::max(0, top.y()))));
    const Canvas& cv = doc_->canvas();
    origin_ = QPoint(std::clamp(cell.x(), 0, std::max(0, cv.width() - 1)),
                     std::clamp(cell.y(), 0, std::max(0, cv.height() - 1)));
    lineUp();
}

void IrcViewDock::scrolledByHand() { following_ = false; }

void IrcViewDock::lineUp() {
    // The picture is the view's canvas at 1x, a cell to every glyph's width
    // and the font's height, so canvas cell (x, y) is the picture's cell
    // (x, y) by number. The bars clamp to their range.
    if (!following_ || image_.isNull() || view_.canvas.width() <= 0 || view_.canvas.height() <= 0)
        return;
    const int cellW = image_.width() / view_.canvas.width();
    const int cellH = image_.height() / view_.canvas.height();
    scroll_->horizontalScrollBar()->setValue(origin_.x() * cellW);
    scroll_->verticalScrollBar()->setValue(origin_.y() * cellH);
}

FollowsDocument::Work IrcViewDock::snapshot() {
    // the cells and options Export sends (IrcMeter::lines): as saved with
    // the document's iCE setting, with the row meter's options
    return [this, canvas = doc_->canvas(), ice = doc_->ansi().ice, opts = meter_->options(), budget = budget(),
            glyph = glyphWidth(doc_->ansi()), target = doc_->target()]() -> Show {
        auto view = std::make_shared<irc::View>(irc::ircView(irc::sentCanvas(canvas, ice, target), budget, opts));
        QImage image = renderIrcView(*view, glyph);
        return [this, view, image, budget] {
            view_ = std::move(*view);
            image_ = image;
            picture_->setPixmap(QPixmap::fromImage(image_));
            picture_->resize(image_.size());
            lineUp();
            const int cut = view_.cutRows();
            status_->setText(cut == 0   ? tr("Every line fits %1 bytes").arg(budget)
                             : cut == 1 ? tr("1 row cut at %1 bytes").arg(budget)
                                        : tr("%1 rows cut at %2 bytes").arg(cut).arg(budget));
        };
    };
}

} // namespace bw

#include "lint_dock.moc"
