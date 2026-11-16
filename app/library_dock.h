#pragma once
// The Library dock (PLAN §12): the stamps and text styles of the bundled and
// user libraries as a grid of thumbnails, with a category list and a search
// box (both cover the two kinds alike). Double-
// clicking a stamp (or Enter on it) asks for it to be placed at the cell
// cursor; dragging it carries its stampKey as kStampMimeType, which the canvas
// takes through StampDropFilter. The window does the placing, through the
// select tool's floating paste.
//
// Thumbnails are the stamp rendered at 1x by render/ and scaled down only to
// fit the grid (stampIcon). Only the icon is cached, keyed by what it draws (the cells
// and the glyph width), so a rescan re-renders only stamps whose art
// changed, and drops the icons of stamps that changed or are gone.
//
// A text style's thumbnail is its sample (library::textStyleSample, the one
// `library check` draws: "Aa" in the block face) drawn with iCE colours, as
// `library list` sizes it, through the same stampIcon and the same cache,
// keyed by the entry's JSON. Activating one emits textStyleActivated (the
// window applies it to the active layer); dragging one carries its stampKey
// as kTextStyleMimeType, which StampDropFilter takes as styleDropped (the
// window makes a text layer in it). A style that needs iCE colours is shown
// disabled, so it can be neither activated nor dragged, while the document
// has them off (setIceColours); its thumbnail is still the iCE look.
//
// The user's collections (#464, PLAN §12 Collections): the dock's
// Collections button, the category list's context menu and the grid's hold
// New, Rename and Delete Collection, Add Files to Collection, and for a
// stamp Add to Collection, Edit Stamp and Remove Stamp (Delete on the grid
// too). Each goes through library::'s Collections functions on the user
// library, rescans, and says in message() what it did or why it was
// refused; a bundled stamp or a bundled category is refused there.
//
// StampDialog is Selection > Save to Library...: name, category and tags,
// and Edit Stamp's dialog.

#include "blockwright/library.h"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QListWidget>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace bw {

class CanvasView;

inline constexpr char kStampMimeType[] = "application/x-blockwright-stamp";
inline constexpr char kTextStyleMimeType[] = "application/x-blockwright-text-style";

// A stamp's id in the dock, its drag payload and stampActivated: the .ans
// path's native bytes (UTF-16 as UTF-8 on Windows), so a name that isn't
// UTF-8 never collides with another the way two decoded QStrings would.
QByteArray stampKey(const std::filesystem::path& p);
// Empty if `key` can't be decoded (not UTF-8, on Windows).
std::filesystem::path stampKeyPath(const QByteArray& key);

// A library frame style's id for the Frame tool: "library:" and its
// path's native bytes (stampKey) percent-encoded but for '/', so two names
// that aren't UTF-8 stay apart and a plain path reads as itself.
QString frameStyleId(const std::filesystem::path& p);

// The grid's icon box.
inline constexpr QSize kIconBox(96, 64);

// A stamp's icon: `cv` rendered at 1x (render/), as is if it fits `box`,
// else scaled down to fit (keeping its aspect) by averaging the 1x pixels
// under each icon pixel. The 1x picture is rendered a band of rows (about
// `bandPixels` pixels) at a time, so a large stamp never holds its whole 1x
// image; the result is the same whatever the band size.
QImage stampIcon(const Canvas& cv, int glyphWidth, QSize box, std::int64_t bandPixels = std::int64_t(1) << 22);

// A frame style's thumbnail: drawn width x height (grown to its corners,
// which kMaxFrameSourceColumns x kMaxFrameSourceRows bounds) and fitted to
// `box` as stampIcon fits a stamp. A declared minimum larger than that is
// left out, so a style's "min" never sizes the picture. Null if the style
// isn't valid.
QImage frameStyleIcon(const FrameStyle& style, int width, int height, QSize box);

// The thumbnail grid: an icon-mode list whose drags carry the stamp's or
// text style's key. Every item is laid out in a whole grid cell, and
// fitGridToItems sizes the cell to its items.
class StampGrid : public QListWidget {
    Q_OBJECT
public:
    explicit StampGrid(QWidget* parent = nullptr);

    // Sizes the grid's cell to hold every item whole: the icon box's width
    // and a gap (or more, for a word of a name that is wider), and the height
    // of the tallest thumbnail and wrapped name, as the item delegate lays
    // them out in the grid's font and style. Called after the items change;
    // a font or style change calls it too.
    void fitGridToItems();

signals:
    // Delete pressed on the grid.
    void deletePressed();

protected:
    void changeEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
};

class LibraryDock : public QWidget {
    Q_OBJECT
public:
    struct Entry {
        library::Stamp stamp;
        bool bundled = false;
        QByteArray thumbKey;  // what its thumbnail draws
    };

    struct StyleEntry {
        library::TextStyleEntry style;
        bool bundled = false;
        QByteArray thumbKey;  // what its thumbnail draws
    };

    explicit LibraryDock(QWidget* parent = nullptr);

