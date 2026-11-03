#include "layer_style_dialog.h"

#include "document.h"
#include "library_dock.h"
#include "lint_dock.h"

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/face.h"
#include "blockwright/irc.h"
#include "blockwright/library.h"
#include "blockwright/palette.h"
#include "blockwright/ramp.h"
#include "blockwright/render.h"
#include "blockwright/text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace bw {

namespace {

constexpr int kKinds = int(TextStyle::kMaxEffects);

EffectSettings defaultSettings(EffectKind kind) {
    switch (kind) {
    case EffectKind::Plate: return PlateEffect{};
    case EffectKind::Glow: return GlowEffect{};
    case EffectKind::Shadow: return ShadowEffect{};
    case EffectKind::Outline: return OutlineEffect{};
    case EffectKind::Fill: return FillEffect{};
    case EffectKind::Shade: return ShadeEffect{};
    case EffectKind::Bevel: return BevelEffect{};
    }
    return PlateEffect{};
}

QString kindLabel(EffectKind kind) {
    switch (kind) {
    case EffectKind::Plate: return LayerStyleDialog::tr("Plate");
    case EffectKind::Glow: return LayerStyleDialog::tr("Glow");
    case EffectKind::Shadow: return LayerStyleDialog::tr("Shadow");
    case EffectKind::Outline: return LayerStyleDialog::tr("Outline");
    case EffectKind::Fill: return LayerStyleDialog::tr("Fill");
    case EffectKind::Shade: return LayerStyleDialog::tr("Shade");
    case EffectKind::Bevel: return LayerStyleDialog::tr("Bevel");
    }
    return {};
}

QIcon swatch(int index) {
    QPixmap pm(16, 16);
    const Rgb c = kPalette[std::size_t(index) & 15];
    pm.fill(QColor(c.r, c.g, c.b));
    return QIcon(pm);
}

QComboBox* colourBox(const QString& name) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    const QString names[16] = {LayerStyleDialog::tr("Black"),         LayerStyleDialog::tr("Blue"),
                               LayerStyleDialog::tr("Green"),         LayerStyleDialog::tr("Cyan"),
                               LayerStyleDialog::tr("Red"),           LayerStyleDialog::tr("Magenta"),
                               LayerStyleDialog::tr("Brown"),         LayerStyleDialog::tr("Grey"),
                               LayerStyleDialog::tr("Dark grey"),     LayerStyleDialog::tr("Light blue"),
                               LayerStyleDialog::tr("Light green"),   LayerStyleDialog::tr("Light cyan"),
                               LayerStyleDialog::tr("Light red"),     LayerStyleDialog::tr("Light magenta"),
                               LayerStyleDialog::tr("Yellow"),        LayerStyleDialog::tr("White")};
    for (int i = 0; i < 16; ++i)
        box->addItem(swatch(i), names[i], i);
    return box;
}

QComboBox* rampBox(const QString& name) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    for (const Ramp& r : namedRamps()) {
        // the ramp's colours dark to light
        QPixmap pm(8 * int(r.colours.size()), 12);
        QPainter p(&pm);
        for (std::size_t i = 0; i < r.colours.size(); ++i) {
            const Rgb c = kPalette[r.colours[i] & 15];
            p.fillRect(int(i) * 8, 0, 8, 12, QColor(c.r, c.g, c.b));
        }
        p.end();
        box->addItem(QIcon(pm), QString::fromStdString(r.name), QString::fromStdString(r.name));
    }
    return box;
}

QComboBox* compassBox(const QString& name) {
    auto* box = new QComboBox;
    box->setObjectName(name);
    for (int c = int(Compass::N); c <= int(Compass::NW); ++c)
        box->addItem(QString::fromLatin1(compassPointName(Compass(c))).toUpper(), c);
    return box;
}

QSpinBox* spin(const QString& name, int lo, int hi) {
    auto* box = new QSpinBox;
    box->setObjectName(name);
    box->setRange(lo, hi);
    return box;
}

void pick(QComboBox* box, const QVariant& data) {
    int i = box->findData(data);
    if (i < 0 && data.typeId() == QMetaType::QString) {
        // a ramp this build doesn't name: listed so the style shows as it is
        box->addItem(data.toString(), data);
        i = box->count() - 1;
    }
    box->setCurrentIndex(i);
}

