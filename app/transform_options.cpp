#include "transform_options.h"

#include "canvas_view.h"
#include "document.h"

#include "blockwright/layer.h"

#include <QBuffer>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStyle>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace bw {

namespace {

constexpr int kMax = ReferenceImage::kMaxPlace;

int side(double v) { return int(std::clamp<long long>(std::llround(v), 1, kMax)); }

// The pixels a scale, in percent, gives a side of `n` px, before the limit
// and the 1 px floor.
long long px(int n, double percent) { return std::llround(n * (percent / 100)); }

// The scale, in percent, that puts the picture's long side at the limit.
double topScale(QSize own) { return 100.0 * kMax / std::max(own.width(), own.height()); }

// The size a scale gives a picture whose own size is `own`: both sides kept
// under the limit, the picture's aspect with them, and at least 1 px.
QSize scaled(QSize own, double percent) {
    const double p = std::min(percent, topScale(own));
    const auto cut = [p](int n) { return int(std::clamp<long long>(px(n, p), 1, kMax)); };
    return {cut(own.width()), cut(own.height())};
}

// Whether some scale, uncut, gives `size` to a picture whose own size is
// `own`, by scaled()'s own arithmetic: as each side only grows with the
// scale, the first scale (any double, ordered as its bits are) where both
// sides reach the size's gives it, or none does. Past 100 * (kMax + 1)%
// every side is past the limit.
bool uniform(QSize own, QSize size) {
    const auto at = [own](std::uint64_t bits) {
        const double p = std::bit_cast<double>(bits);
        return std::pair(std::max(1LL, px(own.width(), p)), std::max(1LL, px(own.height(), p)));
    };
    std::uint64_t lo = 0, hi = std::bit_cast<std::uint64_t>(100.0 * (kMax + 1));
    while (lo < hi) {
        const std::uint64_t k = lo + (hi - lo) / 2;
        const auto [w, h] = at(k);
        w >= size.width() && h >= size.height() ? hi = k : lo = k + 1;
    }
    return at(lo) == std::pair<long long, long long>(size.width(), size.height());
}

// The size a kept edit gives for `width`, `aspect` the width over the
// height: cut where either side would pass the limit, so the aspect holds
// there as a scale's does.
QSize kept(double width, double aspect) {
    width = std::min({width, double(kMax), kMax * aspect});
    return {side(width), side(width / aspect)};
}

// The value the box keeps for `percent`, at its two decimals.
double cent(double percent) { return std::round(percent * 100) / 100; }

bool covers(QSize a, QSize b) { return a.width() >= b.width() && a.height() >= b.height(); }

// The scales, in percent, that round both sides of a picture whose own size
// is `own` to `size`'s, a side at least 1 px: from the first, up to but not
// at the second (none if that isn't past the first). With `cut`, a side at
// the limit bounds none above, as scaled() cuts there; without, the scale
// gives the size uncut.
std::pair<double, double> rounding(QSize own, QSize size, bool cut) {
    double lo = 0, hi = std::numeric_limits<double>::infinity();
    for (const auto [c, n] : {std::pair(size.width(), own.width()), std::pair(size.height(), own.height())}) {
        if (c > 1)
            lo = std::max(lo, 100.0 * (c - 0.5) / n);
        if (!cut || c < kMax)
            hi = std::min(hi, 100.0 * (c + 0.5) / n);
    }
    return {lo, hi};
}

// The hundredths of a percent in [from, to] that give `size`: none, or a
// run, as each side only grows with the scale. Exact, by trying scaled()
// itself: past the scale that puts the long side at the limit every scale
// gives the size it cuts to.
std::optional<std::pair<long long, long long>> giving(QSize own, QSize size, long long from, long long to) {
    const auto at = [own](long long k) { return scaled(own, k / 100.0); };
    // the first k in [lo, hi] where `past` holds, which once true stays true;
    // hi + 1 if none
    const auto first = [](long long lo, long long hi, auto past) {
        for (++hi; lo < hi;) {
            const long long k = lo + (hi - lo) / 2;
            past(k) ? hi = k : lo = k + 1;
        }
        return lo;
    };
    const long long lo = first(from, to, [&](long long k) { return covers(at(k), size); });
    if (lo > to || at(lo) != size)
        return std::nullopt;
    const long long hi = first(lo, to, [&](long long k) { return at(k) != size; }) - 1;
    return std::pair(lo, hi);
}

// The scale to show for `size`: one that gives it, the roundest the box's
// two decimals hold, mid-way through the scales that give it, never below
// `least` and, past the scale that puts the long side at the limit, the
// first hundredth there; else the nearest the box can show, and false. The
// scales that give a side are the ones that round it there, so those that
// give the size are where both sides' meet.
std::pair<double, bool> shownScale(QSize own, QSize size, double least, double most) {
    const double top = topScale(own);
    auto [lo, hi] = rounding(own, size, true);
    const bool open = hi <= top;  // hi itself rounds a side past
    lo = std::max(lo, least);
    hi = std::min(hi, top);
    const auto run = giving(own, size, std::llround(least * 100), std::llround(most * 100));
    if (!run) {
        if (lo > hi || (lo == hi && open)) {
            // none: a size stretched off the picture's aspect, or one below
            // the box's least; its long side's scale
            const bool wide = own.width() >= own.height();
            const double p = 100.0 * (wide ? size.width() : size.height()) / (wide ? own.width() : own.height());
            return {cent(std::clamp(p, least, top)), false};
        }
        return {cent((lo + hi) / 2), false};
    }
    // past the first hundredth at or over the limit's scale, every scale
    // gives the same size: none of those but that one is shown
    const long long first = run->first;
    const long long last = std::max(first, std::min(run->second, static_cast<long long>(std::ceil(top * 100))));
    const double mid = (first + last) / 200.0;
    long long best = first;
    for (const long long unit : {100LL, 10LL, 1LL}) {
        bool found = false;
        for (const long long k : {std::llround(mid * 100 / unit) * unit, (first + unit - 1) / unit * unit,
                                  last / unit * unit}) {
            if (k >= first && k <= last && (!found || std::abs(k / 100.0 - mid) < std::abs(best / 100.0 - mid))) {
                best = k;
                found = true;
            }
        }
        if (found)
            break;  // a hundredth always is: `first`
    }
    return {best / 100.0, true};
}

// The Scale box: its arrows, keys and wheel step from the size there is
// (TransformOptions::scaleStep), not just from the number shown.
class ScaleBox final : public QDoubleSpinBox {
public:
    std::function<std::optional<double>(int)> next;

