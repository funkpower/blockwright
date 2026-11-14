#pragma once
// The Micron colour dialog (PLAN §17): any of the 4096 colours a three-digit
// Micron code (`Frgb, `Brgb) writes, each channel one hex digit 0-F whose
// nibble is doubled. A slider and a spin box a channel and the code as three
// digits, kept in step, with a swatch of the colour. It offers those 4096
// and nothing else, so a colour picked here is written as it shows.

#include "blockwright/palette.h"

#include <QDialog>

#include <array>

class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;

namespace bw {

class MicronColourDialog : public QDialog {
    Q_OBJECT
public:
    // Starts at the twelve-bit colour nearest `initial`, each channel to its
    // nearest digit.
    explicit MicronColourDialog(Rgb initial, const QString& title, QWidget* parent = nullptr);

    // The colour picked: micron::twelveBit of the digits.
    Rgb colour() const;
    // The three digits, 0-15 each.
    std::array<int, 3> digits() const { return digits_; }
    void setDigits(int r, int g, int b);
    // The code as Micron writes it after `F or `B ("a50").
    QString code() const;

    // The digit nearest a channel value: v / 17 rounded.
    static int nearestDigit(int v);

private:
    void sync();

    std::array<int, 3> digits_{};
    std::array<QSlider*, 3> sliders_{};
    std::array<QSpinBox*, 3> spins_{};
    QLineEdit* code_ = nullptr;
    QLabel* swatch_ = nullptr;
    bool syncing_ = false;
};

} // namespace bw