// The cells as an export writes them: transparent ones blank, the others as
// they show with iCE colours `ice` (iceCell), as the canvas draws them and
// the IRC meter measures them. The lint reads the cells before this, as the
// Lint dock reads the canvas (dockLint).
Canvas shown(Canvas c, bool ice) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            Cell& k = c.at(x, y);
            k = k.transparent ? Cell{} : iceCell(k, ice);
        }
    return c;
}

} // namespace

std::vector<Effect> styleRows(const std::optional<TextStyle>& style) {
    std::vector<Effect> rows;
    for (int k = 0; k < kKinds; ++k)
        rows.push_back(Effect{false, defaultSettings(EffectKind(k))});
    if (style)
        for (const Effect& e : style->effects)
            if (std::size_t(e.kind()) < rows.size())
                rows[std::size_t(e.kind())] = e;
    return rows;
}

std::optional<TextStyle> styleFromRows(const std::optional<TextStyle>& original, const std::vector<Effect>& rows) {
    TextStyle out;
    std::vector<bool> placed(rows.size());
    const auto rowOf = [&rows](EffectKind kind) -> const Effect* {
        for (const Effect& r : rows)
            if (r.kind() == kind)
                return &r;
        return nullptr;
    };
    if (original)
        for (const Effect& e : original->effects)
            if (const Effect* r = rowOf(e.kind())) {
                out.effects.push_back(*r);
                placed[std::size_t(r - rows.data())] = true;
            }
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (!placed[i] && (rows[i].enabled || rows[i].settings != defaultSettings(rows[i].kind())))
            out.effects.push_back(rows[i]);
    if (out.effects.empty() && !original)
        return std::nullopt;
    return out;
}

LayerStyleDialog::LayerStyleDialog(Document* doc, LayerId id, QWidget* parent)
    : QDialog(parent), doc_(doc), id_(id) {
    setWindowTitle(tr("Layer Style"));
    setObjectName(QStringLiteral("layerStyleDialog"));
    if (const Layer* l = doc_->layers().find(id_))
        original_ = l->props.style;
    order_ = original_;
    rows_ = styleRows(original_);

    presetBox_ = new QComboBox;
    presetBox_->setObjectName(QStringLiteral("stylePresets"));
    presetBox_->setToolTip(tr("Start from a text style in the Library: its effects replace these"));
    presetBox_->addItem(tr("Choose…"));
    connect(presetBox_, &QComboBox::activated, this, [this](int row) {
        if (row > 0)
            usePreset(row - 1);
        const QSignalBlocker block(presetBox_);
        presetBox_->setCurrentIndex(0);
    });

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("effectList"));
    list_->setToolTip(tr("The effects, in the order they draw; the box switches one on or off"));
    pages_ = new QStackedWidget;
    pages_->setObjectName(QStringLiteral("effectPages"));
    for (int k = 0; k < kKinds; ++k) {
        auto* item = new QListWidgetItem(kindLabel(EffectKind(k)), list_);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        pages_->addWidget(makePage(EffectKind(k)));
    }
    for (int k = 0; k < kKinds; ++k)
        fillRow(k);

    preview_ = new QLabel;
    preview_->setObjectName(QStringLiteral("stylePreview"));
    preview_->setAlignment(Qt::AlignCenter);
    // The preview stays 1×; a layer bigger than the room it gets (QScrollArea
    // caps its size hint near 36 × 24 lines of text) scrolls instead.
    previewArea_ = new QScrollArea;
    previewArea_->setObjectName(QStringLiteral("stylePreviewArea"));
    previewArea_->setWidget(preview_);
    previewArea_->setWidgetResizable(true);
    previewArea_->setMinimumSize(160, 64);
    irc_ = new QLabel;
    irc_->setObjectName(QStringLiteral("styleIrcBytes"));
    confusable_ = new QLabel;
    confusable_->setObjectName(QStringLiteral("styleConfusables"));
    confusable_->setWordWrap(true);
    lint_ = new QPlainTextEdit;
    lint_->setObjectName(QStringLiteral("styleLint"));
    lint_->setReadOnly(true);
    lint_->setMaximumBlockCount(200);
    lint_->setFixedHeight(90);
    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("styleProblem"));
    problem_->setWordWrap(true);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    preset_ = buttons_->addButton(tr("Save as &Preset…"), QDialogButtonBox::ActionRole);
    preset_->setObjectName(QStringLiteral("saveStylePreset"));
    preset_->setToolTip(tr("Save the style to the user library as a text style"));
    preset_->setEnabled(false);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (problem().isEmpty()) {
            apply();
            accept();
        }
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(preset_, &QPushButton::clicked, this, &LayerStyleDialog::askPreset);

    connect(list_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (filling_)
            return;
        const int row = list_->row(item);
        rows_[std::size_t(row)].enabled = item->checkState() == Qt::Checked;
        changed();
    });

    auto* presetRow = new QFormLayout;
    presetRow->addRow(tr("P&reset:"), presetBox_);
    auto* top = new QHBoxLayout;
    top->addWidget(list_, 1);
    top->addWidget(pages_, 2);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(presetRow);
    layout->addLayout(top);
    layout->addWidget(previewArea_, 1);
    layout->addWidget(irc_);
    layout->addWidget(confusable_);
    layout->addWidget(lint_);
    layout->addWidget(problem_);
    layout->addWidget(buttons_);

    // the first effect the layer has on, else the first
    int first = 0;
    for (int k = 0; k < kKinds; ++k)
        if (rows_[std::size_t(k)].enabled) {
            first = k;
            break;
        }
    list_->setCurrentRow(first);
    refresh();
}

