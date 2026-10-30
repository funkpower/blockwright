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
// keyed by the entry's JSON. A text style isn't dragged and isn't placed:
// activating one emits textStyleActivated, which nothing applies yet (PLAN
// §15, As built). A style that needs iCE colours is shown disabled while the
// document has them off (setIceColours); its thumbnail is still the iCE look.
//
// StampDialog is Selection > Save to Library...: name, category and tags.

#include "blockwright/library.h"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QListWidget>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace bw {

class CanvasView;

inline constexpr char kStampMimeType[] = "application/x-blockwright-stamp";

// A stamp's id in the dock, its drag payload and stampActivated: the .ans
// path's native bytes (UTF-16 as UTF-8 on Windows), so a name that isn't
// UTF-8 never collides with another the way two decoded QStrings would.
QByteArray stampKey(const std::filesystem::path& p);
// Empty if `key` can't be decoded (not UTF-8, on Windows).
std::filesystem::path stampKeyPath(const QByteArray& key);

// The grid's icon box.
inline constexpr QSize kIconBox(96, 64);

// A stamp's icon: `cv` rendered at 1x (render/), as is if it fits `box`,
// else scaled down to fit (keeping its aspect) by averaging the 1x pixels
// under each icon pixel. The 1x picture is rendered a band of rows (about
// `bandPixels` pixels) at a time, so a large stamp never holds its whole 1x
// image; the result is the same whatever the band size.
QImage stampIcon(const Canvas& cv, int glyphWidth, QSize box, std::int64_t bandPixels = std::int64_t(1) << 22);

// The thumbnail grid: an icon-mode list whose drags carry the stamp's path.
class StampGrid : public QListWidget {
    Q_OBJECT
public:
    explicit StampGrid(QWidget* parent = nullptr);

protected:
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

signals:
    // A stamp was double-clicked or activated with Enter (its stampKey).
    void stampActivated(const QByteArray& key);
    // A text style was double-clicked or activated with Enter (the stampKey
    // of its .style.json).
    void textStyleActivated(const QByteArray& key);

private:
    void refill();
    static QByteArray thumbnailKey(const library::Stamp& s);
    static QByteArray thumbnailKey(const library::TextStyleEntry& s);
    QImage thumbnail(const Entry& e);
    QImage thumbnail(const StyleEntry& e);
    QImage cachedThumbnail(const QByteArray& key, const std::function<QImage()>& render);

    std::filesystem::path bundled_, user_;
    std::vector<Entry> entries_;
    std::vector<StyleEntry> styles_;
    std::vector<library::Problem> problems_;
    QHash<QByteArray, QImage> thumbnails_;
    int renders_ = 0;
    bool ice_ = true;

    QLineEdit* search_;
    QListWidget* categories_;
    StampGrid* grid_;
    QLabel* problemLabel_;
};

// Watches the canvas view for stamps dragged from the dock and says which
// cell each was dropped on. The view is told to accept drops.
class StampDropFilter : public QObject {
    Q_OBJECT
public:
    explicit StampDropFilter(CanvasView* view);

signals:
    void dropped(const QByteArray& key, QPoint cell);  // a stampKey

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
    // most a SAUCE title) or a text style preset (Save as Preset: any name).
    enum class Purpose { Stamp, Preset };

    StampDialog(const QStringList& categories, QWidget* parent = nullptr, Purpose purpose = Purpose::Stamp);

    QString name() const;
    QString category() const;
    QStringList tags() const;
    // Why the fields can't be saved, or empty.
    QString problem() const;

    QLineEdit* nameEdit() const { return name_; }
    QComboBox* categoryBox() const { return category_; }
    QLineEdit* tagsEdit() const { return tags_; }
    QDialogButtonBox* buttons() const { return buttons_; }

private:
    void check();

    QLineEdit* name_;
    QComboBox* category_;
    QLineEdit* tags_;
    QLabel* problem_;
    QDialogButtonBox* buttons_;
    Purpose purpose_;
};

} // namespace bw
