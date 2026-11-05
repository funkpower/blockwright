#pragma once
// The Layer Style dialog (PLAN §15), from Layer > Layer Style… or a
// double-click on a layer's Style cell in the Layers dock: the effects on
// the left, one row of each kind in the order they draw, each with its
// switch; the selected effect's settings on the right. Under them the layer
// drawn with the style at 1×, the IRC bytes of its widest line as WeeChat's
// ascii script sends it (irc::txtSentBytes, as the row meter measures it)
// against the project's budget and that line as written, the lint of it
// and, for a text layer, the characters its face draws alike. Every change
// redraws those; the layer itself is changed only by OK, as one undo step
// "Layer Style" through Document::setLayerProperties (Cancel changes
// nothing). Save as Preset… writes the style to the user library as a
// .style.json. The Preset box
// above the effects lists the Library's text styles: picking one puts its
// effects in the rows, in its order, in place of the layer's. A preset that
// needs iCE colours is shown disabled, its tooltip saying why, while the
// document has them off.

#include "blockwright/canvas.h"
#include "blockwright/layer.h"
#include "blockwright/library.h"
#include "blockwright/lint_rules.h"
#include "blockwright/text_style.h"

#include <QDialog>

#include <array>
#include <filesystem>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QScrollArea;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace bw {

class Document;

// The dialog's rows: an effect of each kind in EffectKind's order, the
// style's own where it has one, the default switched off where not.
std::vector<Effect> styleRows(const std::optional<TextStyle>& style);

// The style the rows give a layer whose style was `original`: its effects
// in their order with the rows' switches and settings, then each other row
// that is switched on or has settings other than the default, in the rows'
// order. No style when that leaves nothing and the layer had none.
std::optional<TextStyle> styleFromRows(const std::optional<TextStyle>& original, const std::vector<Effect>& rows);

class LayerStyleDialog : public QDialog {
    Q_OBJECT
public:
    LayerStyleDialog(Document* doc, LayerId id, QWidget* parent = nullptr);

    LayerId layer() const { return id_; }
    const std::vector<Effect>& rows() const { return rows_; }
    // Puts `effect` in the row of its kind, the widgets following.
    void setRow(const Effect& effect);
    std::optional<TextStyle> style() const;
    // Why the style can't go on the layer (layerStyleProblem, or the text
    // drawn with it), or empty; OK is off while there is one.
    QString problem();

    // Sets the style on the layer, one undo step; false when it changes
    // nothing or the document refuses it. OK calls it.
    bool apply();

    // The layer alone drawn with the style (its cells, the transparent ones
    // blank), the bytes of its widest IRC line (irc::encodeCanvas), its lint
    // (the Lint dock's default: every named ramp and the craft pairs) and,
    // for a text layer, the pairs of characters its face draws alike in its
    // text ("S5 O0"; empty when none).
    const Canvas& previewCells();
    // The widest line's bytes as the ascii script sends it, and as written.
    std::size_t widestLineBytes();
    std::size_t widestLineWrittenBytes();
    const lint::LintReport& lintReport();
    QString confusables();

    // The presets the Preset box offers (the Library dock's text styles).
    void setPresets(std::vector<library::TextStyleEntry> presets);
    const std::vector<library::TextStyleEntry>& presets() const { return presets_; }
    // Puts preset `index`'s effects in the rows, in its order, in place of
    // the layer's (what picking it in the box does); false, and the rows as
    // they were, for an index out of range or a preset that needs iCE
    // colours while the document has them off.
    bool usePreset(int index);

    // Where Save as Preset… writes; empty turns it off.
    void setPresetDir(std::filesystem::path dir, QStringList categories = {});
    // Writes style() as a preset (library::writeTextStyle) and emits
    // presetSaved; the path written, or empty with `error` set.
    QString savePreset(const QString& name, const QString& category, const QStringList& tags, QString* error);

    QListWidget* effectList() const { return list_; }
    QStackedWidget* pages() const { return pages_; }
    QLabel* previewLabel() const { return preview_; }
    QScrollArea* previewArea() const { return previewArea_; }
    QLabel* ircLabel() const { return irc_; }
    QPlainTextEdit* lintText() const { return lint_; }
    QLabel* confusableLabel() const { return confusable_; }
    QLabel* problemLabel() const { return problem_; }
    QPushButton* presetButton() const { return preset_; }
    // The Preset box: a "Choose…" row, then one row per preset.
    QComboBox* presetBox() const { return presetBox_; }
    QDialogButtonBox* buttons() const { return buttons_; }

signals:
    void presetSaved(const QString& path);

private:
    struct Page {
        QComboBox* colour = nullptr;
        QComboBox* ramp = nullptr;
        QComboBox* compass = nullptr;
        QComboBox* form = nullptr;
        QSpinBox* amount = nullptr;
        QSpinBox* level = nullptr;  // Glow's brightness
    };

    QWidget* makePage(EffectKind kind);
    void fillPage(EffectKind kind);
    void readPage(EffectKind kind);
    void fillRow(int row);
    void changed();
    void refresh();
    void askPreset();

    Document* doc_;
    LayerId id_;
    std::optional<TextStyle> original_;
    // The effects whose order style() keeps: the layer's, or a preset's
    // once one is picked.
    std::optional<TextStyle> order_;
    std::vector<Effect> rows_;
    std::vector<library::TextStyleEntry> presets_;
    std::array<Page, TextStyle::kMaxEffects> page_{};
    int filling_ = 0;
    bool stale_ = true;
    Canvas cells_{0, 0};
    std::size_t widest_ = 0, widestWritten_ = 0;
    lint::LintReport report_;
    QString confusables_, problemText_;
    std::filesystem::path presetDir_;
    QStringList categories_;

    QComboBox* presetBox_;
    QListWidget* list_;
    QStackedWidget* pages_;
    QLabel* preview_;
    QScrollArea* previewArea_;
    QLabel* irc_;
    QPlainTextEdit* lint_;
    QLabel* confusable_;
    QLabel* problem_;
    QPushButton* preset_;
    QDialogButtonBox* buttons_;
};

} // namespace bw
