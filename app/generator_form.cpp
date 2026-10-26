#include "generator_form.h"

#include "document.h"
#include "tool_options.h"

#include "blockwright/palette.h"
#include "blockwright/ramp.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace bw {

namespace {

using generator::Param;
using generator::ParamType;
using generator::ParamValue;

QString colourName(int i) {
    static const char* const names[16] = {
        QT_TRANSLATE_NOOP("GeneratorForm", "Black"),       QT_TRANSLATE_NOOP("GeneratorForm", "Blue"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Green"),       QT_TRANSLATE_NOOP("GeneratorForm", "Cyan"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Red"),         QT_TRANSLATE_NOOP("GeneratorForm", "Magenta"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Brown"),       QT_TRANSLATE_NOOP("GeneratorForm", "Grey"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Dark Grey"),   QT_TRANSLATE_NOOP("GeneratorForm", "Light Blue"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Light Green"), QT_TRANSLATE_NOOP("GeneratorForm", "Light Cyan"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Light Red"),   QT_TRANSLATE_NOOP("GeneratorForm", "Light Magenta"),
        QT_TRANSLATE_NOOP("GeneratorForm", "Yellow"),      QT_TRANSLATE_NOOP("GeneratorForm", "White"),
    };
    return QStringLiteral("%1 %2").arg(i).arg(GeneratorForm::tr(names[i & 15]));
}

QIcon swatch(int i) {
    QPixmap pm(16, 12);
    const Rgb c = kPalette[std::size_t(i & 15)];
    pm.fill(QColor(c.r, c.g, c.b));
    return QIcon(pm);
}

// The fewest decimals (up to 6, all a number param has) that show `d` as
// it is: written with them and read back unchanged, as QDoubleSpinBox
// rounds.
int decimals(double d) {
    for (int n = 0; n < 6; ++n)
        if (QString::number(d, 'f', n).toDouble() == d)
            return n;
    return 6;
}

int clampInt(double d) {
    return int(std::clamp(d, double(std::numeric_limits<int>::min()), double(std::numeric_limits<int>::max())));
}

} // namespace