    // The two libraries; rescans.
    void setDirectories(const std::filesystem::path& bundled, const std::filesystem::path& user);
    const std::filesystem::path& bundledDir() const { return bundled_; }
    const std::filesystem::path& userDir() const { return user_; }

    // Reads both libraries again and redraws the grid, keeping the search
    // and the category.
    void rescan();

    // The document's iCE setting (on until told otherwise). With it off, a
    // text style that needs iCE colours is shown disabled.
    void setIceColours(bool on);
    bool iceColours() const { return ice_; }

    const std::vector<Entry>& entries() const { return entries_; }
    const std::vector<StyleEntry>& styles() const { return styles_; }
    // The frame styles (.frame.json) in both libraries, bundled first, each
    // with frameStyleId(its path) as its id, for the Frame tool's picker.
    const std::vector<FrameStyle>& frameStyles() const { return frames_; }
    const std::vector<library::Problem>& problems() const { return problems_; }
    // The stamp read from the .ans at `path`, or null.
    const library::Stamp* find(const std::filesystem::path& path) const;
    // The stamp whose stampKey is `key`, or null: any other bytes (from
    // another application's drag, say) are no stamp, and are never decoded.
    const library::Stamp* findKey(const QByteArray& key) const;
    // The text style whose stampKey (of its .style.json) is `key`, or null.
    const library::TextStyleEntry* findStyleKey(const QByteArray& key) const;
    QStringList categories() const;

    // The names the grid shows, in order (for tests): the stamps, then the
    // text styles.
    QStringList shownNames() const;
    // How many thumbnails have been rendered (a cache miss each), and the
    // cache: one icon-sized image per distinct stamp art or text style in
    // the libraries.
    int thumbnailRenders() const { return renders_; }
    const QHash<QByteArray, QImage>& thumbnailCache() const { return thumbnails_; }

    QLineEdit* searchEdit() const { return search_; }
    QListWidget* categoryList() const { return categories_; }
    StampGrid* grid() const { return grid_; }
    QLabel* problemLabel() const { return problemLabel_; }

    // Collections (#464). The user library's collections, read on each
    // rescan (an empty one is listed as a category too).
    const std::vector<library::Collection>& collections() const { return collections_; }
    // The user's collection called `name`, or null. Two folders may go by
    // one name (a folder with no marker whose stamps share a collection's
    // name); this gives the first, so a command checks with
    // uniqueCollection before acting.
    const library::Collection* findCollection(const QString& name) const;
    // The collection called `name` when exactly one folder goes by it;
    // null otherwise, with *error saying why when it is more than one
    // (library::findCollection's refusal, naming both) and empty when none.
    const library::Collection* uniqueCollection(const QString& name, QString* error) const;
    // The user stamp titled `title` (CP437) anywhere in the collection's
    // folder, a stamp renamed in place included: the one
    // library::addStamp(replace) writes over. Empty when there is none.
    std::filesystem::path stampTitled(const library::Collection& c, const std::string& title) const;
    // The category picked in the list, or empty for All.
    QString pickedCategory() const;
    // The stamp current in the grid, or null (nothing, or a text style).
    const Entry* currentEntry() const;
    // The commands without asking: each changes the user library, rescans
    // (keeping the search and the category; a picked collection renamed
    // stays picked under its new name), says what it did in message() and
    // returns true; or refuses, changing nothing, with the reason in
    // *error. A stamp or collection that isn't the user's is refused.
    bool createCollection(const QString& name, QString* error);
    // A removal goes to the trash unless `how` is Removal::Delete. A trash
    // that refuses (library::TrashUnavailable: none, or it can't take the
    // files; for a stamp, also a sidecar that can't be read, which the
    // trash couldn't give back) removes nothing and sets *trashRefused,
    // when given, beside the error; the ask commands then offer deleting
    // for good.
    bool deleteCollection(const QString& name, QString* error, library::Removal how = library::Removal::Trash,
                          bool* trashRefused = nullptr);
    bool renameCollection(const QString& from, const QString& to, QString* error);
    // Copies the stamp (bundled or the user's) into the collection.
    bool addToCollection(const std::filesystem::path& stamp, const QString& collection, QString* error);
    // Adds each .ans of `files` (a folder: the .ans files in it and below)
    // to the collection; how many were added. Each refused is listed in
    // *failed as "<file>: <why>", and the rest are added. A collection
    // refused itself (not the user's, gone from the disk since the last
    // scan, or a user library that is the bundled one) adds nothing and
    // lists no file: the reason goes in *refusal, when given, and the
    // status reads "Nothing added: <why>; <n> files not added".
    int addFilesToCollection(const QString& collection, const QStringList& files, QStringList* failed,
                             QString* refusal = nullptr);
    bool removeStamp(const std::filesystem::path& stamp, QString* error, library::Removal how = library::Removal::Trash,
                     bool* trashRefused = nullptr);
    // Renames, re-files and re-tags a user stamp (library::editStamp);
    // only what differs from the stamp is changed.
    bool editStamp(const std::filesystem::path& stamp, const QString& name, const QString& category,
                   const QStringList& tags, QString* error);

