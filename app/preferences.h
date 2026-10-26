#pragma once
// Edit > Preferences...: the fonts folder (PLAN §15), where TheDraw fonts
// for text layers are read from. There is none by default; the folder is
// kept in the settings (appSettings) under kFontsFolderKey, and the dialog
// lists what a scan of it finds: each font, and each .tdf file it couldn't
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

namespace bw {

inline constexpr char kFontsFolderKey[] = "fonts/folder";

// The fonts folder the settings hold, or empty when none is set.
QString fontsFolderSetting();
// Keeps `folder` as the fonts folder; empty clears it.
void setFontsFolderSetting(const QString& folder);

// The catalogue of `folder` (bw::scanFontsFolder), or null for none (empty).
std::shared_ptr<const FontCatalogue> loadFontsFolder(const QString& folder);

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // Shows `folder` and the catalogue of it, scanning it when `catalogue`
    // is null.
    explicit PreferencesDialog(const QString& folder, std::shared_ptr<const FontCatalogue> catalogue = nullptr,
                               QWidget* parent = nullptr);

    // The folder as entered, trimmed; empty for none.
    QString fontsFolder() const;
    // The catalogue of fontsFolder(), scanned when it last changed; null for
    // no folder.
    std::shared_ptr<const FontCatalogue> catalogue() const { return catalogue_; }

    QLineEdit* fontsFolderEdit() const { return folder_; }
    QPushButton* browseButton() const { return browse_; }
    QPushButton* clearButton() const { return clear_; }
    QLabel* fontsSummary() const { return summary_; }
    QPlainTextEdit* fontsList() const { return list_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    // Scans the folder entered when it isn't the one last scanned, or
    // `always` (OK does, so fonts added to an unchanged folder are seen).
    void rescan(bool always = false);
    void showCatalogue();

    QString scanned_;
    std::shared_ptr<const FontCatalogue> catalogue_;
    QLineEdit* folder_;
    QPushButton* browse_;
    QPushButton* clear_;
    QLabel* summary_;
    QPlainTextEdit* list_;
    QDialogButtonBox* buttons_;
};

} // namespace bw