GeneratorForm::GeneratorForm(Document* doc, QWidget* parent)
    : QWidget(parent), doc_(doc), fields_(new QWidget), form_(new QFormLayout(fields_)), message_(new QLabel),
      run_(new QPushButton(tr("&Run"))) {
    setObjectName(QStringLiteral("generatorForm"));
    form_->setContentsMargins(0, 0, 0, 0);
    message_->setObjectName(QStringLiteral("generatorMessage"));
    message_->setWordWrap(true);
    message_->hide();
    run_->setObjectName(QStringLiteral("runGenerator"));
    run_->setToolTip(tr("Run the generator again with these parameters"));
    connect(run_, &QPushButton::clicked, this, [this] { doc_->requestGeneratorRun(id_); });
    auto* title = new QLabel(tr("<b>Generator</b>"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->addWidget(title);
    layout->addWidget(fields_);
    layout->addWidget(message_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(run_);
    layout->addLayout(buttons);

    // another document: nothing the fields show or hold is its
    connect(doc_, &Document::reset, this, [this] {
        id_ = 0;
        specText_.reset();
        spec_.reset();
        shown_.clear();
    });
    connect(doc_, &Document::layersChanged, this, &GeneratorForm::sync);
    connect(doc_, &Document::activeLayerChanged, this, &GeneratorForm::sync);
    connect(doc_, &Document::generatorChanged, this, [this](LayerId id) {
        if (id == id_)
            showValues();
    });
    sync();
}

QWidget* GeneratorForm::field(const std::string& name) const {
    const auto it = widgets_.find(name);
    return it == widgets_.end() ? nullptr : it->second;
}

void GeneratorForm::sync() {
    const Layer* l = doc_->active();
    if (!l || l->props.kind != LayerKind::Generator) {
        id_ = 0;
        specText_.reset();
        spec_.reset();
        hide();
        return;
    }
    const std::optional<std::string> text =
        doc_->keptFile(generator::directory(l->props.id) + std::string(generator::kSpecFile));
    if (l->props.id != id_ || text != specText_ || !text) {
        id_ = l->props.id;
        specText_ = text;
        QString error;
        spec_ = doc_->generatorSpec(id_, &error);
        rebuild(spec_ ? *spec_ : generator::Spec{});
        message_->setText(error);
        message_->setVisible(!spec_);
        run_->setEnabled(bool(spec_));
    }
    showValues();
    show();
}

void GeneratorForm::rebuild(const generator::Spec& spec) {
    while (form_->rowCount() > 0)
        form_->removeRow(0);
    widgets_.clear();
    shown_.clear();
    for (const Param& p : spec.params) {
        const std::string name = p.name;
        QWidget* w = nullptr;
        switch (p.type) {
        case ParamType::Integer: {
            auto* spin = new QSpinBox;
            spin->setKeyboardTracking(false);
            spin->setRange(clampInt(p.min.value_or(std::numeric_limits<int>::min())),
                           clampInt(p.max.value_or(std::numeric_limits<int>::max())));
            if (p.min && p.max) {
                // a slider with its value beside it
                auto* slider = new QSlider(Qt::Horizontal);
                slider->setTracking(false);
                slider->setRange(spin->minimum(), spin->maximum());
                auto* box = new QWidget;
                auto* row = new QHBoxLayout(box);
                row->setContentsMargins(0, 0, 0, 0);
                row->addWidget(slider, 1);
                row->addWidget(spin);
                connect(slider, &QSlider::valueChanged, this, [this, name, spin](int v) {
                    const QSignalBlocker block(spin);
                    spin->setValue(v);
                    commit(name, double(v));
                });
                connect(spin, &QSpinBox::valueChanged, this, [this, name, slider](int v) {
                    const QSignalBlocker block(slider);
                    slider->setValue(v);
                    commit(name, double(v));
                });
                w = box;
            } else {
                connect(spin, &QSpinBox::valueChanged, this, [this, name](int v) { commit(name, double(v)); });
                w = spin;
            }
            break;
        }
        case ParamType::Number: {
            auto* spin = new QDoubleSpinBox;
            spin->setKeyboardTracking(false);
            int places = 3;
            if (p.step)
                places = decimals(*p.step);
            for (const std::optional<double> d : {p.min, p.max, std::optional(std::get<double>(p.defaultValue))})
                if (d)
                    places = std::max(places, decimals(*d));
            spin->setDecimals(places);
            // every value a number param takes (generator::paramProblem)
            spin->setRange(p.min.value_or(-1e12), p.max.value_or(1e12));
            spin->setSingleStep(p.step.value_or(1.0));
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, name](double v) { commit(name, v); });
            w = spin;
            break;
        }
        case ParamType::Boolean: {
            auto* box = new QCheckBox;
            connect(box, &QCheckBox::toggled, this, [this, name](bool on) { commit(name, on); });
            w = box;
            break;
        }
        case ParamType::Colour: {
            auto* box = new QComboBox;
            for (int i = 0; i < 16; ++i)
                box->addItem(swatch(i), colourName(i));
            connect(box, &QComboBox::activated, this, [this, name](int i) { commit(name, double(i)); });
            w = box;
            break;
        }
        case ParamType::Ramp: {
            auto* box = new QComboBox;
            for (const Ramp& r : namedRamps())
                box->addItem(rampIcon(r), QString::fromStdString(r.name), QString::fromStdString(r.name));
            connect(box, &QComboBox::activated, this, [this, name, box](int i) {
                commit(name, box->itemData(i).toString().toStdString());
            });
            w = box;
            break;
        }
        case ParamType::Text: {
            auto* edit = new QLineEdit;
            connect(edit, &QLineEdit::editingFinished, this,
                    [this, name, edit] { commit(name, edit->text().toStdString()); });
            w = edit;
            break;
        }
        }
        w->setObjectName(QStringLiteral("param_") + QString::fromStdString(name));
        w->setToolTip(tr("%1 (%2)").arg(QString::fromStdString(name),
                                        QString::fromStdString(std::string(generator::paramTypeName(p.type)))));
        form_->addRow(QString::fromStdString(p.label.empty() ? p.name : p.label), w);
        widgets_[name] = w;
    }
    fields_->setVisible(!spec.params.empty());
}

void GeneratorForm::showValues() {
    if (!spec_)
        return;
    std::map<std::string, ParamValue> values;
    try {
        values = generator::paramValues(
            *spec_, doc_->keptFile(generator::directory(id_) + std::string(generator::kParamsFile)).value_or(""));
    } catch (const generator::GeneratorError& e) {
        message_->setText(QString::fromStdString(e.what()));
        message_->show();
        for (const Param& p : spec_->params)
            values[p.name] = p.defaultValue;
    }
    for (const Param& p : spec_->params) {
        QWidget* w = widgets_.at(p.name);
        const ParamValue& v = values.at(p.name);
        if (const auto shown = shown_.find(p.name); shown != shown_.end() && shown->second == v)
            continue;
        shown_[p.name] = v;
        switch (p.type) {
        case ParamType::Integer:
            if (auto* spin = qobject_cast<QSpinBox*>(w) ? qobject_cast<QSpinBox*>(w) : w->findChild<QSpinBox*>()) {
                const QSignalBlocker block(spin);
                spin->setValue(clampInt(std::get<double>(v)));
            }
            if (auto* slider = w->findChild<QSlider*>()) {
                const QSignalBlocker block(slider);
                slider->setValue(clampInt(std::get<double>(v)));
            }
            break;
        case ParamType::Number: {
            auto* spin = static_cast<QDoubleSpinBox*>(w);
            const QSignalBlocker block(spin);
            // a stored value finer than the declaration shows as it is
            spin->setDecimals(std::max(spin->decimals(), decimals(std::get<double>(v))));
            spin->setValue(std::get<double>(v));
            break;
        }
        case ParamType::Boolean: {
            auto* box = static_cast<QCheckBox*>(w);
            const QSignalBlocker block(box);
            box->setChecked(std::get<bool>(v));
            break;
        }
        case ParamType::Colour: {
            auto* box = static_cast<QComboBox*>(w);
            const QSignalBlocker block(box);
            box->setCurrentIndex(int(std::get<double>(v)));
            break;
        }
        case ParamType::Ramp: {
            auto* box = static_cast<QComboBox*>(w);
            const QSignalBlocker block(box);
            box->setCurrentIndex(box->findData(QString::fromStdString(std::get<std::string>(v))));
            break;
        }
        case ParamType::Text: {
            auto* edit = static_cast<QLineEdit*>(w);
            const QSignalBlocker block(edit);
            if (edit->text().toStdString() != std::get<std::string>(v))
                edit->setText(QString::fromStdString(std::get<std::string>(v)));
            break;
        }
        }
    }
}

void GeneratorForm::commit(const std::string& name, const ParamValue& value) {
    if (!spec_ || !id_)
        return;
    QString error;
    const std::string params =
        doc_->keptFile(generator::directory(id_) + std::string(generator::kParamsFile)).value_or("");
    try {
        // a field let go without a change (a spin box losing focus) is no step
        if (generator::paramValues(*spec_, params).at(name) == value)
            return;
    } catch (const generator::GeneratorError&) {
    }
    if (!doc_->setGeneratorParam(id_, name, value, &error) && !error.isEmpty()) {
        message_->setText(error);
        message_->show();
        // the refused field shows the value it has again
        shown_.erase(name);
        showValues();
        return;
    }
    message_->hide();
}

} // namespace bw
