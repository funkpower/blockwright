#pragma once
// What's This (#153): Help > What's This? (Shift+F1) puts the window in
// Qt's What's This mode, and a click on a part of it says what that part
// does. A tool's action, and so its toolbox button and Tools menu item,
// shows the tool's whole help (help_text.h). The settings in Tool Options
// show their tooltips, which already say what each one changes: the panel
// answers for any widget in it that has a tooltip and no What's This text
// of its own, and for a form row's label by its field, so a tooltip set or
// changed later is shown too.

#include <QString>

class QWidget;

namespace bw {

// The tool's help as rich text for a What's This popup; empty with no help.
QString toolWhatsThis(const QString& id);

// Makes root answer What's This for the widgets in it from their tooltips.
void whatsThisFromToolTips(QWidget* root);

// The text a What's This click on w shows under whatsThisFromToolTips:
// w's own What's This text, else the tooltip of w or the nearest widget
// above it inside root (a label's buddy's for a label); empty when none.
QString toolTipWhatsThis(const QWidget* root, const QWidget* w);

} // namespace bw
