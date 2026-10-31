#include "text_options.h"

#include "canvas_view.h"
#include "document.h"
#include "face_picker.h"
#include "flow_row.h"
#include "text_tool.h"

#include "blockwright/big_text.h"
#include "blockwright/palette.h"
#include "blockwright/text.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSpinBox>

namespace bw {

namespace {

QIcon swatch(int index) {
    QPixmap pm(16, 16);
    const Rgb c = kPalette[size_t(index) & 15];
    pm.fill(QColor(c.r, c.g, c.b));
    return QIcon(pm);
}

} // namespace

TextOptions::TextOptions(CanvasView* view, TextTool* tool, QWidget* parent)
    : QWidget(parent), view_(view), tool_(tool) {
    picker_ = new FacePicker;
    picker_->setObjectName(QStringLiteral("textFace"));
    picker_->setToolTip(tr("The face the text is set in"));

    case_ = new QComboBox;
    case_->setObjectName(QStringLiteral("textCase"));
    case_->addItem(tr("As typed"), int(TextCase::AsTyped));
    case_->addItem(tr("Capitals"), int(TextCase::Capitals));
    case_->setToolTip(tr("The text as typed, or in capitals"));

    letterSpacing_ = new QSpinBox;
    letterSpacing_->setObjectName(QStringLiteral("textLetterSpacing"));
    lineSpacing_ = new QSpinBox;
    lineSpacing_->setObjectName(QStringLiteral("textLineSpacing"));
    for (QSpinBox* b : {letterSpacing_, lineSpacing_}) {
        b->setRange(0, TextSource::kMaxSpacing);
        b->setKeyboardTracking(false);
    }
    letterSpacing_->setToolTip(tr("Blank columns between letters, on top of the face's own"));
    lineSpacing_->setToolTip(tr("Blank cell rows between lines"));

    align_ = new QComboBox;
    align_->setObjectName(QStringLiteral("textAlign"));
    align_->addItem(tr("Left"), int(TextAlign::Left));
    align_->addItem(tr("Centre"), int(TextAlign::Centre));
    align_->addItem(tr("Right"), int(TextAlign::Right));
    align_->setToolTip(tr("Where each line sits in the text's box"));

    wrap_ = new QSpinBox;
    wrap_->setObjectName(QStringLiteral("textWrapWidth"));
    wrap_->setRange(0, TextSource::kMaxWrapWidth);
    wrap_->setSpecialValueText(tr("None"));
    wrap_->setSuffix(tr(" columns"));
    wrap_->setKeyboardTracking(false);
    wrap_->setToolTip(tr("The width lines wrap at, in columns"));

    colour_ = new QComboBox;
    colour_->setObjectName(QStringLiteral("textColour"));
    const QString names[16] = {tr("Black"),      tr("Blue"),        tr("Green"),       tr("Cyan"),
                               tr("Red"),        tr("Magenta"),     tr("Brown"),       tr("Grey"),
                               tr("Dark grey"),  tr("Light blue"),  tr("Light green"), tr("Light cyan"),
                               tr("Light red"),  tr("Light magenta"), tr("Yellow"),    tr("White")};
    for (int i = 0; i < 16; ++i)
        colour_->addItem(swatch(i), names[i], i);
    colour_->setToolTip(tr("The letters' colour"));
    // It narrows with the column, its name cut short, rather than make the
    // options scroll sideways: "Light magenta" alone is wider than the rest
    // of a field in some fonts (a Mac's).
    colour_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    colour_->setMinimumContentsLength(8);

    bigStyle_ = new QComboBox;
    bigStyle_->setObjectName(QStringLiteral("textBigStyle"));
    bigStyle_->addItem(tr("None"), int(BigTextStyle::None));
    bigStyle_->addItem(tr("Big shadow"), int(BigTextStyle::Shadow));
    bigStyle_->addItem(tr("Big 3D"), int(BigTextStyle::ThreeD));
    bigStyle_->addItem(tr("Big outline"), int(BigTextStyle::Outline));
    bigStyle_->addItem(tr("Big block"), int(BigTextStyle::Block));
    bigStyle_->addItem(tr("Big rainbow"), int(BigTextStyle::Rainbow));
    bigStyle_->setToolTip(tr("Whole-cell IBM 8×8 lettering, matching /ascii big"));
    bigColours_ = new QLineEdit;
    bigColours_->setObjectName(QStringLiteral("textBigColours"));
    bigColours_->setMaxLength(256);
    // As narrow as a few colours (and the edit's frame and margins) rather
    // than a line edit's usual width, so it fits beside the big style; it
    // takes what the row has to spare.
    bigColours_->setSizePolicy(QSizePolicy::Ignored, bigColours_->sizePolicy().verticalPolicy());
    bigColours_->setMinimumWidth(bigColours_->fontMetrics().horizontalAdvance(QStringLiteral("0,15,4,")) + 12);
    bigColours_->setToolTip(tr("mIRC colours 0–15 or names, separated by commas: face, then shadow or border; "
                               "Big rainbow walks them a letter at a time. "
                               "Palette names: fire, ice, neon, sea, forest, candy, steel, rainbow."));

    letterSpacing_->setAccessibleName(tr("Letter spacing"));
    lineSpacing_->setAccessibleName(tr("Line spacing"));

    // Settings share a row while the column is wide enough and wrap under
    // each other when it isn't (#236).
    auto* form = new FlowForm(this);
    form->setContentsMargins(0, 0, 0, 0);
    // As close as the rows of the Tool Options form it sits in.
    form->setVerticalSpacing(4);
    const auto pairRow = [form](const QString& text, QWidget* field, const QString& besideText, QWidget* beside) {
        auto* label = new QLabel(text);
        label->setBuddy(field);
        auto* row = new FlowRow;
        row->addWidget(field);
        row->addPair(new QLabel(besideText), beside);
        form->addRow(label, row);
    };
    form->addRow(tr("Face"), picker_);
    pairRow(tr("Big style"), bigStyle_, tr("Colours"), bigColours_);
    // The colour list and the wrap spin box are the widest fields, so each
    // shares a row with a narrow one.
    pairRow(tr("Colour"), colour_, tr("Case"), case_);
    pairRow(tr("Wrap"), wrap_, tr("Align"), align_);
    auto* spacingLabel = new QLabel(tr("Spacing"));
    spacingLabel->setToolTip(tr("Blank columns between letters and blank cell rows between lines"));
    auto* spacing = new FlowRow;
    spacing->addPair(new QLabel(tr("Letters")), letterSpacing_);
    spacing->addPair(new QLabel(tr("Lines")), lineSpacing_);
    form->addRow(spacingLabel, spacing);

    connect(bigStyle_, &QComboBox::currentIndexChanged, this, [this] {
        if (syncing_)
            return;
        const auto style = BigTextStyle(bigStyle_->currentData().toInt());
        bigColours_->setText(QString::fromStdString(bigTextColoursText(bigTextDefaultColours(style))));
        apply(tr("Big Text Style"));
    });
    connect(bigColours_, &QLineEdit::editingFinished, this, [this] { apply(tr("Big Text Colours")); });
    connect(picker_, &FacePicker::picked, this, [this] { apply(tr("Text Face")); });
    connect(case_, &QComboBox::currentIndexChanged, this, [this] { apply(tr("Text Case")); });
    connect(letterSpacing_, &QSpinBox::valueChanged, this, [this] { apply(tr("Letter Spacing")); });
    connect(lineSpacing_, &QSpinBox::valueChanged, this, [this] { apply(tr("Line Spacing")); });
    connect(align_, &QComboBox::currentIndexChanged, this, [this] { apply(tr("Text Alignment")); });
    connect(wrap_, &QSpinBox::valueChanged, this, [this] { apply(tr("Wrap Width")); });
    connect(colour_, &QComboBox::currentIndexChanged, this, [this] { apply(tr("Text Colour")); });

    Document* doc = view_->document();
    // the faces first, so the face shown is listed
    for (auto signal : {&Document::layersChanged, &Document::reset, &Document::fontsFolderChanged})
        connect(doc, signal, this, [this] {
            syncFaces();
            sync();
        });
    connect(doc, &Document::activeLayerChanged, this, &TextOptions::sync);
    connect(tool_, &TextTool::spotChanged, this, &TextOptions::sync);
    connect(tool_, &TextTool::keptChanged, this, [this] {
        facesListed_ = false;
        syncFaces();
        sync();
    });
    syncFaces();
    sync();
}

void TextOptions::syncFaces() {
    const Document* d = view_->document();
    std::shared_ptr<const FontCatalogue> folder = d->sharedFontsFolder();
    picker_->setScanning(d->fontsScanning());
    // the face new text starts in, picked from a folder or project since gone
    const FaceRef& face = tool_->defaults().face;
    const FontStore& keptFonts = tool_->keptFonts();
    std::optional<FaceRef> kept;
    if (keptFonts.face(face) && !d->extras().fonts.file(face.file) && !(folder && folder->store.file(face.file)))
        kept = face;
    if (facesListed_ && folder == folder_ && d->extras().fonts == project_ && kept == kept_)
        return;
    facesListed_ = true;
    folder_ = std::move(folder);
    project_ = d->extras().fonts;
    kept_ = kept;
    std::vector<FaceEntry> entries = bundledFaceEntries();
    if (folder_) {
        const std::vector<FaceEntry> listed = folder_->entries();
        entries.insert(entries.end(), listed.begin(), listed.end());
    }
    for (const auto& [sha, file] : project_.files()) {
        if (folder_ && folder_->store.file(sha))
            continue;
        for (std::size_t i = 0; i < file->names.size(); ++i) {
            FaceRef ref;
            ref.kind = FaceRef::Kind::Tdf;
            ref.name = file->names[i];
            ref.file = sha;
            ref.index = int(i);
            entries.push_back({ref, tr("%1 (in the project)").arg(QString::fromStdString(ref.name)).toStdString(),
                               nullptr, file, file->types[i] == TdfType::Outline ? "outline" :
                                              file->types[i] == TdfType::Block ? "block" : "colour"});
        }
    }
    if (kept_)
        entries.push_back({*kept_, tr("%1 (kept for new text)").arg(QString::fromStdString(kept_->name)).toStdString(),
                           nullptr, keptFonts.file(kept_->file),
                           keptFonts.file(kept_->file)->types[size_t(kept_->index)] == TdfType::Outline ? "outline" :
                           keptFonts.file(kept_->file)->types[size_t(kept_->index)] == TdfType::Block ? "block" : "colour"});
    picker_->setEntries(std::move(entries));
}

TextSource TextOptions::shown() const {
    const Layer* l = view_->document()->active();
    if (l && l->props.text && !tool_->spot())
        return *l->props.text;
    return tool_->defaults();
}

void TextOptions::sync() {
    const QScopedValueRollback guard(syncing_, true);
    const TextSource s = shown();
    picker_->setCurrent(s.face);
    picker_->setText(casedText(s));
    case_->setCurrentIndex(case_->findData(int(s.textCase)));
    letterSpacing_->setValue(s.letterSpacing);
    lineSpacing_->setValue(s.lineSpacing);
    align_->setCurrentIndex(align_->findData(int(s.align)));
    wrap_->setValue(s.wrapWidth);
    colour_->setCurrentIndex(colour_->findData(int(s.colour)));
    bigStyle_->setCurrentIndex(bigStyle_->findData(int(s.bigStyle)));
    bigColours_->setText(QString::fromStdString(
        bigTextColoursText(s.bigColours.empty() ? bigTextDefaultColours(s.bigStyle) : s.bigColours)));
    const bool big = s.bigStyle != BigTextStyle::None;
    picker_->setEnabled(!big);
    colour_->setEnabled(!big);
    bigColours_->setEnabled(big);

}

void TextOptions::apply(const QString& undoText) {
    if (syncing_)
        return;
    TextSource s = shown();
    s.face = picker_->current();
    s.bigStyle = BigTextStyle(bigStyle_->currentData().toInt());
    if (s.bigStyle != BigTextStyle::None) {
        const auto colours = parseBigTextColours(bigColours_->text().toStdString());
        if (!colours) {
            bigColours_->setToolTip(tr("Use mIRC colours 0–15 or names separated by "
                                       "commas, or a palette name."));
            bigColours_->selectAll();
            return;
        }
        s.bigColours = *colours;
        s.face = FaceRef{};
    } else
        s.bigColours.clear();
    s.textCase = TextCase(case_->currentData().toInt());
    s.letterSpacing = letterSpacing_->value();
    s.lineSpacing = lineSpacing_->value();
    s.align = TextAlign(align_->currentData().toInt());
    s.wrapWidth = wrap_->value();
    s.colour = std::uint8_t(colour_->currentData().toInt());
    // a kept font no longer picked leaves the list when the tool drops its
    // file (TextTool::keptChanged), once the picker is out of the signal
    // that picked
    tool_->setSettings(s, undoText, view_->document());
    // a setting the layer refused (its box too large) shows as it is
    sync();
}

} // namespace bw