    // The commands' actions, asking first where they need to (each is a
    // child of the dock, so it is listed under Library).
    QAction* newCollectionAction() const { return newCollection_; }
    QAction* renameCollectionAction() const { return renameCollection_; }
    QAction* deleteCollectionAction() const { return deleteCollection_; }
    QAction* addFilesAction() const { return addFiles_; }
    QAction* addToCollectionAction() const { return addToCollection_; }
    QAction* editStampAction() const { return editStamp_; }
    QAction* removeStampAction() const { return removeStamp_; }

signals:
    // What a command did, or why it was refused, for the status bar.
    void message(const QString& text);
    // A stamp was double-clicked or activated with Enter (its stampKey).
    void stampActivated(const QByteArray& key);
    // A text style was double-clicked or activated with Enter (the stampKey
    // of its .style.json).
    void textStyleActivated(const QByteArray& key);
    // A rescan finished.
    void rescanned();

private:
    void refill();
    void createActions();
    void showGridMenu(const QPoint& pos);
    void showCategoryMenu(const QPoint& pos);
    // The commands as the actions run them, asking first.
    void askNewCollection();
    void askRenameCollection();
    void askDeleteCollection();
    void askAddFiles();
    void askAddToCollection();
    void askEditStamp();
    void askRemoveStamp();
    // The picked category as a collection of the user's, or null with the
    // reason in message() (`what` names the command: "renamed").
    const library::Collection* pickedCollection(const QString& what);
    // The current stamp as one of the user's, or null with the reason in
    // message().
    const Entry* currentUserStamp(const QString& what);
    // Asks which collection (an existing one, or a new name, made first);
    // empty if cancelled or refused.
    QString askCollection(const QString& title);
    void refused(const QString& why);
    static QByteArray thumbnailKey(const library::Stamp& s);
    static QByteArray thumbnailKey(const library::TextStyleEntry& s);
    QImage thumbnail(const Entry& e);
    QImage thumbnail(const StyleEntry& e);
    QImage cachedThumbnail(const QByteArray& key, const std::function<QImage()>& render);

    std::filesystem::path bundled_, user_;
    std::vector<Entry> entries_;
    std::vector<StyleEntry> styles_;
    std::vector<FrameStyle> frames_;
    std::vector<library::Problem> problems_;
    std::vector<library::Collection> collections_;
    QHash<QByteArray, QImage> thumbnails_;
    int renders_ = 0;
    bool ice_ = true;

    QLineEdit* search_;
    QListWidget* categories_;
    StampGrid* grid_;
    QLabel* problemLabel_;
    QAction* newCollection_ = nullptr;
    QAction* renameCollection_ = nullptr;
    QAction* deleteCollection_ = nullptr;
    QAction* addFiles_ = nullptr;
    QAction* addToCollection_ = nullptr;
    QAction* editStamp_ = nullptr;
    QAction* removeStamp_ = nullptr;
};

// Watches the canvas view for stamps dragged from the dock and says which
// cell each was dropped on. The view is told to accept drops.
class StampDropFilter : public QObject {
    Q_OBJECT
public:
    explicit StampDropFilter(CanvasView* view);

signals:
    void dropped(const QByteArray& key, QPoint cell);       // a stamp's stampKey
    void styleDropped(const QByteArray& key, QPoint cell);  // a text style's

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    CanvasView* view_;
};

// Selection > Save to Library...: a name (the SAUCE title, CP437, at most
// 35 bytes), a category (an existing one or a new one) and comma-separated
// tags (so a tag never holds a comma). OK waits until problem() is empty.
class StampDialog : public QDialog {
    Q_OBJECT
public:
    // What the fields name: a stamp (Save to Library: a CP437 name of at
    // most a SAUCE title), a text style preset (Save as Preset: any name),
    // or a stamp being edited (Edit Stamp: a stamp's fields, no Resize).
    enum class Purpose { Stamp, Preset, Edit };

    StampDialog(const QStringList& categories, QWidget* parent = nullptr, Purpose purpose = Purpose::Stamp);

    QString name() const;
    QString category() const;
    QStringList tags() const;
    // A stamp's slice insets (Resize: Width only is a band, Width and
    // height a frame), or unset for a stamp of a fixed size. Whether they
    // fit the cells is encodeStamp's to say.
    std::optional<StampSlice> slice() const;
    void setSlice(const std::optional<StampSlice>& slice);
    // Why the fields can't be saved, or empty.
    QString problem() const;

    QLineEdit* nameEdit() const { return name_; }
    QComboBox* categoryBox() const { return category_; }
    QLineEdit* tagsEdit() const { return tags_; }
    QDialogButtonBox* buttons() const { return buttons_; }
    QComboBox* resizeBox() const { return resize_; }
    // left, right, top, bottom
    QSpinBox* insetBox(int i) const { return insets_[std::size_t(i)]; }

private:
    void check();

    QLineEdit* name_;
    QComboBox* category_;
    QLineEdit* tags_;
    QLabel* problem_;
    QDialogButtonBox* buttons_;
    QComboBox* resize_ = nullptr;
    std::array<QSpinBox*, 4> insets_{};
    Purpose purpose_;
};

} // namespace bw
