#pragma once
// File > SAUCE...: the record's title, author, group, date and COMNT
// comment lines. Fields hold only text CP437 can store and no more than
// SAUCE has room for (35/20/20 characters, 64 per comment line, 255 lines,
// a CCYYMMDD date); OK stays disabled while anything is over.

#include "blockwright/sauce.h"

#include <QDialog>
#include <QString>

#include <optional>
#include <string>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace bw {

// `text` as CP437 bytes, or nothing if a character has no CP437 glyph or is
// NUL. Composed first (NFC), so a decomposed é is one byte.
std::optional<std::string> toCp437(const QString& text);
// CP437 bytes as text.
QString fromCp437(const std::string& bytes);

class SauceDialog : public QDialog {
    Q_OBJECT
public:
    // Shows the text fields of `sauce`.
    explicit SauceDialog(const Sauce& sauce, QWidget* parent = nullptr);

    // A record holding the dialog's text fields and nothing else: trailing
    // spaces dropped (SAUCE pads with them), and trailing empty comment
    // lines. A field (or the comment block) left as shown is the record's,
    // byte for byte, so accepting the dialog unchanged changes nothing.
    // Only meaningful while problem() is empty.
    Sauce fields() const;
    // Why the fields can't be stored, or empty. A field left as shown is
    // not checked: a file's malformed date doesn't block editing the rest.
    QString problem() const;

    QLineEdit* titleEdit() const { return title_; }
    QLineEdit* authorEdit() const { return author_; }
    QLineEdit* groupEdit() const { return group_; }
    QLineEdit* dateEdit() const { return date_; }
    QPlainTextEdit* commentsEdit() const { return comments_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void check();

    Sauce shown_;  // the record the dialog opened with
    QString shownComments_;  // and its comments as the box first held them
    QLineEdit* title_;
    QLineEdit* author_;
    QLineEdit* group_;
    QLineEdit* date_;
    QPlainTextEdit* comments_;
    QLabel* problem_;
    QDialogButtonBox* buttons_;
};

} // namespace bw
