#include "frame_style_dialog.h"

#include "library_dock.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace bw {

namespace {

QSpinBox* insetSpin(const char* name, int max, const QString& tip) {
    auto* s = new QSpinBox;
    s->setObjectName(QString::fromLatin1(name));
    s->setRange(0, std::max(0, max));
    s->setValue(std::min(1, max));
    s->setToolTip(tip);
    return s;
}

} // namespace

FrameStyleDialog::FrameStyleDialog(const Canvas& cells, QWidget* parent) : QDialog(parent), cells_(cells) {
    setWindowTitle(tr("Save as Frame Style"));
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("frameStyleName"));
    const int w = cells.width(), h = cells.height();
    left_ = insetSpin("frameInsetLeft", w - 1, tr("The columns of the left corners and edge"));
    right_ = insetSpin("frameInsetRight", w - 1, tr("The columns of the right corners and edge"));
    top_ = insetSpin("frameInsetTop", h - 1, tr("The rows of the top corners and edge"));
    bottom_ = insetSpin("frameInsetBottom", h - 1, tr("The rows of the bottom corners and edge"));
    fill_ = new QCheckBox(tr("Draw the &middle"));
    fill_->setObjectName(QStringLiteral("frameFill"));
    fill_->setToolTip(tr("Repeat the middle cells to fill the frame; off, the middle is left as it is"));
    keepBg_ = new QCheckBox(tr("&Keep the background under it"));
    keepBg_->setObjectName(QStringLiteral("frameKeepBackground"));
    keepBg_->setToolTip(tr("Write each cell's glyph and foreground and leave the background already there"));
    preview_ = new QLabel;
    preview_->setObjectName(QStringLiteral("frameStylePreview"));
    preview_->setAlignment(Qt::AlignCenter);
    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("frameStyleProblem"));
    problem_->setWordWrap(true);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* across = new QHBoxLayout;
    across->addWidget(new QLabel(tr("left")));
    across->addWidget(left_);
    across->addWidget(new QLabel(tr("right")));
    across->addWidget(right_);
    auto* down = new QHBoxLayout;
    down->addWidget(new QLabel(tr("top")));
    down->addWidget(top_);
    down->addWidget(new QLabel(tr("bottom")));
    down->addWidget(bottom_);
    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name_);
    form->addRow(tr("Corner columns:"), across);
    form->addRow(tr("Corner rows:"), down);
    form->addRow(QString(), fill_);
    form->addRow(QString(), keepBg_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(preview_);
    layout->addWidget(problem_);
    layout->addWidget(buttons_);

    connect(name_, &QLineEdit::textChanged, this, &FrameStyleDialog::check);
    for (QSpinBox* s : {left_, right_, top_, bottom_})
        connect(s, &QSpinBox::valueChanged, this, &FrameStyleDialog::check);
    connect(fill_, &QCheckBox::toggled, this, &FrameStyleDialog::check);
    connect(keepBg_, &QCheckBox::toggled, this, &FrameStyleDialog::check);
    check();
}

QString FrameStyleDialog::name() const { return name_->text().trimmed(); }
int FrameStyleDialog::left() const { return left_->value(); }
int FrameStyleDialog::right() const { return right_->value(); }
int FrameStyleDialog::top() const { return top_->value(); }
int FrameStyleDialog::bottom() const { return bottom_->value(); }
bool FrameStyleDialog::fill() const { return fill_->isChecked(); }
bool FrameStyleDialog::keepBackground() const { return keepBg_->isChecked(); }

std::optional<FrameStyle> FrameStyleDialog::style() const {
    return frameStyleFromCells(cells_, std::string(), name().toStdString(), left(), right(), top(), bottom(), fill(),
                               keepBackground());
}

QString FrameStyleDialog::problem() const {
    std::string why;
    if (!frameStyleFromCells(cells_, std::string(), std::string(), left(), right(), top(), bottom(), fill(),
                             keepBackground(), &why))
        return QString::fromStdString(why);
    if (name().isEmpty())
        return tr("Name the frame style");
    return {};
}

void FrameStyleDialog::check() {
    const QString why = problem();
    problem_->setText(why);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(why.isEmpty());
    // The style drawn twice the selection's size (the repeat shows), at
    // most 40 x 16 past its corners, fitted to 320 x 256 pixels.
    QImage drawn;
    if (const std::optional<FrameStyle> s = style())
        drawn = frameStyleIcon(*s, std::min(2 * cells_.width(), 40), std::min(2 * cells_.height(), 16),
                               QSize(320, 256));
    preview_->setPixmap(drawn.isNull() ? QPixmap() : QPixmap::fromImage(drawn));
}

} // namespace bw