QWidget* LayerStyleDialog::makePage(EffectKind kind) {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    Page& p = page_[std::size_t(kind)];
    const QString base = QString::fromLatin1(effectKindName(kind));
    switch (kind) {
    case EffectKind::Plate:
        p.colour = colourBox(base + QStringLiteral("Colour"));
        p.amount = spin(base + QStringLiteral("Padding"), 0, TextStyle::kMaxPadding);
        form->addRow(tr("Colour"), p.colour);
        form->addRow(tr("Padding (cells)"), p.amount);
        break;
    case EffectKind::Glow:
        p.ramp = rampBox(base + QStringLiteral("Ramp"));
        p.amount = spin(base + QStringLiteral("Width"), 1, TextStyle::kMaxGlow);
        p.level = spin(base + QStringLiteral("Brightness"), 1, TextStyle::kMaxGlow);
        p.level->setToolTip(tr("The ramp's rung the ring next to the letters takes; each ring out is a rung darker"));
        form->addRow(tr("Ramp"), p.ramp);
        form->addRow(tr("Width (cells)"), p.amount);
        form->addRow(tr("Brightness (rung)"), p.level);
        break;
    case EffectKind::Shadow:
        p.form = new QComboBox;
        p.form->setObjectName(base + QStringLiteral("Form"));
        p.form->addItem(tr("Drop"), int(ShadowForm::Drop));
        p.form->addItem(tr("Extrude"), int(ShadowForm::Extrude));
        p.compass = compassBox(base + QStringLiteral("Direction"));
        p.amount = spin(base + QStringLiteral("Distance"), 1, TextStyle::kMaxDistance);
        p.colour = colourBox(base + QStringLiteral("Colour"));
        form->addRow(tr("Form"), p.form);
        form->addRow(tr("Direction"), p.compass);
        form->addRow(tr("Distance (pixels)"), p.amount);
        form->addRow(tr("Colour"), p.colour);
        break;
    case EffectKind::Outline:
        p.form = new QComboBox;
        p.form->setObjectName(base + QStringLiteral("Form"));
        p.form->addItem(tr("Pixels"), int(OutlineForm::Pixels));
        p.form->addItem(tr("Cells"), int(OutlineForm::Cells));
        p.colour = colourBox(base + QStringLiteral("Colour"));
        form->addRow(tr("Form"), p.form);
        form->addRow(tr("Colour"), p.colour);
        break;
    case EffectKind::Fill:
        p.form = new QComboBox;
        p.form->setObjectName(base + QStringLiteral("Mode"));
        p.form->addItem(tr("Flat"), int(FillMode::Flat));
        p.form->addItem(tr("Rows"), int(FillMode::Rows));
        p.form->addItem(tr("Columns"), int(FillMode::Columns));
        p.form->addItem(tr("Rainbow"), int(FillMode::Rainbow));
        p.colour = colourBox(base + QStringLiteral("Colour"));
        p.ramp = rampBox(base + QStringLiteral("Ramp"));
        form->addRow(tr("Mode"), p.form);
        form->addRow(tr("Colour (flat)"), p.colour);
        form->addRow(tr("Ramp (the rest)"), p.ramp);
        break;
    case EffectKind::Shade:
    case EffectKind::Bevel:
        p.ramp = rampBox(base + QStringLiteral("Ramp"));
        p.compass = compassBox(base + QStringLiteral("Light"));
        form->addRow(tr("Ramp"), p.ramp);
        form->addRow(tr("Light from"), p.compass);
        break;
    }
    for (QComboBox* box : {p.colour, p.ramp, p.compass, p.form})
        if (box)
            connect(box, &QComboBox::currentIndexChanged, this, [this, kind] {
                if (!filling_) {
                    readPage(kind);
                    changed();
                }
            });
    for (QSpinBox* box : {p.amount, p.level})
        if (box)
            connect(box, &QSpinBox::valueChanged, this, [this, kind] {
                if (!filling_) {
                    readPage(kind);
                    changed();
                }
            });
    return page;
}