    void stepBy(int steps) override {
        // A typed scale lands first, as a plain spin box's does; one the box
        // can't take puts the scale back and steps nowhere.
        QString text = lineEdit()->displayText();
        int pos = lineEdit()->cursorPosition();
        const bool takes = !pending() || validate(text, pos) == QValidator::Acceptable;
        interpretText();
        if (const auto p = takes ? next(steps) : std::nullopt) {
            // a "≈" scale at the box's least can step to that same number
            if (cent(*p) == value())
                emit valueChanged(value());
            else
                setValue(*p);
        }
        if (style()->styleHint(QStyle::SH_SpinBox_SelectOnStep, nullptr, this))
            selectAll();
    }

protected:
    StepEnabled stepEnabled() const override {
        if (isReadOnly() || !isEnabled())
            return QDoubleSpinBox::stepEnabled();
        // A typed scale not yet committed lands first (stepBy), and the step
        // goes on from the size it gives, so neither arrow is off for the
        // size there is now.
        if (pending())
            return StepUpEnabled | StepDownEnabled;
        StepEnabled e;
        if (next(1))
            e |= StepUpEnabled;
        if (next(-1))
            e |= StepDownEnabled;
        return e;
    }

private:
    // text typed and not yet committed
    bool pending() const { return lineEdit()->text() != prefix() + textFromValue(value()) + suffix(); }
};

QSpinBox* spin(const char* name, int min, int max, const QString& tip) {
    auto* s = new QSpinBox;
    s->setObjectName(QLatin1String(name));
    s->setRange(min, max);
    s->setSuffix(TransformOptions::tr(" px"));
    s->setKeyboardTracking(false);
    s->setToolTip(tip);
    return s;
}

} // namespace

