#include "palette_dialogs.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace bw {

namespace {

QColor toQColor(Rgb c) { return QColor(c.r, c.g, c.b); }

QIcon swatchIcon(Rgb c) {
    QPixmap pm(16, 16);
    pm.fill(toQColor(c));
    return QIcon(pm);
}

} // namespace

PaletteEditDialog::PaletteEditDialog(const Palette& palette, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("paletteEditDialog"));
    setWindowTitle(tr("Edit Palette"));
    auto* layout = new QVBoxLayout(this);
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    for (int i = 0; i < 16; ++i) {
        auto* b = new QPushButton;
        b->setObjectName(QStringLiteral("paletteEntry%1").arg(i));
        b->setMinimumSize(44, 32);
        b->setAutoDefault(false);
        connect(b, &QPushButton::clicked, this, [this, i] { pick(i); });
        swatches_[std::size_t(i)] = b;
        grid->addWidget(b, i / 8, i % 8);
    }
    auto* note = new QLabel(tr("Click a colour to change it. Each is kept as an XBIN holds it, 64 levels a channel."));
    note->setWordWrap(true);
    layout->addWidget(note);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons_);
    setPalette(palette);
}

void PaletteEditDialog::setColour(int index, Rgb colour) {
    if (index < 0 || index > 15)
        return;
    palette_[std::size_t(index)] = xbinColour(colour);
    showEntry(index);
}

void PaletteEditDialog::setPalette(const Palette& palette) {
    for (int i = 0; i < 16; ++i)
        setColour(i, palette[std::size_t(i)]);
}

void PaletteEditDialog::showEntry(int index) {
    const Rgb c = palette_[std::size_t(index)];
    QPushButton* b = swatches_[std::size_t(index)];
    const QColor q = toQColor(c);
    b->setText(QString::number(index));
    b->setToolTip(tr("Colour %1: %2").arg(index).arg(q.name()));
    b->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                         .arg(q.name(), q.lightness() > 110 ? QStringLiteral("black") : QStringLiteral("white")));
}

void PaletteEditDialog::pick(int index) {
    const QColor was = toQColor(palette_[std::size_t(index)]);
    const QColor c = QColorDialog::getColor(was, this, tr("Colour %1").arg(index));
    if (c.isValid())
        setColour(index, Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())});
}

RemapDialog::RemapDialog(const Palette& palette, bool hasSelection, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("remapDialog"));
    setWindowTitle(tr("Remap Colours"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Each colour index becomes the one beside it, in the foreground and the background. "
                                "The palette stays; cells in RGB colours stay."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    for (int i = 0; i < 16; ++i) {
        auto* from = new QLabel;
        from->setPixmap(swatchIcon(palette[std::size_t(i)]).pixmap(16, 16));
        auto* name = new QLabel(QString::number(i));
        auto* to = new QComboBox;
        to->setObjectName(QStringLiteral("remapTarget%1").arg(i));
        for (int j = 0; j < 16; ++j)
            to->addItem(swatchIcon(palette[std::size_t(j)]), QString::number(j));
        to->setCurrentIndex(i);
        connect(to, &QComboBox::currentIndexChanged, this, &RemapDialog::syncText);
        targets_[std::size_t(i)] = to;
        const int row = i % 8, col = (i / 8) * 4;
        grid->addWidget(from, row, col);
        grid->addWidget(name, row, col + 1);
        grid->addWidget(new QLabel(QStringLiteral("→")), row, col + 2);
        grid->addWidget(to, row, col + 3);
    }
    auto* form = new QFormLayout;
    text_ = new QLineEdit;
    text_->setObjectName(QStringLiteral("remapText"));
    text_->setPlaceholderText(tr("3=11,11=3"));
    text_->setToolTip(tr("FROM=TO pairs separated by commas, or 16 hex digits, the one at n what n becomes"));
    form->addRow(tr("&Map:"), text_);
    layout->addLayout(form);
    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("remapProblem"));
    layout->addWidget(problem_);
    connect(text_, &QLineEdit::textEdited, this, [this](const QString& t) {
        std::string error;
        const auto m = t.trimmed().isEmpty() ? std::optional(identityColourMap())
                                             : parseColourMap(t.toStdString(), &error);
        problem_->setText(m ? QString() : QString::fromStdString(error));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(m.has_value());
        if (!m)
            return;
        syncing_ = true;
        for (int i = 0; i < 16; ++i)
            targets_[std::size_t(i)]->setCurrentIndex((*m)[std::size_t(i)]);
        syncing_ = false;
    });

    auto* box = new QGroupBox(tr("Apply to"));
    auto* boxLayout = new QVBoxLayout(box);
    selection_ = new QRadioButton(tr("The &selection"));
    selection_->setObjectName(QStringLiteral("remapSelection"));
    selection_->setEnabled(hasSelection);
    layer_ = new QRadioButton(tr("The active &layer"));
    layer_->setObjectName(QStringLiteral("remapLayer"));
    document_ = new QRadioButton(tr("&Every layer"));
    document_->setObjectName(QStringLiteral("remapDocument"));
    document_->setToolTip(tr("Every cells, pixels and text layer; live text and generator layers draw their own"));
    for (QRadioButton* r : {selection_, layer_, document_})
        boxLayout->addWidget(r);
    (hasSelection ? selection_ : layer_)->setChecked(true);
    layout->addWidget(box);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons_);
}

ColourMap RemapDialog::map() const {
    ColourMap m{};
    for (int i = 0; i < 16; ++i)
        m[std::size_t(i)] = std::uint8_t(targets_[std::size_t(i)]->currentIndex());
    return m;
}

void RemapDialog::setMap(const ColourMap& map) {
    for (int i = 0; i < 16; ++i)
        targets_[std::size_t(i)]->setCurrentIndex(map[std::size_t(i)] & 15);
}

void RemapDialog::syncText() {
    if (syncing_)
        return;
    text_->setText(QString::fromStdString(colourMapText(map())));
    problem_->clear();
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);
}

Document::RemapScope RemapDialog::scope() const {
    if (selection_->isChecked())
        return Document::RemapScope::Selection;
    return document_->isChecked() ? Document::RemapScope::Document : Document::RemapScope::Layer;
}

void RemapDialog::setScope(Document::RemapScope scope) {
    (scope == Document::RemapScope::Selection ? selection_
     : scope == Document::RemapScope::Document ? document_
                                               : layer_)
        ->setChecked(true);
}

} // namespace bw