void LayerStyleDialog::fillPage(EffectKind kind) {
    const QScopedValueRollback<int> guard(filling_, filling_ + 1);
    Page& p = page_[std::size_t(kind)];
    std::visit(
        [&p](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, PlateEffect>) {
                pick(p.colour, int(s.colour));
                p.amount->setValue(s.padding);
            } else if constexpr (std::is_same_v<T, GlowEffect>) {
                pick(p.ramp, QString::fromStdString(s.ramp));
                p.amount->setValue(s.width);
                p.level->setValue(s.brightness);
            } else if constexpr (std::is_same_v<T, ShadowEffect>) {
                pick(p.form, int(s.form));
                pick(p.compass, int(s.direction));
                p.amount->setValue(s.distance);
                pick(p.colour, int(s.colour));
            } else if constexpr (std::is_same_v<T, OutlineEffect>) {
                pick(p.form, int(s.form));
                pick(p.colour, int(s.colour));
            } else if constexpr (std::is_same_v<T, FillEffect>) {
                pick(p.form, int(s.mode));
                pick(p.colour, int(s.colour));
                pick(p.ramp, QString::fromStdString(s.ramp));
                p.colour->setEnabled(s.mode == FillMode::Flat);
                p.ramp->setEnabled(s.mode != FillMode::Flat);
            } else {
                pick(p.ramp, QString::fromStdString(s.ramp));
                pick(p.compass, int(s.light));
            }
        },
        rows_[std::size_t(kind)].settings);
}

void LayerStyleDialog::readPage(EffectKind kind) {
    const Page& p = page_[std::size_t(kind)];
    const auto colour = [](QComboBox* box) { return std::uint8_t(box->currentData().toInt()); };
    const auto ramp = [](QComboBox* box) { return box->currentData().toString().toStdString(); };
    const auto compass = [](QComboBox* box) { return Compass(box->currentData().toInt()); };
    std::visit(
        [&](auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, PlateEffect>) {
                s.colour = colour(p.colour);
                s.padding = p.amount->value();
            } else if constexpr (std::is_same_v<T, GlowEffect>) {
                s.ramp = ramp(p.ramp);
                s.width = p.amount->value();
                s.brightness = p.level->value();
            } else if constexpr (std::is_same_v<T, ShadowEffect>) {
                s.form = ShadowForm(p.form->currentData().toInt());
                s.direction = compass(p.compass);
                s.distance = p.amount->value();
                s.colour = colour(p.colour);
            } else if constexpr (std::is_same_v<T, OutlineEffect>) {
                s.form = OutlineForm(p.form->currentData().toInt());
                s.colour = colour(p.colour);
            } else if constexpr (std::is_same_v<T, FillEffect>) {
                s.mode = FillMode(p.form->currentData().toInt());
                s.colour = colour(p.colour);
                s.ramp = ramp(p.ramp);
            } else {
                s.ramp = ramp(p.ramp);
                s.light = compass(p.compass);
            }
        },
        rows_[std::size_t(kind)].settings);
    if (kind == EffectKind::Fill) {
        const bool flat = std::get<FillEffect>(rows_[std::size_t(kind)].settings).mode == FillMode::Flat;
        p.colour->setEnabled(flat);
        p.ramp->setEnabled(!flat);
    }
    // changing an effect's settings switches it on
    if (!rows_[std::size_t(kind)].enabled) {
        rows_[std::size_t(kind)].enabled = true;
        fillRow(int(kind));
    }
}