TransformOptions::TransformOptions(CanvasView* view, QWidget* parent) : QWidget(parent), view_(view) {
    x_ = spin("transformX", -kMax, kMax, tr("The reference picture's left edge, in canvas pixels"));
    y_ = spin("transformY", -kMax, kMax, tr("The reference picture's top edge, in canvas pixels"));
    width_ = spin("transformWidth", 1, kMax, tr("The width the reference picture is drawn across, in canvas pixels"));
    height_ =
        spin("transformHeight", 1, kMax, tr("The height the reference picture is drawn across, in canvas pixels"));
    keep_ = new QCheckBox(tr("Keep aspect"));
    keep_->setObjectName(QStringLiteral("transformKeepAspect"));
    keep_->setChecked(true);
    keep_->setToolTip(tr("A new width or height takes the other side with it (a corner drag keeps it unless Shift "
                         "is held)"));
    auto* scale = new ScaleBox;
    scale->next = [this](int steps) { return scaleStep(steps); };
    // typing can turn an arrow on or off: draw them again
    connect(scale->findChild<QLineEdit*>(), &QLineEdit::textChanged, scale, qOverload<>(&QWidget::update));
    scale_ = scale;
    scale_->setObjectName(QStringLiteral("transformScale"));
    scale_->setRange(0.01, 100.0 * kMax);
    scale_->setDecimals(2);
    scale_->setSuffix(QStringLiteral("%"));
    scale_->setKeyboardTracking(false);
    scaleTip_ = tr("The size against the picture's own; a new scale sizes both sides from the picture's own");
    scale_->setToolTip(scaleTip_);
    note_ = new QLabel;
    note_->setObjectName(QStringLiteral("transformNote"));
    note_->setWordWrap(true);

    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(note_);
    form->addRow(tr("X"), x_);
    form->addRow(tr("Y"), y_);
    form->addRow(tr("Width"), width_);
    form->addRow(tr("Height"), height_);
    form->addRow(keep_);
    form->addRow(tr("Scale"), scale_);

    const auto place = [this] {
        if (const Layer* l = placeable(); l && !syncing_)
            apply(x_->value(), y_->value(), l->props.reference->width, l->props.reference->height);
    };
    connect(x_, &QSpinBox::valueChanged, this, place);
    connect(y_, &QSpinBox::valueChanged, this, place);
    connect(width_, &QSpinBox::valueChanged, this, &TransformOptions::applyWidth);
    connect(height_, &QSpinBox::valueChanged, this, &TransformOptions::applyHeight);
    connect(scale_, &QDoubleSpinBox::valueChanged, this, &TransformOptions::applyScale);
    Document* doc = view_->document();
    // Any change but a kept edit's or a scale's own (a free edit, a drag,
    // undo, another layer or picture) works the kept aspect out again, and
    // any but a scale's own shows the scale of the size it leaves.
    const auto changed = [this] {
        aspectKept_ = aspectKept_ && (keeping_ || scaling_);
        scaleKept_ = scaleKept_ && scaling_;
        sync();
    };
    connect(doc, &Document::layersChanged, this, changed);
    connect(doc, &Document::activeLayerChanged, this, changed);
    connect(doc, &Document::reset, this, changed);
    sync();
}

const Layer* TransformOptions::placeable() const {
    const Layer* l = view_->document()->active();
    if (!l || l->props.kind != LayerKind::Reference || !l->props.reference || l->props.locked || !l->props.visible)
        return nullptr;
    return l;
}

