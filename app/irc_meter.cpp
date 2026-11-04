#include "irc_meter.h"

#include "canvas_view.h"
#include "document.h"
#include "plural.h"
#include "tool.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace bw {

void GutterScrollArea::setGutter(QWidget* gutter, int width) {
    gutter_ = gutter;
    width_ = gutter ? width : 0;
    setViewportMargins(0, 0, width_, 0);
    placeGutter();
}

void GutterScrollArea::resizeEvent(QResizeEvent* e) {
    QScrollArea::resizeEvent(e);
    placeGutter();
}

void GutterScrollArea::placeGutter() {
    if (!gutter_)
        return;
    // Beside the viewport, above the horizontal scroll bar.
    const QRect v = viewport()->geometry();
    gutter_->setGeometry(v.right() + 1, v.top(), width_, v.height());
}

IrcMeter::IrcMeter(Document* doc, CanvasView* view, GutterScrollArea* scroll)
    : QWidget(scroll), doc_(doc), view_(view), scroll_(scroll) {
    setObjectName(QStringLiteral("ircMeter"));
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
    resetRows();
    connect(doc_, &Document::cellsChanged, this, &IrcMeter::invalidate);
    connect(doc_, &Document::reset, this, &IrcMeter::resetRows);
    connect(doc_, &Document::formatChanged, this, &IrcMeter::resetRows);
    connect(doc_, &Document::ircBudgetChanged, this, &IrcMeter::resetRows);
    connect(doc_, &Document::targetChanged, this, &IrcMeter::resetRows);
    connect(view_, &CanvasView::toolChanged, this, &IrcMeter::toolChanged);
    toolChanged();
    // The rows follow the canvas as it scrolls, zooms and moves.
    view_->installEventFilter(this);
    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(view_, &CanvasView::zoomChanged, this, qOverload<>(&QWidget::update));
    setShown(true);
}

const project::IrcBudget& IrcMeter::budget() const { return doc_->extras().ircBudget; }

void IrcMeter::setFit(bool on) {
    if (on != fit_) {
        fit_ = on;
        resetRows();
        emit optionsChanged();
    }
}

void IrcMeter::setPad(bool on) {
    if (on != pad_) {
        pad_ = on;
        resetRows();
        emit optionsChanged();
    }
}

void IrcMeter::setSwapShades(bool on) {
    if (on != swapShades_) {
        swapShades_ = on;
        resetRows();
        emit optionsChanged();
    }
}

irc::EncodeOptions IrcMeter::options() const {
    irc::EncodeOptions o;
    o.swapShades = swapShades_;
    o.pad = pad_;
    o.fit = fit_ ? budget().bytes : 0;
    return o;
}

void IrcMeter::setShown(bool on) {
    scroll_->setGutter(on ? this : nullptr, sizeHint().width());
    setVisible(on);
}

const IrcMeter::Row& IrcMeter::row(int y) {
    static const Row none{{}, true};
    if (y < 0 || y >= int(rows_.size()))
        return none;
    Row& r = rows_[std::size_t(y)];
    if (!r.valid) {
        const Canvas& cv = doc_->canvas();
        std::vector<Cell> cells(std::size_t(cv.width()));
        for (int x = 0; x < cv.width(); ++x)
            cells[std::size_t(x)] = cv.at(x, y);
        // The tool's edit in progress, as the view draws it over the canvas.
        auto c = std::lower_bound(pending_.begin(), pending_.end(), y,
                                  [](const CellChange& a, int row) { return a.y < row; });
        for (; c != pending_.end() && c->y == y; ++c)
            if (c->x >= 0 && c->x < cv.width())
                cells[std::size_t(c->x)] = c->after;
        const bool ice = doc_->ansi().ice;
        for (Cell& k : cells)
            k = irc::sentCell(k, ice, doc_->target());
        r = {encoded(irc::mircRow(cells)), true};
    }
    return r;
}

irc::Line IrcMeter::encoded(const std::vector<irc::MircCell>& cells) {
    const irc::EncodeOptions o = options();
    std::string key;
    key.reserve(cells.size() * 6 + 10);
    const auto put = [&key](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            key += char(v >> (8 * i));
    };
    put(o.fit, 8);
    put(o.pad, 1);
    put(o.swapShades, 1);
    // A cluster by its code points, not its id: the key says what is drawn
    // whatever pool the id came from (PLAN §16, Storage).
    for (const irc::MircCell& c : cells) {
        const GlyphCodePoints cps = codePoints(c.ch);
        put(std::uint64_t(cps.size), 1);
        for (const char32_t cp : cps)
            put(cp, 4);
        put(c.fg, 1);
        put(c.bg, 1);
    }
    if (const auto it = cache_.find(key); it != cache_.end())
        return it->second;
    // A few canvases' worth of rows, then start again.
    if (cache_.size() >= std::max<std::size_t>(4096, rows_.size() * 8))
        cache_.clear();
    return cache_.emplace(std::move(key), irc::encodeMircLine(cells, o)).first->second;
}

