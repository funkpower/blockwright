#pragma once
// The Text tool's settings in the Tool Options dock (PLAN §11, §15): the
// face picker, case, letter and line spacing, alignment, wrap width and
// colour, plus whole-cell big modes and their mIRC colours. They show the active text layer's source, or what a new text
// layer starts with when the active layer isn't one or the tool's text
// cursor waits for a new layer (TextTool::spot), and a change sets both
// (TextTool::setSettings), one undo step on the layer.
//
// The face picker lists the bundled faces, then the TheDraw fonts of the
// fonts folder (Document::fontsFolder), then the project's copies of fonts
// the folder doesn't have, "NAME (in the project)".

#include "blockwright/font_store.h"
#include "blockwright/text_source.h"

#include <QWidget>

#include <memory>
#include <optional>

class QComboBox;
class QSpinBox;
class QLineEdit;

namespace bw {

class CanvasView;
class FacePicker;
class TextTool;

class TextOptions : public QWidget {
    Q_OBJECT
  public:
    TextOptions(CanvasView* view, TextTool* tool, QWidget* parent = nullptr);

    FacePicker* facePicker() const { return picker_; }
    QComboBox* caseBox() const { return case_; }
    QSpinBox* letterSpacingBox() const { return letterSpacing_; }
    QSpinBox* lineSpacingBox() const { return lineSpacing_; }
    QComboBox* alignBox() const { return align_; }
    QSpinBox* wrapWidthBox() const { return wrap_; }
    QComboBox* colourBox() const { return colour_; }
    QComboBox* bigStyleBox() const { return bigStyle_; }
    QLineEdit* bigColoursBox() const { return bigColours_; }
    // Shows the settings the controls show again: the active text layer's,
    // or the tool's defaults (--screenshot calls it after setting them).
    void sync();

  private:
    // The source the controls show.
    TextSource shown() const;
    // Lists the faces the document can set text in, when the fonts folder,
    // the project's fonts or the face new text starts in, kept by the tool
    // (TextTool::keptFonts), changed; the kept one only when neither of the
    // others has it.
    void syncFaces();
    // Sets the tool's settings from the controls, the step named `undoText`.
    void apply(const QString& undoText);

    CanvasView* view_;
    TextTool* tool_;
    FacePicker* picker_;
    QComboBox* case_;
    QSpinBox* letterSpacing_;
    QSpinBox* lineSpacing_;
    QComboBox* align_;
    QSpinBox* wrap_;
    QComboBox* colour_;
    QComboBox* bigStyle_;
    QLineEdit* bigColours_;
    bool syncing_ = false;
    // What the listed TheDraw faces belong to, held so they last; the tool
    // holds the kept one's file until it has told (TextTool::keptChanged).
    std::shared_ptr<const FontCatalogue> folder_;
    FontStore project_;
    std::optional<FaceRef> kept_;
    bool facesListed_ = false;
};

} // namespace bw
