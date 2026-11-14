#include "micron_colour_dialog.h"

#include "blockwright/micron.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace bw {

int MicronColourDialog::nearestDigit(int v) { return std::clamp((std::clamp(v, 0, 255) * 2 + 17) / 34, 0, 15); }

MicronColourDialog::MicronColourDialog(Rgb initial, const QString& title, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("micronColourDialog"));
    setWindowTitle(title);
    auto* grid = new QGridLayout;
    static const char* const names[] = {"red", "green", "blue"};
    const QString labels[] = {tr("&Red"), tr("&Green"), tr("&Blue")};
    for (int i = 0; i < 3; ++i) {
        auto* label = new QLabel(labels[i]);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setObjectName(QString::fromLatin1(names[i]) + QStringLiteral("Slider"));
        slider->setRange(0, 15);
        slider->setPageStep(4);
        auto* spin = new QSpinBox;
        spin->setObjectName(QString::fromLatin1(names[i]));
        spin->setRange(0, 15);
        spin->setDisplayIntegerBase(16);
        label->setBuddy(spin);
        grid->addWidget(label, i, 0);
        grid->addWidget(slider, i, 1);
        grid->addWidget(spin, i, 2);
        sliders_[std::size_t(i)] = slider;
        spins_[std::size_t(i)] = spin;
        const auto set = [this, i](int v) {
            if (syncing_)
                return;
            digits_[std::size_t(i)] = v;
            sync();
        };
        connect(slider, &QSlider::valueChanged, this, set);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, set);
    }
    auto* codeLabel = new QLabel(tr("&Code:"));
    code_ = new QLineEdit;
    code_->setObjectName(QStringLiteral("code"));
    code_->setMaxLength(3);
    code_->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9a-fA-F]{0,3}")), code_));
    code_->setToolTip(tr("Three hex digits, as `F and `B write them"));
    codeLabel->setBuddy(code_);
    connect(code_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (text.size() != 3)
            return;
        for (int i = 0; i < 3; ++i)
            digits_[std::size_t(i)] = QString(text[i]).toInt(nullptr, 16);
        sync();
    });
    grid->addWidget(codeLabel, 3, 0);
    grid->addWidget(code_, 3, 1, 1, 2);
    swatch_ = new QLabel;
    swatch_->setObjectName(QStringLiteral("swatch"));
    swatch_->setMinimumSize(64, 48);
    swatch_->setAutoFillBackground(true);
    swatch_->setFrameShape(QFrame::Box);
    grid->addWidget(swatch_, 0, 3, 4, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addWidget(buttons);
    setDigits(nearestDigit(initial.r), nearestDigit(initial.g), nearestDigit(initial.b));
}

Rgb MicronColourDialog::colour() const { return micron::twelveBit(digits_[0], digits_[1], digits_[2]); }

void MicronColourDialog::setDigits(int r, int g, int b) {
    digits_ = {std::clamp(r, 0, 15), std::clamp(g, 0, 15), std::clamp(b, 0, 15)};
    sync();
}

QString MicronColourDialog::code() const { return QString::fromStdString(micron::colourCode(colour())); }

void MicronColourDialog::sync() {
    syncing_ = true;
    for (std::size_t i = 0; i < 3; ++i) {
        sliders_[i]->setValue(digits_[i]);
        spins_[i]->setValue(digits_[i]);
    }
    // a field being typed in keeps what was typed
    if (code_->text().compare(code(), Qt::CaseInsensitive) != 0)
        code_->setText(code());
    const Rgb c = colour();
    QPalette pal = swatch_->palette();
    pal.setColor(QPalette::Window, QColor(c.r, c.g, c.b));
    swatch_->setPalette(pal);
    syncing_ = false;
}

} // namespace bw