std::size_t IrcMeter::bytes(int y) { return row(y).line.text.size(); }

int IrcMeter::merged(int y) { return row(y).line.merged; }

IrcMeter::RowState IrcMeter::state(int y) {
    const irc::Line& l = row(y).line;
    if (l.text.size() > budget().bytes)
        return RowState::Over;
    return l.merged > 0 ? RowState::Merged : RowState::Fits;
}

QString IrcMeter::describe(int y) {
    const irc::Line& l = row(y).line;
    const std::size_t n = l.text.size();
    QString s = tr("Row %1: %2 of %3 bytes").arg(y).arg(n).arg(budget().bytes);
    if (n > budget().bytes)
        s += tr(", over by %1").arg(n - budget().bytes);
    if (l.merged > 0)
        s += plural(l.merged, tr(", %n cell merged to fit"), tr(", %n cells merged to fit"));
    return s;
}

void IrcMeter::resetRows() {
    rows_.assign(std::size_t(std::max(0, doc_->canvas().height())), Row{});
    update();
}

void IrcMeter::invalidate(const QRegion& cells) {
    for (const QRect& r : cells)
        for (int y = r.top(); y <= r.bottom(); ++y)
            invalidateRow(y);
    update();
}

void IrcMeter::invalidateRow(int y) {
    if (y >= 0 && y < int(rows_.size()))
        rows_[std::size_t(y)].valid = false;
}

void IrcMeter::toolChanged() {
    disconnect(toolConnection_);
    if (Tool* t = view_->tool())
        toolConnection_ = connect(t, &Tool::changed, this, &IrcMeter::pendingChanged);
    pendingChanged();
}

void IrcMeter::pendingChanged() {
    std::vector<CellChange> now;
    if (const Tool* t = view_->tool()) {
        // As the view draws it: the composite with the edit on the active
        // layer, empty when the layer would refuse it; row by row, a cell
        // changed twice landing as its last change.
        const Canvas& cv = doc_->canvas();
        for (const CellChange& c : doc_->previewEdit(t->pending()).changes)
            if (cv.contains(c.x, c.y))
                now.push_back(c);
        std::stable_sort(now.begin(), now.end(), [](const CellChange& a, const CellChange& b) { return a.y < b.y; });
    }
    if (now == pending_)
        return;
    for (const CellChange& c : pending_)
        invalidateRow(c.y);
    pending_ = std::move(now);
    for (const CellChange& c : pending_)
        invalidateRow(c.y);
    update();
}

std::vector<irc::Line> IrcMeter::lines() {
    std::vector<irc::Line> out;
    out.reserve(rows_.size());
    for (int y = 0; y < int(rows_.size()); ++y)
        out.push_back(row(y).line);
    irc::trimBlankLines(out);
    return out;
}

QRect IrcMeter::rowRect(int y) const {
    const QRect cell = view_->toWidget(view_->cellRect(QPoint(0, y)));
    const QPoint top = mapFrom(window(), view_->mapTo(window(), cell.topLeft()));
    return QRect(0, top.y(), width(), cell.height());
}

int IrcMeter::rowAt(int y) const {
    const QRect first = rowRect(0);
    if (first.height() <= 0)
        return -1;
    return int(std::floor(double(y - first.top()) / first.height()));
}

QSize IrcMeter::sizeHint() const { return {fontMetrics().horizontalAdvance(QStringLiteral("00000")) + 12, 0}; }

bool IrcMeter::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int y = rowAt(he->pos().y());
        if (y >= 0 && y < int(rows_.size()))
            QToolTip::showText(he->globalPos(), describe(y), this, rowRect(y));
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

bool IrcMeter::eventFilter(QObject* watched, QEvent* e) {
    if (watched == view_ && (e->type() == QEvent::Move || e->type() == QEvent::Resize))
        update();
    return QWidget::eventFilter(watched, e);
}

