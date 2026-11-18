#pragma once
// The palette the app's colour lists show (#497, PLAN §8, Palettes): the
// open document's, which MainWindow sets on open, new and every palette
// change. The colour and ramp lists in Tool Options, the text options, the
// generator form and the layer style dialog draw their swatches in it, and
// the docks' lists follow a change (refreshSwatches).

#include "blockwright/palette.h"
#include "blockwright/ramp.h"

#include <QIcon>
#include <QObject>

class QComboBox;
class QWidget;

namespace bw {

class ShownPalette : public QObject {
    Q_OBJECT
public:
    static ShownPalette& instance();
    const Palette& colours() const { return colours_; }
    // Emits changed() when `colours` are other than the ones shown.
    void set(const Palette& colours);

signals:
    void changed();

private:
    Palette colours_ = kPalette;
};

// ShownPalette::instance().colours().
const Palette& shownPalette();

// Palette entry `index` as a 16 x 16 swatch, in the shown palette.
QIcon swatchIcon(int index);
// A ramp's colours dark to light, 8 px each and 12 high, in the shown palette.
QIcon rampIcon(const Ramp& ramp);

// What a list's items are, for refreshSwatches: item n palette entry n, or
// each item a named ramp by its data (the ramp's name; any other data, a
// custom ramp, is left).
enum class SwatchList { Colours, Ramps };
void markSwatches(QComboBox* box, SwatchList kind);
// Draws the icons of every list under `root` markSwatches marked again, in
// the shown palette.
void refreshSwatches(QWidget* root);

} // namespace bw
