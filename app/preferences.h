#pragma once
// Edit > Preferences...: a tab per page. Fonts is the fonts folder (PLAN
// §15), where TheDraw fonts for text layers are read from; the window adds
// Shortcuts (shortcut_settings.h). The local collection is the default; the folder is
// kept in the settings (appSettings) under kFontsFolderKey, and the dialog
// lists what a background scan of it finds: each font, and each .tdf file it couldn't
// read with the reason.

#include "blockwright/font_store.h"

#include <QDialog>
#include <QString>

#include <memory>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

namespace bw {

inline constexpr char kFontsFolderKey[] = "fonts/folder";

// The explicit setting, including empty, or the existing default under home.
// A null home resolves through testing::home, or QDir::homePath() when that
// is null too; tests can inject a scratch home.
QString fontsFolderSetting(const QString& home = {});
bool fontsFolderIsDefault();
// Keeps `folder` as the fonts folder; empty clears it.
void setFontsFolderSetting(const QString& folder);

// The catalogue of `folder` (bw::scanFontsFolder), or null for none (empty).
std::shared_ptr<const FontCatalogue> loadFontsFolder(const QString& folder);

namespace testing {
// When not null, the home folder the default fonts folder is looked for
// under. A test sets it on Windows, where QDir::homePath() is the user's
// profile folder, which no variable it can set moves.
inline QString home;
} // namespace testing

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // Shows `folder` and its catalogue, scanning in the background when
    // `catalogue` is null.
    explicit PreferencesDialog(const QString& folder, std::shared_ptr<const FontCatalogue> catalogue = nullptr,
                               QWidget* parent = nullptr);

    // The folder as entered, trimmed; empty for none.
    QString fontsFolder() const;
    // An edited value or an explicit Browse/Clear choice; OK alone keeps defaults implicit.
    bool fontsFolderChanged() const;
    // The catalogue of fontsFolder(); null while scanning or for no folder.
    std::shared_ptr<const FontCatalogue> catalogue() const { return catalogue_; }
    bool scanning() const;

    QLineEdit* fontsFolderEdit() const { return folder_; }
    QPushButton* browseButton() const { return browse_; }
    QPushButton* clearButton() const { return clear_; }
    QLabel* fontsSummary() const { return summary_; }
    QPlainTextEdit* fontsList() const { return list_; }
    QDialogButtonBox* buttons() const { return buttons_; }
    // The tabs, Fonts first; addPage puts another after them.
    QTabWidget* pages() const { return pages_; }
    void addPage(QWidget* page, const QString& title);

private:
    // Scans the folder entered when it isn't the one last scanned, or
    // `always` (OK does, so fonts added to an unchanged folder are seen).
    void rescan(bool always = false);
    void showCatalogue();

    class FontScanner* scanner_;
    QLabel* default_;
    QString scanned_;
    QString initialFolder_;
    bool folderChosen_ = false;
    std::shared_ptr<const FontCatalogue> catalogue_;
    QLineEdit* folder_;
    QPushButton* browse_;
    QPushButton* clear_;
    QLabel* summary_;
    QPlainTextEdit* list_;
    QDialogButtonBox* buttons_;
    QTabWidget* pages_;
};

} // namespace bw