void IrcMeter::paintEvent(QPaintEvent* e) {
    QPainter p(this);
    const int first = std::max(0, rowAt(e->rect().top()));
    const int last = std::min(int(rows_.size()) - 1, rowAt(e->rect().bottom()));
    const QFontMetrics fm = fontMetrics();
    const std::size_t limit = std::max<std::size_t>(1, budget().bytes);
    for (int y = first; y <= last; ++y) {
        const QRect band = rowRect(y);
        const RowState st = state(y);
        const std::size_t n = bytes(y);
        QColor colour = st == RowState::Over     ? QColor(0xd0, 0x30, 0x30)
                        : st == RowState::Merged ? QColor(0xe0, 0x9a, 0x20)
                                                 : palette().color(QPalette::Mid);
        // A bar as long as the row's share of the budget, full when over.
        const int w = int(std::lround(double(band.width()) * std::min<double>(1.0, double(n) / double(limit))));
        p.fillRect(QRect(band.left(), band.top(), w, std::max(1, band.height() - 1)), colour);
        if (band.height() >= fm.height() && n > 0) {
            p.setPen(st == RowState::Fits ? palette().color(QPalette::Text) : Qt::black);
            p.drawText(band.adjusted(2, 0, -2, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(n));
        }
    }
}

IrcBudgetDialog::IrcBudgetDialog(const project::IrcBudget& budget, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("IRC Budget"));
    preset_ = new QComboBox;
    preset_->setObjectName(QStringLiteral("ircPreset"));
    for (const auto& p : irc::kBudgetPresets)
        preset_->addItem(tr("%1 (%2 bytes)").arg(QString::fromUtf8(p.name.data(), qsizetype(p.name.size())))
                             .arg(p.bytes),
                         QString::fromUtf8(p.profile.data(), qsizetype(p.profile.size())));
    preset_->addItem(tr("Custom"), QString::fromUtf8(irc::kCustomProfile.data(), qsizetype(irc::kCustomProfile.size())));
    bytes_ = new QSpinBox;
    bytes_->setObjectName(QStringLiteral("ircBytes"));
    bytes_->setRange(int(irc::kMinBudget), int(irc::kMaxBudget));
    bytes_->setSuffix(tr(" bytes"));
    mask_ = new QLineEdit;
    mask_->setObjectName(QStringLiteral("ircMask"));
    mask_->setPlaceholderText(tr("nick!user@host"));
    target_ = new QLineEdit;
    target_->setObjectName(QStringLiteral("ircTarget"));
    target_->setPlaceholderText(tr("#channel"));

    const int i = preset_->findData(QString::fromStdString(budget.profile));
    preset_->setCurrentIndex(i >= 0 ? i : preset_->count() - 1);
    bytes_->setValue(int(std::clamp(budget.bytes, irc::kMinBudget, irc::kMaxBudget)));

    auto* hint = new QLabel(tr("A line is relayed as \":mask PRIVMSG #channel :text\" in 512 bytes. "
                               "Give your mask and the channel to work out the room left."));
    hint->setWordWrap(true);
    auto* form = new QFormLayout(this);
    form->addRow(tr("&Preset:"), preset_);
    form->addRow(tr("&Budget:"), bytes_);
    form->addRow(hint);
    form->addRow(tr("&Mask:"), mask_);
    form->addRow(tr("&Channel:"), target_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(preset_, &QComboBox::activated, this, &IrcBudgetDialog::presetPicked);
    connect(bytes_, &QSpinBox::valueChanged, this, [this](int value) {
        // A byte count no preset has is a custom budget.
        const int k = preset_->currentIndex();
        if (k < int(irc::kBudgetPresets.size()) && irc::kBudgetPresets[std::size_t(k)].bytes != std::size_t(value))
            preset_->setCurrentIndex(preset_->count() - 1);
    });
    connect(mask_, &QLineEdit::textEdited, this, &IrcBudgetDialog::prefixEdited);
    connect(target_, &QLineEdit::textEdited, this, &IrcBudgetDialog::prefixEdited);
}

project::IrcBudget IrcBudgetDialog::budget() const {
    return {preset_->currentData().toString().toStdString(), std::size_t(bytes_->value())};
}

void IrcBudgetDialog::presetPicked() {
    const int k = preset_->currentIndex();
    if (k < int(irc::kBudgetPresets.size()))
        bytes_->setValue(int(irc::kBudgetPresets[std::size_t(k)].bytes));
}

void IrcBudgetDialog::prefixEdited() {
    if (const auto b = irc::budgetFor(mask_->text().toStdString(), target_->text().toStdString())) {
        preset_->setCurrentIndex(preset_->count() - 1);
        bytes_->setValue(int(*b));
    }
}

} // namespace bw
