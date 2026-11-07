#include "canvas_size_dialog.h"

#include "plural.h"

#include "blockwright/ansi.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdlib>

namespace bw {

namespace {

struct AnchorLook {
    const char* arrow;
    const char* name;
};

// Each anchor's arrow points the way the canvas grows from it.
constexpr std::array<AnchorLook, 9> kLooks = {{{"↖", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Top left")},
                                               {"↑", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Top")},
                                               {"↗", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Top right")},
                                               {"←", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Left")},
                                               {"●", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Centre")},
                                               {"→", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Right")},
                                               {"↙", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Bottom left")},
                                               {"↓", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Bottom")},
                                               {"↘", QT_TRANSLATE_NOOP("CanvasSizeDialog", "Bottom right")}}};

QString side(int n, bool columns, const QString& where) {
    if (n == 0)
        return {};
    const int count = std::abs(n);
    const QString what = columns ? plural(count, CanvasSizeDialog::tr("%n column"), CanvasSizeDialog::tr("%n columns"))
                                 : plural(count, CanvasSizeDialog::tr("%n row"), CanvasSizeDialog::tr("%n rows"));
    return n > 0 ? CanvasSizeDialog::tr("%1 added %2").arg(what, where)
                 : CanvasSizeDialog::tr("%1 cut %2").arg(what, where);
}

} // namespace

QString canvasSizeSummary(QSize from, QSize to, Anchor anchor) {
    const CellShift s = anchorShift(from.width(), from.height(), to.width(), to.height(), anchor);
    QStringList parts;
    for (const QString& p : {side(s.dx, true, CanvasSizeDialog::tr("on the left")),
                             side(to.width() - from.width() - s.dx, true, CanvasSizeDialog::tr("on the right")),
                             side(s.dy, false, CanvasSizeDialog::tr("at the top")),
                             side(to.height() - from.height() - s.dy, false, CanvasSizeDialog::tr("at the bottom"))})
        if (!p.isEmpty())
            parts << p;
    if (parts.isEmpty())
        return CanvasSizeDialog::tr("No change");
    QString text = parts.join(QStringLiteral("; "));
    text[0] = text[0].toUpper();
    return text;
}

CanvasSizeDialog::CanvasSizeDialog(QSize canvas, QWidget* parent) : QDialog(parent), old_(canvas) {
    setObjectName(QStringLiteral("canvasSizeDialog"));
    setWindowTitle(tr("Canvas Size"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    width_ = new QSpinBox;
    width_->setObjectName(QStringLiteral("canvasWidth"));
    width_->setRange(1, kMaxColumns);
    width_->setValue(canvas.width());
    width_->setSuffix(tr(" columns"));
    width_->setToolTip(tr("The new width, 1 to %1 columns").arg(kMaxColumns));
    height_ = new QSpinBox;
    height_->setObjectName(QStringLiteral("canvasHeight"));
    height_->setRange(1, kMaxRows);
    height_->setValue(canvas.height());
    height_->setSuffix(tr(" rows"));
    height_->setToolTip(tr("The new height, 1 to %1 rows").arg(kMaxRows));
    form->addRow(tr("&Width:"), width_);
    form->addRow(tr("&Height:"), height_);
    auto* now = new QLabel(tr("Now %1 × %2").arg(canvas.width()).arg(canvas.height()));
    form->addRow(QString(), now);

    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    for (std::size_t i = 0; i < kLooks.size(); ++i) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("anchor-%1").arg(QString::fromLatin1(anchorName(Anchor(i)))));
        b->setText(QString::fromUtf8(kLooks[i].arrow));
        b->setCheckable(true);
        b->setAutoRaise(false);
        b->setFixedSize(32, 32);
        const QString name = tr(kLooks[i].name);
        b->setAccessibleName(name);
        b->setToolTip(tr("Keep the canvas at the %1").arg(name.toLower()));
        group_->addButton(b, int(i));
        grid->addWidget(b, int(i / 3), int(i % 3));
        anchors_[i] = b;
    }
    auto* gridBox = new QWidget;
    gridBox->setLayout(grid);
    auto* anchorLabel = new QLabel(tr("&Anchor:"));
    anchorLabel->setBuddy(anchors_[0]);
    form->addRow(anchorLabel, gridBox);

    summary_ = new QLabel;
    summary_->setObjectName(QStringLiteral("canvasSizeSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    auto* undoNote = new QLabel(tr("Cut cells can be brought back with Undo."));
    undoNote->setWordWrap(true);
    layout->addWidget(undoNote);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(width_, &QSpinBox::valueChanged, this, &CanvasSizeDialog::refresh);
    connect(height_, &QSpinBox::valueChanged, this, &CanvasSizeDialog::refresh);
    connect(group_, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on)
            refresh();
    });
    setAnchor(Anchor::TopLeft);
    refresh();
}

QSize CanvasSizeDialog::canvasSize() const { return {width_->value(), height_->value()}; }

Anchor CanvasSizeDialog::anchor() const { return Anchor(std::max(0, group_->checkedId())); }

void CanvasSizeDialog::setAnchor(Anchor anchor) { anchors_[std::size_t(anchor)]->setChecked(true); }

void CanvasSizeDialog::refresh() { summary_->setText(canvasSizeSummary(old_, canvasSize(), anchor())); }

} // namespace bw
