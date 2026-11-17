#pragma once
// Help > Tool Reference...: what each tool and layer kind is for, from the help text
// store (help_text.h), one section a tool in the Tools menu's order with its
// key and its picture (tool_illustration.h), then the layer kinds. It opens offline; the docs site's full manual
// (#57) isn't bundled. A button opens Help > Keyboard Shortcuts.

#include <QDialog>
#include <QString>

class QTextBrowser;
class QPushButton;
class QUrl;

namespace bw {

class ToolRegistry;

// The manual as Markdown: every tool in `tools`, its name, the key of the
// window's action for it (objectName <id>Tool) when it has one, its picture
// as an image at illustrationUrl(id) when it has one, and its help, then
// every layer kind with help. The dialog's text() loads the pictures.
QString manualMarkdown(const ToolRegistry& tools, const QWidget* window);
QUrl illustrationUrl(const QString& toolId);

class ManualDialog : public QDialog {
    Q_OBJECT
public:
    // The manual for `window`'s tools; `window` is also its parent.
    ManualDialog(const ToolRegistry& tools, QWidget* window);

    QTextBrowser* text() const { return text_; }
    QPushButton* shortcutsButton() const { return shortcuts_; }

private:
    QTextBrowser* text_;
    QPushButton* shortcuts_;
};

// Shows `window`'s manual. The window has one, made the first time and
// hidden, not deleted, when it is closed.
ManualDialog* showManual(const ToolRegistry& tools, QWidget* window);

} // namespace bw
