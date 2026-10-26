#pragma once
// The generator form (PLAN §7, §11), in the Layers dock under the list: the
// parameters the active generator layer declares (generator.json's
// "params"), with their values from params.json, one field each: a slider
// with a spin box for an integer with a range, a spin box for another
// number, a check box for a boolean, a palette colour picker, a ramp list
// and a line of text. A field commits when it is let go (a slider
// released, a spin box's Enter or focus leaving it, a picked entry), and a
// commit is one undo step (Document::setGeneratorParam), after which the
// generator runs again (GeneratorRunner). Run runs it with its params as
// they are (Document::requestGeneratorRun). Hidden unless the active layer
// is a generator.

#include "blockwright/generator.h"
#include "blockwright/layer.h"

#include <QWidget>

#include <map>
#include <optional>
#include <string>

class QFormLayout;
class QLabel;
class QPushButton;

namespace bw {

class Document;

class GeneratorForm : public QWidget {
    Q_OBJECT
public:
    explicit GeneratorForm(Document* doc, QWidget* parent = nullptr);

    // The layer the form shows; 0 for none (hidden).
    LayerId layer() const { return id_; }
    // The field for the parameter `name` ("param_<name>"), or null.
    QWidget* field(const std::string& name) const;
    QPushButton* runButton() const { return run_; }
    // What the form says about the generator: a spec it can't read, or a
    // value it refused.
    QLabel* message() const { return message_; }

private:
    // Brings the form up to the document: rebuilt for another layer or
    // spec, else only the values put back (so a field being used stays).
    void sync();
    void rebuild(const generator::Spec& spec);
    // Puts each param's value from params.json in its field, leaving a field
    // whose value is the one it was last given (shown_), so an edit not yet
    // committed, typed text or a spin box's, outlives a run landing.
    void showValues();
    void commit(const std::string& name, const generator::ParamValue& value);

    Document* doc_;
    LayerId id_ = 0;
    std::optional<std::string> specText_;
    std::optional<generator::Spec> spec_;
    QWidget* fields_;
    QFormLayout* form_;
    std::map<std::string, QWidget*> widgets_;
    // The value each field was last given (showValues).
    std::map<std::string, generator::ParamValue> shown_;
    QLabel* message_;
    QPushButton* run_;
};

} // namespace bw