QSize TransformOptions::natural(const Layer& layer) {
    const ReferenceImage& r = *layer.props.reference;
    if (naturalOf_ == r.bytes)
        return natural_;
    naturalOf_ = r.bytes;
    natural_ = {};
    const auto type = r.bytes ? referenceImageType(*r.bytes) : std::nullopt;
    if (!type)
        return natural_;
    QByteArray data = QByteArray::fromRawData(r.bytes->data(), qsizetype(r.bytes->size()));
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    // the format the signature names, so nothing else is tried
    QImageReader reader(&buffer, QByteArray(type->data(), qsizetype(type->size())));
    QSize size = reader.size();
    // as it is drawn: a JPEG's orientation applied
    if (size.isValid() && reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        size.transpose();
    if (size.width() > 0 && size.height() > 0)
        natural_ = size;
    return natural_;
}

double TransformOptions::keptAspect(const Layer& layer) {
    if (aspectKept_)
        return aspect_;
    const ReferenceImage& r = *layer.props.reference;
    // A size some scale of the picture gives, uncut, keeps the picture's own
    // aspect exactly; a size stretched on purpose keeps its own.
    const QSize own = natural(layer);
    const bool ownAspect = !own.isEmpty() && uniform(own, QSize(r.width, r.height));
    aspect_ = ownAspect ? double(own.width()) / own.height() : double(r.width) / r.height;
    aspectKept_ = true;
    return aspect_;
}

void TransformOptions::sync() {
    const QScopedValueRollback guard(syncing_, true);
    const Layer* active = view_->document()->active();
    const Layer* l = placeable();
    note_->setText(!active || active->props.kind != LayerKind::Reference
                       ? tr("Drag to move the active layer. Place and size a reference layer here.")
                   : !active->props.reference ? tr("The reference layer has no picture.")
                   : active->props.locked     ? tr("The reference layer is locked.")
                   : !active->props.visible   ? tr("The reference layer is hidden.")
                                              : tr("Drag to move it; drag a corner to resize it."));
    for (QWidget* w : {static_cast<QWidget*>(x_), static_cast<QWidget*>(y_), static_cast<QWidget*>(width_),
                       static_cast<QWidget*>(height_), static_cast<QWidget*>(keep_)})
        w->setEnabled(l);
    if (!l) {
        scale_->setEnabled(false);
        return;
    }
    const ReferenceImage& r = *l->props.reference;
    x_->setValue(r.x);
    y_->setValue(r.y);
    width_->setValue(r.width);
    height_->setValue(r.height);
    const QSize own = natural(*l);
    scale_->setEnabled(!own.isEmpty());
    if (own.isEmpty())
        return;
    // The scale shown is the one asked for while it still gives the size
    // there is, so steps that round to the same size add up; else one that
    // gives the size (shownScale), or, when no scale the box takes gives
    // it, the nearest it can show, marked "≈". Never past the first
    // hundredth at the scale that puts the long side at the limit.
    const QSize size(r.width, r.height);
    const double asked = std::min(scale_->value(), topScale(own));
    bool exact = scaleKept_ && scaled(own, cent(asked)) == size;
    if (exact) {
        scale_->setValue(asked);
    } else {
        const auto [percent, gives] = shownScale(own, size, scale_->minimum(), scale_->maximum());
        scale_->setValue(percent);
        exact = gives;
    }
    scale_->setPrefix(exact ? QString() : QStringLiteral("≈ "));
    scale_->setToolTip(exact ? scaleTip_
                             : scaleTip_ + tr(" (≈: no scale this box takes gives this size exactly; the arrows "
                                               "step from the size)"));
}

std::optional<double> TransformOptions::scaleStep(int steps) {
    const Layer* l = placeable();
    const QSize own = l ? natural(*l) : QSize();
    if (own.isEmpty() || steps == 0)
        return std::nullopt;
    const ReferenceImage& r = *l->props.reference;
    const QSize size(r.width, r.height);
    const bool up = steps > 0;
    // A step moves no side the wrong way and, from a scale that doesn't give
    // the size, moves one the right way; where the box's end that way gives
    // no such size, it does nothing.
    const auto onward = [&](QSize to, bool strict) {
        return (up ? covers(to, size) : covers(size, to)) && (!strict || to != size);
    };
    const double top = topScale(own), least = scale_->minimum();
    if (!onward(scaled(own, up ? top : least), true))
        return std::nullopt;
    const double shown = scale_->value();
    const bool exact = scaled(own, shown) == size;
    const double next = std::clamp(shown + steps * scale_->singleStep(), least, scale_->maximum());
    if (onward(scaled(own, cent(next)), !exact))
        return next;
    // From a scale that doesn't give the size (one moved, undone or edited
    // to, or a stretch): the first scale past the number that way giving a
    // size onward, from both sides' rounding: up, every side at least where
    // it is and one past; down, every side at most and one below.
    const auto [lo, hi] = rounding(own, size, up);
    double p = up ? std::ceil(std::max(lo, std::min(hi, top)) * 100) / 100
                  : std::floor(std::min(lo, std::min(hi, top)) * 100) / 100;
    for (int i = 0; i < 100 && !onward(scaled(own, cent(p)), true); ++i)
        p += up ? 0.01 : -0.01;  // a hair off where the rounding turns
    p = std::clamp(p, least, std::ceil(top * 100) / 100);
    if (!onward(scaled(own, cent(p)), true))
        p = up ? std::ceil(top * 100) / 100 : least;
    // shown as the size it lands on would be
    const QSize to = scaled(own, cent(p));
    const auto [percent, gives] = shownScale(own, to, least, scale_->maximum());
    return gives ? percent : p;
}

void TransformOptions::apply(int x, int y, int width, int height) {
    const Layer* l = placeable();
    if (!l)
        return;
    const ReferenceImage& r = *l->props.reference;
    if (x == r.x && y == r.y && width == r.width && height == r.height) {
        sync();
        return;
    }
    const bool resized = width != r.width || height != r.height;
    LayerProperties props = l->props;
    props.reference->x = x;
    props.reference->y = y;
    props.reference->width = width;
    props.reference->height = height;
    view_->document()->setLayerProperties(props.id, std::move(props),
                                          resized ? tr("Resize Layer") : tr("Move Layer"));
}

void TransformOptions::applyWidth(int width) {
    const Layer* l = placeable();
    if (!l || syncing_)
        return;
    const ReferenceImage& r = *l->props.reference;
    const bool keep = keep_->isChecked();
    const QSize size = keep ? kept(width, keptAspect(*l)) : QSize(width, r.height);
    const QScopedValueRollback keeping(keeping_, keep);
    apply(r.x, r.y, size.width(), size.height());
}

void TransformOptions::applyHeight(int height) {
    const Layer* l = placeable();
    if (!l || syncing_)
        return;
    const ReferenceImage& r = *l->props.reference;
    const bool keep = keep_->isChecked();
    const double aspect = keep ? keptAspect(*l) : 1;
    const QSize size = keep ? kept(height * aspect, aspect) : QSize(r.width, height);
    const QScopedValueRollback keeping(keeping_, keep);
    apply(r.x, r.y, size.width(), size.height());
}

void TransformOptions::applyScale(double percent) {
    const Layer* l = placeable();
    if (!l || syncing_)
        return;
    const QSize own = natural(*l);
    if (own.isEmpty())
        return;
    const ReferenceImage& r = *l->props.reference;
    // A scale chooses the picture's own aspect: kept edits after it hold
    // that, whether or not it moved a pixel.
    aspect_ = double(own.width()) / own.height();
    aspectKept_ = true;
    scaleKept_ = true;
    const QScopedValueRollback scaling(scaling_, true);
    const QSize size = scaled(own, percent);
    apply(r.x, r.y, size.width(), size.height());
}

} // namespace bw
