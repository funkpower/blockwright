#pragma once
// The face picker (PLAN §15): the faces a text layer can be set in, each
// with a preview of a sample word drawn in it, a search field over their
// names and favourites (kept in the settings, listed first). For the text
// being set it marks, on each face, the characters the face can't draw and
// the pairs of them that misread for each other in it (confusablesIn).
//
// It lists face entries (font_store.h): the bundled pixel faces, and the
// TheDraw fonts of the fonts folder and the project's copies.

#include "blockwright/face.h"
#include "blockwright/font_store.h"
#include "blockwright/text_source.h"

#include <QHash>
#include <QCache>
#include <QSet>
#include <QIcon>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;
class QListView;
class QComboBox;
class QLabel;
class QToolButton;

namespace bw {

// The bundled pixel faces, in face.h's order.
std::vector<FaceEntry> bundledFaceEntries();

class FacePicker : public QWidget {
    Q_OBJECT
public:
    // Lists metadata only; the delegate resolves faces and previews on paint.
    explicit FacePicker(std::vector<FaceEntry> entries = {}, QWidget* parent = nullptr);

    const std::vector<FaceEntry>& entries() const { return entries_; }
    void setEntries(std::vector<FaceEntry> entries);

    // The face picked; setting it emits nothing.
    const FaceRef& current() const { return current_; }
    void setCurrent(const FaceRef& ref);

    // The text the marks are for.
    void setText(std::u32string text);

    bool isFavourite(const FaceRef& ref) const;
    void setFavourite(const FaceRef& ref, bool on);

    // What an entry is marked with for the text: the characters the face
    // lacks, and its confusable pairs in the text ("O0 S5"); empty for none.
    QString missingIn(const FaceRef& ref) const;
    QString confusablesIn(const FaceRef& ref) const;
    // The names listed now, in order.
    QStringList listed() const;

    QLineEdit* searchField() const { return search_; }
    QListView* list() const { return list_; }
    QComboBox* typeFilter() const { return type_; }
    void setScanning(bool scanning);
    bool scanning() const;
    int previewsBuilt() const { return previewsBuilt_; }
    // Toggles the current face's favourite.
    QToolButton* favouriteButton() const { return favourite_; }

    // The settings key the favourites are kept under.
    static QString favouritesKey();

signals:
    void picked(const bw::FaceRef& ref);

private:
    static QString keyOf(const FaceRef& ref);
    const FaceEntry* find(const FaceRef& ref) const;
    void rebuild();
    QVariant rowData(int row, int role) const;
    void selectCurrent();
    void updateFavouriteButton();

    std::vector<FaceEntry> entries_;
    FaceRef current_;
    std::u32string text_;
    QSet<QString> favourites_;
    mutable QCache<QString, QIcon> previews_{256};
    mutable QHash<int, QString> labels_;
    mutable int previewsBuilt_ = 0;
    std::vector<int> rows_;
    QStringList searchText_;
    QStringList keys_;
    class FaceListModel* model_;
    QComboBox* type_;
    QLabel* scanning_;
    QLineEdit* search_;
    QListView* list_;
    QToolButton* favourite_;
};

} // namespace bw