void LayerStyleDialog::fillRow(int row) {
    const QScopedValueRollback<int> guard(filling_, filling_ + 1);
    list_->item(row)->setCheckState(rows_[std::size_t(row)].enabled ? Qt::Checked : Qt::Unchecked);
    fillPage(EffectKind(row));
}

void LayerStyleDialog::setRow(const Effect& effect) {
    const int row = int(effect.kind());
    rows_[std::size_t(row)] = effect;
    fillRow(row);
    changed();
}

std::optional<TextStyle> LayerStyleDialog::style() const { return styleFromRows(order_, rows_); }

void LayerStyleDialog::setPresets(std::vector<library::TextStyleEntry> presets) {
    presets_ = std::move(presets);
    const bool ice = doc_->ansi().ice;
    const QSignalBlocker block(presetBox_);
    while (presetBox_->count() > 1)
        presetBox_->removeItem(1);
    auto* model = qobject_cast<QStandardItemModel*>(presetBox_->model());
    for (const library::TextStyleEntry& p : presets_) {
        presetBox_->addItem(QString::fromStdString(p.name));
        QStandardItem* item = model->item(presetBox_->count() - 1);
        if (p.iceRequired && !ice) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(tr("Needs iCE colours, which are off: turn on View > iCE Colours to use it"));
        } else {
            item->setToolTip(p.iceRequired ? tr("Needs iCE colours") : tr("Works with iCE colours on or off"));
        }
    }
    presetBox_->setCurrentIndex(0);
}

bool LayerStyleDialog::usePreset(int index) {
    if (index < 0 || std::size_t(index) >= presets_.size())
        return false;
    const library::TextStyleEntry& p = presets_[std::size_t(index)];
    if (p.iceRequired && !doc_->ansi().ice)
        return false;
    order_ = p.style;
    rows_ = styleRows(order_);
    for (int k = 0; k < kKinds; ++k)
        fillRow(k);
    for (int k = 0; k < kKinds; ++k)
        if (rows_[std::size_t(k)].enabled) {
            list_->setCurrentRow(k);
            break;
        }
    changed();
    return true;
}

void LayerStyleDialog::changed() {
    stale_ = true;
    refresh();
}

