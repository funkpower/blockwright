#pragma once
// Colours > Edit Palette... and Colours > Remap Colours... (#497, PLAN §8,
// Palettes).
//
// The palette editor shows the document's 16 colours as buttons; a click
// opens a colour picker for that entry. An entry is kept as an XBIN holds it
// (xbinColour: 6 bits a channel), and the dialog says so, so what it shows
// is what an XBIN save writes. OK applies the whole palette as one undo step.
//
// The remap dialog has a row per index 0-15, each with the colour it becomes
// (a swatch list in the document's palette), a text form of the map the
// CLI's --map takes, and the cells it applies to: the selection, the active
// layer or every layer. OK applies it as one undo step.

#include "document.h"

#include "blockwright/palette_swap.h"

#include <QDialog>

#include <array>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace bw {

class PaletteEditDialog : public QDialog {
    Q_OBJECT
public:
    PaletteEditDialog(const Palette& palette, QWidget* parent = nullptr);

    const Palette& palette() const { return palette_; }
    // Sets entry `index` to `colour`, rounded to an XBIN's 6 bits a channel
    // (xbinColour); what the picker does with the colour picked.
    void setColour(int index, Rgb colour);
    // Every entry as `palette` has it, each rounded the same way.
    void setPalette(const Palette& palette);
    QPushButton* swatch(int index) const { return swatches_[std::size_t(index)]; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void pick(int index);
    void showEntry(int index);

    Palette palette_;
    std::array<QPushButton*, 16> swatches_{};
    QDialogButtonBox* buttons_ = nullptr;
};

class RemapDialog : public QDialog {
    Q_OBJECT
public:
    // `hasSelection` enables the Selection scope (and starts on it).
    RemapDialog(const Palette& palette, bool hasSelection, QWidget* parent = nullptr);

    ColourMap map() const;
    void setMap(const ColourMap& map);
    Document::RemapScope scope() const;
    void setScope(Document::RemapScope scope);
    QComboBox* target(int index) const { return targets_[std::size_t(index)]; }
    QLineEdit* mapText() const { return text_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void syncText();

    std::array<QComboBox*, 16> targets_{};
    QLineEdit* text_ = nullptr;
    QLabel* problem_ = nullptr;
    QRadioButton* selection_ = nullptr;
    QRadioButton* layer_ = nullptr;
    QRadioButton* document_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    bool syncing_ = false;
};

} // namespace bw