void LayerStyleDialog::refresh() {
    if (!stale_)
        return;
    stale_ = false;
    cells_ = Canvas(0, 0);
    widest_ = 0;
    report_ = {};
    confusables_.clear();
    problemText_.clear();
    const Layer* l = doc_->layers().find(id_);
    const bool ice = doc_->ansi().ice;
    Canvas raw(0, 0);  // the composite as the document stores it: what the lint reads
    if (!l) {
        problemText_ = tr("The layer is gone.");
    } else {
        LayerProperties props = l->props;
        props.style = style();
        const FontSources fonts = doc_->fontSources();
        if (const auto why = layerStyleProblem(props, fonts)) {
            problemText_ = QString::fromStdString(*why);
        } else {
            try {
                // the layer alone, at (0, 0), as the compositor shows it
                Layer copy = *l;
                applyLayerProperties(copy, props, fonts, ice);
                copy.props.x = copy.props.y = 0;
                copy.props.visible = true;
                copy.props.mode = LayerMode::Normal;
                if (copy.width() > 0 && copy.height() > 0) {
                    LayerStack stack(copy.width(), copy.height());
                    stack.add(std::move(copy));
                    raw = composite(stack, {ice, Cell{}});
                    cells_ = shown(raw, ice);
                }
            } catch (const std::exception& e) {
                problemText_ = QString::fromStdString(e.what());
                cells_ = raw = Canvas(0, 0);
            }
            if (props.text)
                if (const Face* face = findFace(props.text->face, fonts)) {
                    QStringList pairs;
                    for (const ConfusablePair& p : confusablesIn(*face, casedText(*props.text)))
                        pairs << QString::fromStdU32String(std::u32string{p.a, p.b});
                    confusables_ = pairs.join(QLatin1Char(' '));
                }
        }
    }
    if (cells_.width() > 0 && cells_.height() > 0) {
        for (const std::string& line : irc::encodeCanvas(cells_))
            widest_ = std::max(widest_, line.size());
        report_ = dockLint(raw, ice);
    }

    // the widgets
    const std::size_t budget = doc_->extras().ircBudget.bytes;
    if (cells_.width() > 0 && cells_.height() > 0) {
        preview_->setPixmap(QPixmap::fromImage(renderCanvas(cells_, 8)));
        irc_->setText(widest_ > budget ? tr("IRC: widest line %1 bytes, over the %2-byte budget").arg(widest_).arg(budget)
                                       : tr("IRC: widest line %1 of %2 bytes").arg(widest_).arg(budget));
    } else {
        preview_->setPixmap(QPixmap());
        preview_->setText(tr("Nothing to show"));
        irc_->setText(tr("IRC: nothing to send"));
    }
    confusable_->setText(confusables_.isEmpty() ? QString()
                                                : tr("These characters look alike in the face: %1").arg(confusables_));
    confusable_->setVisible(!confusables_.isEmpty());
    QStringList lint{QString::fromStdString(lint::summary(report_))};
    for (const lint::Finding& f : report_.findings)
        lint << tr("%1,%2  %3").arg(f.x).arg(f.y).arg(QString::fromStdString(f.message));
    lint_->setPlainText(lint.join(QLatin1Char('\n')));
    problem_->setText(problemText_);
    problem_->setVisible(!problemText_.isEmpty());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(problemText_.isEmpty());
    preset_->setEnabled(!presetDir_.empty() && problemText_.isEmpty() && style());
}

QString LayerStyleDialog::problem() {
    refresh();
    return problemText_;
}

const Canvas& LayerStyleDialog::previewCells() {
    refresh();
    return cells_;
}

std::size_t LayerStyleDialog::widestLineBytes() {
    refresh();
    return widest_;
}

const lint::LintReport& LayerStyleDialog::lintReport() {
    refresh();
    return report_;
}

QString LayerStyleDialog::confusables() {
    refresh();
    return confusables_;
}

bool LayerStyleDialog::apply() {
    const Layer* l = doc_->layers().find(id_);
    if (!l)
        return false;
    LayerProperties props = l->props;
    props.style = style();
    if (props.style == l->props.style)
        return false;
    return doc_->setLayerProperties(id_, std::move(props), tr("Layer Style"));
}

void LayerStyleDialog::setPresetDir(std::filesystem::path dir, QStringList categories) {
    presetDir_ = std::move(dir);
    categories_ = std::move(categories);
    stale_ = true;
    refresh();
}

QString LayerStyleDialog::savePreset(const QString& name, const QString& category, const QStringList& tags,
                                     QString* error) {
    const std::optional<TextStyle> s = style();
    if (presetDir_.empty()) {
        *error = tr("there is no user library directory");
        return {};
    }
    if (!s) {
        *error = tr("the style has no effects");
        return {};
    }
    library::TextStyleEntry entry;
    entry.name = name.trimmed().toStdString();
    entry.category = category.trimmed().toStdString();
    for (const QString& t : tags)
        entry.tags.push_back(t.trimmed().toStdString());
    entry.style = *s;
    entry.iceRequired = styleNeedsIce(*s);
    std::filesystem::path written;
    try {
        written = library::writeTextStyle(presetDir_, entry);
    } catch (const std::exception& e) {
        *error = QString::fromStdString(e.what());
        return {};
    }
    const QString path = QString::fromStdString(written.string());
    emit presetSaved(path);
    return path;
}

void LayerStyleDialog::askPreset() {
    StampDialog dialog(categories_, this, StampDialog::Purpose::Preset);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (savePreset(dialog.name(), dialog.category(), dialog.tags(), &error).isEmpty())
        QMessageBox::warning(this, tr("Save as Preset"), tr("Could not save the preset:\n%1").arg(error));
}

} // namespace bw
