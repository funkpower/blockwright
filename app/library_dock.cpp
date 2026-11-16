#include "library_dock.h"

#include "canvas_view.h"
#include "message_box.h"
#include "plural.h"
#include "sauce_dialog.h"

#include "blockwright/render.h"

#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <vector>
#include <exception>
#include <iterator>
#include <optional>

namespace bw {

namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kStyleRole = Qt::UserRole + 1;  // true on a text style's item

QString qpath(const std::filesystem::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); }

QString qtext(const std::string& utf8) { return QString::fromStdString(utf8); }

std::filesystem::path fspath(const QString& s) { return std::filesystem::path(QFile::encodeName(s).toStdString()); }

} // namespace

QByteArray stampKey(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::u8string u = p.u8string();
    return QByteArray(reinterpret_cast<const char*>(u.data()), qsizetype(u.size()));
#else
    return QByteArray::fromStdString(p.native());
#endif
}

QString frameStyleId(const std::filesystem::path& p) {
    return QStringLiteral("library:") + QString::fromLatin1(stampKey(p).toPercentEncoding("/"));
}

std::filesystem::path stampKeyPath(const QByteArray& key) {
#ifdef _WIN32
    try {
        return std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(key.constData()), size_t(key.size())));
    } catch (const std::exception&) {
        return {};
    }
#else
    return std::filesystem::path(key.toStdString());
#endif
}

namespace {

// The grid's delegate: an item's size is the grid's cell. QListView with
// uniform item sizes gives every item the size hint of its last item, so a
// short thumbnail or a one-line name there would clip the others; the cell
// is what StampGrid::fitGridToItems measures them all to need.
class CellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto* view = qobject_cast<const QListView*>(option.widget);
        if (view && view->gridSize().isValid())
            return view->gridSize();
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

} // namespace

StampGrid::StampGrid(QWidget* parent) : QListWidget(parent) {
    setViewMode(QListView::IconMode);
    setIconSize(kIconBox);
    setItemDelegate(new CellDelegate(this));
    fitGridToItems();
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setWordWrap(true);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
}

void StampGrid::fitGridToItems() {
    QStyleOptionViewItem opt;
    initViewItemOption(&opt);
    const QStyledItemDelegate natural;  // the items as the style lays them out
    // The largest item with names wrapped at `width`.
    const auto largest = [&](int width) {
        opt.rect = QRect(0, 0, width, kIconBox.height());
        QSize most(0, 0);
        for (int i = 0; i < count(); ++i)
            most = most.expandedTo(natural.sizeHint(opt, model()->index(i, 0)));
        return most;
    };
    // Names wrap at the icon box's width and a gap, or wider for a word too
    // wide to wrap; the height is measured at the width they are painted at.
    // An empty grid keeps the icon box and a line.
    const int width = std::max(kIconBox.width() + 8, largest(kIconBox.width() + 8).width());
    setGridSize(QSize(width, count() > 0 ? largest(width).height() : kIconBox.height() + fontMetrics().lineSpacing()));
}

void StampGrid::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete && e->modifiers() == Qt::NoModifier) {
        emit deletePressed();
        e->accept();
        return;
    }
    QListWidget::keyPressEvent(e);
}

void StampGrid::changeEvent(QEvent* e) {
    QListWidget::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
        fitGridToItems();
}

QStringList StampGrid::mimeTypes() const {
    return {QString::fromLatin1(kStampMimeType), QString::fromLatin1(kTextStyleMimeType)};
}

QMimeData* StampGrid::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty())
        return nullptr;
    auto* m = new QMimeData;
    m->setData(QString::fromLatin1(items.first()->data(kStyleRole).toBool() ? kTextStyleMimeType : kStampMimeType),
               items.first()->data(kPathRole).toByteArray());
    return m;
}

LibraryDock::LibraryDock(QWidget* parent) : QWidget(parent) {
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("librarySearch"));
    search_->setPlaceholderText(tr("Search names and tags"));
    search_->setClearButtonEnabled(true);
    categories_ = new QListWidget;
    categories_->setObjectName(QStringLiteral("libraryCategories"));
    categories_->setMaximumHeight(80);
    grid_ = new StampGrid;
    grid_->setObjectName(QStringLiteral("libraryGrid"));
    problemLabel_ = new QLabel;
    problemLabel_->setObjectName(QStringLiteral("libraryProblems"));
    problemLabel_->setWordWrap(true);
    auto* refresh = new QToolButton;
    refresh->setObjectName(QStringLiteral("libraryRefresh"));
    refresh->setText(tr("Refresh"));
    refresh->setToolTip(tr("Read the libraries again"));
    connect(refresh, &QToolButton::clicked, this, &LibraryDock::rescan);
    createActions();
    auto* collectionsButton = new QToolButton;
    collectionsButton->setObjectName(QStringLiteral("libraryCollections"));
    collectionsButton->setText(tr("Collections"));
    collectionsButton->setToolTip(tr("Make, rename and delete your own collections, and add stamps to them"));
    collectionsButton->setPopupMode(QToolButton::InstantPopup);
    auto* collectionsMenu = new QMenu(collectionsButton);
    collectionsMenu->addActions({newCollection_, renameCollection_, deleteCollection_, addFiles_, addFolder_});
    collectionsMenu->addSeparator();
    collectionsMenu->addActions({addToCollection_, editStamp_, removeStamp_});
    collectionsButton->setMenu(collectionsMenu);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(problemLabel_, 1);
    bottom->addWidget(collectionsButton);
    bottom->addWidget(refresh);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(search_);
    layout->addWidget(categories_);
    layout->addWidget(grid_, 1);
    layout->addLayout(bottom);

    connect(search_, &QLineEdit::textChanged, this, &LibraryDock::refill);
    connect(categories_, &QListWidget::currentRowChanged, this, &LibraryDock::refill);
    connect(grid_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) {
                const QByteArray key = item->data(kPathRole).toByteArray();
                if (item->data(kStyleRole).toBool())
                    emit textStyleActivated(key);
                else
                    emit stampActivated(key);
            });
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, &LibraryDock::showGridMenu);
    connect(grid_, &StampGrid::deletePressed, this, &LibraryDock::askRemoveStamp);
    categories_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(categories_, &QWidget::customContextMenuRequested, this, &LibraryDock::showCategoryMenu);
}

void LibraryDock::setDirectories(const std::filesystem::path& bundled, const std::filesystem::path& user) {
    bundled_ = bundled;
    user_ = user;
    rescan();
}

void LibraryDock::rescan() {
    entries_.clear();
    styles_.clear();
    frames_.clear();
    problems_.clear();
    for (const auto& [dir, bundled] : {std::pair{bundled_, true}, std::pair{user_, false}}) {
        if (dir.empty())
            continue;
        library::ScanResult r = library::scan(dir);
        for (library::Stamp& s : r.stamps) {
            QByteArray key = thumbnailKey(s);
            entries_.push_back({std::move(s), bundled, std::move(key)});
        }
        for (library::TextStyleEntry& t : r.styles) {
            QByteArray key = thumbnailKey(t);
            styles_.push_back({std::move(t), bundled, std::move(key)});
        }
        for (library::FrameStyleEntry& f : r.frames) {
            f.style.id = frameStyleId(f.path).toStdString();
            frames_.push_back(std::move(f.style));
        }
        problems_.insert(problems_.end(), r.problems.begin(), r.problems.end());
    }
    collections_.clear();
    if (!user_.empty()) {
        try {
            collections_ = library::collections(user_);
        } catch (const std::exception& e) {
            // with the scan's problems, once: the scan can't list it either
            if (std::none_of(problems_.begin(), problems_.end(),
                             [&](const library::Problem& p) { return p.path == user_; }))
                problems_.push_back({user_, e.what()});
        }
    }
    // Forget the thumbnails of entries that are gone or have changed.
    QSet<QByteArray> keys;
    for (const Entry& e : entries_)
        keys.insert(e.thumbKey);
    for (const StyleEntry& e : styles_)
        keys.insert(e.thumbKey);
    for (auto it = thumbnails_.begin(); it != thumbnails_.end();)
        it = keys.contains(it.key()) ? std::next(it) : thumbnails_.erase(it);

    // The category list: All, then each category; keep the one picked.
    const QString picked = categories_->currentRow() > 0 ? categories_->currentItem()->text() : QString();
    {
        const QSignalBlocker block(categories_);
        categories_->clear();
        categories_->addItem(tr("All"));
        for (const QString& c : categories())
            categories_->addItem(c);
        const auto found = categories_->findItems(picked, Qt::MatchExactly);
        categories_->setCurrentRow(picked.isEmpty() || found.isEmpty() ? 0 : categories_->row(found.first()));
    }

    if (problems_.empty()) {
        problemLabel_->clear();
        problemLabel_->setToolTip(QString());
    } else {
        problemLabel_->setText(
            plural(int(problems_.size()), tr("%n file skipped"), tr("%n files skipped")));
        QStringList lines;
        for (const library::Problem& p : problems_)
            lines << tr("%1: %2").arg(qpath(p.path), qtext(p.message));
        problemLabel_->setToolTip(lines.join(QLatin1Char('\n')));
    }
    refill();
    emit rescanned();
}

QStringList LibraryDock::categories() const {
    QStringList out;
    std::vector<std::string> cats;
    for (const Entry& e : entries_)
        cats.push_back(e.stamp.meta.category);
    for (const StyleEntry& e : styles_)
        cats.push_back(e.style.category);
    for (const library::Collection& c : collections_)
        cats.push_back(c.name);
    std::sort(cats.begin(), cats.end());
    cats.erase(std::unique(cats.begin(), cats.end()), cats.end());
    for (const std::string& c : cats)
        out << qtext(c);
    return out;
}

const library::Stamp* LibraryDock::findKey(const QByteArray& key) const {
    for (const Entry& e : entries_)
        if (stampKey(e.stamp.path) == key)
            return &e.stamp;
    return nullptr;
}

const library::TextStyleEntry* LibraryDock::findStyleKey(const QByteArray& key) const {
    for (const StyleEntry& e : styles_)
        if (stampKey(e.style.path) == key)
            return &e.style;
    return nullptr;
}

const library::Stamp* LibraryDock::find(const std::filesystem::path& path) const {
    for (const Entry& e : entries_)
        if (e.stamp.path == path)
            return &e.stamp;
    return nullptr;
}

QStringList LibraryDock::shownNames() const {
    QStringList out;
    for (int i = 0; i < grid_->count(); ++i)
        out << grid_->item(i)->text();
    return out;
}

QByteArray LibraryDock::thumbnailKey(const library::Stamp& s) {
    const Canvas& cv = s.art.canvas;
    // What renderCanvas reads: the glyph width, the size and each cell's
    // glyph and colours. A key in this process's cache only, so a glyph's
    // bits() will do.
    QCryptographicHash h(QCryptographicHash::Sha1);
    const int head[] = {glyphWidth(s.art), cv.width(), cv.height()};
    h.addData(QByteArrayView(reinterpret_cast<const char*>(head), sizeof head));
    for (int y = 0; y < cv.height(); ++y)
        for (int x = 0; x < cv.width(); ++x) {
            const Cell& c = cv.at(x, y);
            const Rgb fg = resolveColor(c.fg), bg = resolveColor(c.bg);
            const std::uint32_t v[] = {c.ch.bits(), std::uint32_t(fg.r), std::uint32_t(fg.g),
                                       std::uint32_t(fg.b), std::uint32_t(bg.r), std::uint32_t(bg.g),
                                       std::uint32_t(bg.b)};
            h.addData(QByteArrayView(reinterpret_cast<const char*>(v), sizeof v));
        }
    return h.result();
}

QByteArray LibraryDock::thumbnailKey(const library::TextStyleEntry& s) {
    // The sample is drawn from the style alone, so the entry's JSON (which
    // holds it) names the picture; the prefix keeps it apart from a stamp's.
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArrayView("text-style\n"));
    try {
        h.addData(QByteArray::fromStdString(library::textStyleEntryJson(s)));
    } catch (const std::exception&) {
        h.addData(stampKey(s.path));
    }
    return h.result();
}

QImage stampIcon(const Canvas& cv, int glyphWidth, QSize box, std::int64_t bandPixels) {
    const std::int64_t fw = std::int64_t(cv.width()) * glyphWidth, fh = std::int64_t(cv.height()) * 16;
    if (fw <= box.width() && fh <= box.height())
        return renderCanvas(cv, glyphWidth);
    QSize icon = QSize(int(std::min<std::int64_t>(fw, INT_MAX)), int(std::min<std::int64_t>(fh, INT_MAX)))
                     .scaled(box, Qt::KeepAspectRatio);
    icon = icon.expandedTo({1, 1});
    const int iw = icon.width(), ih = icon.height();
    // Each icon pixel is the average of the 1x pixels that fall in it, the
    // 1x picture being rendered a band of rows at a time, so only one band
    // is ever held (a 400 x 5000 stamp is 3200 x 80000 px at 1x).
    std::vector<int> column(static_cast<size_t>(fw));
    for (std::int64_t x = 0; x < fw; ++x)
        column[size_t(x)] = int(x * iw / fw);
    std::vector<std::uint64_t> sum(size_t(iw) * ih * 3, 0), count(size_t(iw) * ih, 0);
    const int rows = int(std::clamp<std::int64_t>(bandPixels / (fw * 16), 1, cv.height()));
    for (int y0 = 0; y0 < cv.height(); y0 += rows) {
        const int n = std::min(rows, cv.height() - y0);
        Canvas band(cv.width(), n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < cv.width(); ++x)
                band.at(x, y) = cv.at(x, y0 + y);
        const QImage img = renderCanvas(band, glyphWidth).convertToFormat(QImage::Format_RGB32);
        for (int py = 0; py < img.height(); ++py) {
            const std::int64_t sy = std::int64_t(y0) * 16 + py;
            const size_t row = size_t(sy * ih / fh) * iw;
            const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(py));
            for (int px = 0; px < img.width(); ++px) {
                const size_t i = row + size_t(column[size_t(px)]);
                sum[i * 3] += qRed(line[px]);
                sum[i * 3 + 1] += qGreen(line[px]);
                sum[i * 3 + 2] += qBlue(line[px]);
                ++count[i];
            }
        }
    }
    QImage out(icon, QImage::Format_RGB32);
    for (int y = 0; y < ih; ++y) {
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < iw; ++x) {
            const size_t i = size_t(y) * iw + x;
            const std::uint64_t c = std::max<std::uint64_t>(count[i], 1);
            line[x] = qRgb(int((sum[i * 3] + c / 2) / c), int((sum[i * 3 + 1] + c / 2) / c),
                           int((sum[i * 3 + 2] + c / 2) / c));
        }
    }
    return out;
}

QImage frameStyleIcon(const FrameStyle& style, int width, int height, QSize box) {
    FrameStyle s = style;
    s.minWidth = std::min(s.minWidth, width);
    s.minHeight = std::min(s.minHeight, height);
    const std::optional<Canvas> drawn =
        drawFrame(s, std::max(width, s.minimumWidth()), std::max(height, s.minimumHeight()));
    return drawn ? stampIcon(*drawn, 8, box) : QImage();
}

QImage LibraryDock::cachedThumbnail(const QByteArray& key, const std::function<QImage()>& render) {
    if (const auto it = thumbnails_.constFind(key); it != thumbnails_.constEnd())
        return *it;
    QImage img;
    try {
        img = render();
        ++renders_;
    } catch (const std::exception&) {
        return {};
    }
    thumbnails_.insert(key, img);
    return img;
}

QImage LibraryDock::thumbnail(const Entry& e) {
    return cachedThumbnail(e.thumbKey,
                           [&] { return stampIcon(e.stamp.art.canvas, glyphWidth(e.stamp.art), kIconBox); });
}

QImage LibraryDock::thumbnail(const StyleEntry& e) {
    return cachedThumbnail(e.thumbKey,
                           [&] { return stampIcon(library::textStyleSample(e.style.style, true), 8, kIconBox); });
}

void LibraryDock::setIceColours(bool on) {
    if (on == ice_)
        return;
    ice_ = on;
    refill();
}

void LibraryDock::refill() {
    const QString cat = categories_->currentRow() > 0 ? categories_->currentItem()->text() : QString();
    const std::string query = search_->text().toStdString();
    grid_->clear();
    for (const Entry& e : entries_) {
        const library::Stamp& s = e.stamp;
        if (!cat.isEmpty() && qtext(s.meta.category) != cat)
            continue;
        if (!library::matches(s, query))
            continue;
        auto* item = new QListWidgetItem(qtext(s.displayName()), grid_);
        item->setData(kPathRole, stampKey(s.path));
        const QImage img = thumbnail(e);
        if (!img.isNull())
            item->setIcon(QIcon(QPixmap::fromImage(img)));
        QStringList tags;
        for (const std::string& t : s.meta.tags)
            tags << qtext(t);
        item->setToolTip(tr("%1\n%2 x %3 cells, widest IRC row %4 bytes\nCategory: %5\nTags: %6\n%7 library: %8")
                             .arg(qtext(s.displayName()))
                             .arg(s.art.canvas.width())
                             .arg(s.art.canvas.height())
                             .arg(s.meta.ircBytes)
                             .arg(qtext(s.meta.category), tags.isEmpty() ? tr("none") : tags.join(QStringLiteral(", ")),
                                  e.bundled ? tr("Bundled") : tr("User"), qpath(s.path)));
    }
    for (const StyleEntry& e : styles_) {
        const library::TextStyleEntry& t = e.style;
        if (!cat.isEmpty() && qtext(t.category) != cat)
            continue;
        if (!library::matches(t, query))
            continue;
        auto* item = new QListWidgetItem(qtext(t.name), grid_);
        item->setData(kPathRole, stampKey(t.path));
        item->setData(kStyleRole, true);
        const bool usable = ice_ || !t.iceRequired;
        if (!usable)
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        const QImage img = thumbnail(e);
        if (!img.isNull())
            item->setIcon(QIcon(QPixmap::fromImage(img)));
        QStringList tags;
        for (const std::string& tag : t.tags)
            tags << qtext(tag);
        const QString iceNeed = !usable         ? tr("Needs iCE colours, which are off: turn on View > iCE Colours to use it")
                                : t.iceRequired ? tr("Needs iCE colours")
                                                : tr("Works with iCE colours on or off");
        item->setToolTip(tr("%1\nText style\nCategory: %2\nTags: %3\n%4\n%5 library: %6")
                             .arg(qtext(t.name), qtext(t.category),
                                  tags.isEmpty() ? tr("none") : tags.join(QStringLiteral(", ")),
                                  iceNeed, e.bundled ? tr("Bundled") : tr("User"), qpath(t.path)));
    }
    grid_->fitGridToItems();
}

// Collections (#464, PLAN §12 Collections).

namespace {

QString errorText(const std::exception& e) { return QString::fromStdString(e.what()); }

std::vector<std::string> tagList(const QStringList& tags) {
    std::vector<std::string> out;
    for (const QString& t : tags)
        out.push_back(t.trimmed().toStdString());
    return out;
}

} // namespace

void LibraryDock::createActions() {
    const auto action = [this](const QString& text, const char* name, const QString& tip, void (LibraryDock::*slot)()) {
        auto* a = new QAction(text, this);
        a->setObjectName(QString::fromLatin1(name));
        a->setToolTip(tip);
        a->setStatusTip(tip);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    newCollection_ = action(tr("&New Collection..."), "newLibraryCollection",
                            tr("Make an empty collection of your own in the user library"),
                            &LibraryDock::askNewCollection);
    renameCollection_ = action(tr("&Rename Collection..."), "renameLibraryCollection",
                               tr("Rename the collection picked in the category list, and its stamps' category"),
                               &LibraryDock::askRenameCollection);
    deleteCollection_ = action(tr("&Delete Collection..."), "deleteLibraryCollection",
                               tr("Delete the collection picked in the category list and every stamp in it"),
                               &LibraryDock::askDeleteCollection);
    addFiles_ = action(tr("Add &Files to Collection..."), "addFilesToLibraryCollection",
                       tr("Add .ans files from disk to one of your collections as stamps"),
                       &LibraryDock::askAddFiles);
    addFolder_ = action(tr("Add Fol&der to Collection..."), "addFolderToLibraryCollection",
                        tr("Add the .ans files in a folder on disk to one of your collections as stamps"),
                        &LibraryDock::askAddFolder);
    addToCollection_ = action(tr("&Add to Collection..."), "addStampToLibraryCollection",
                              tr("Copy the stamp picked in the Library into one of your collections"),
                              &LibraryDock::askAddToCollection);
    editStamp_ = action(tr("&Edit Stamp..."), "editLibraryStamp",
                        tr("Change the name, category and tags of one of your stamps"), &LibraryDock::askEditStamp);
    removeStamp_ = action(tr("Re&move Stamp..."), "removeLibraryStamp",
                          tr("Remove one of your stamps from the user library"), &LibraryDock::askRemoveStamp);
}

void LibraryDock::showGridMenu(const QPoint& pos) {
    if (QListWidgetItem* it = grid_->itemAt(pos))
        grid_->setCurrentItem(it);
    QMenu menu(this);
    menu.addActions({addToCollection_, editStamp_, removeStamp_});
    menu.addSeparator();
    menu.addActions({newCollection_, addFiles_, addFolder_});
    menu.exec(grid_->viewport()->mapToGlobal(pos));
}

void LibraryDock::showCategoryMenu(const QPoint& pos) {
    if (QListWidgetItem* it = categories_->itemAt(pos))
        categories_->setCurrentItem(it);
    QMenu menu(this);
    menu.addActions({newCollection_, renameCollection_, deleteCollection_, addFiles_, addFolder_});
    menu.exec(categories_->viewport()->mapToGlobal(pos));
}

const library::Collection* LibraryDock::findCollection(const QString& name) const {
    const std::string n = name.toStdString();
    for (const library::Collection& c : collections_)
        if (c.name == n)
            return &c;
    return nullptr;
}

const library::Collection* LibraryDock::uniqueCollection(const QString& name, QString* error) const {
    error->clear();
    const std::string n = name.toStdString();
    const auto count = std::count_if(collections_.begin(), collections_.end(),
                                     [&](const library::Collection& c) { return c.name == n; });
    if (count == 1)
        return findCollection(name);
    if (count > 1) {
        // library::findCollection's refusal names both folders
        try {
            library::findCollection(user_, n);
            *error = tr("more than one folder goes by the collection name \"%1\"").arg(name);
        } catch (const std::exception& e) {
            *error = errorText(e);
        }
    }
    return nullptr;
}

std::filesystem::path LibraryDock::stampTitled(const library::Collection& c, const std::string& title) const {
    for (const Entry& e : entries_) {
        if (e.bundled || e.stamp.name != title)
            continue;
        const std::filesystem::path rel = e.stamp.path.lexically_relative(c.dir);
        if (!rel.empty() && *rel.begin() != "..")
            return e.stamp.path;
    }
    return {};
}

QString LibraryDock::pickedCategory() const {
    return categories_->currentRow() > 0 ? categories_->currentItem()->text() : QString();
}

const LibraryDock::Entry* LibraryDock::currentEntry() const {
    const QListWidgetItem* it = grid_->currentItem();
    if (!it || !grid_->selectedItems().contains(const_cast<QListWidgetItem*>(it)) || it->data(kStyleRole).toBool())
        return nullptr;
    const QByteArray key = it->data(kPathRole).toByteArray();
    for (const Entry& e : entries_)
        if (stampKey(e.stamp.path) == key)
            return &e;
    return nullptr;
}

void LibraryDock::refused(const QString& why) { emit message(why); }

const library::Collection* LibraryDock::pickedCollection(const QString& what) {
    const QString cat = pickedCategory();
    if (cat.isEmpty()) {
        refused(tr("Pick one of your collections in the category list first; nothing %1").arg(what));
        return nullptr;
    }
    QString why;
    if (const library::Collection* c = uniqueCollection(cat, &why))
        return c;
    if (!why.isEmpty()) {
        refused(tr("%1; nothing %2").arg(why, what));
        return nullptr;
    }
    const bool bundled = std::any_of(entries_.begin(), entries_.end(),
                                     [&](const Entry& e) { return e.bundled && qtext(e.stamp.meta.category) == cat; }) ||
                         std::any_of(styles_.begin(), styles_.end(),
                                     [&](const StyleEntry& e) { return e.bundled && qtext(e.style.category) == cat; });
    refused(bundled ? tr("\"%1\" is a bundled category, not one of your collections; nothing %2").arg(cat, what)
                    : tr("\"%1\" is not one of your collections; nothing %2").arg(cat, what));
    return nullptr;
}

const LibraryDock::Entry* LibraryDock::currentUserStamp(const QString& what) {
    const QListWidgetItem* it = grid_->currentItem();
    if (it && grid_->selectedItems().contains(const_cast<QListWidgetItem*>(it)) && it->data(kStyleRole).toBool()) {
        refused(tr("%1 is a text style, not a stamp; nothing %2").arg(it->text(), what));
        return nullptr;
    }
    const Entry* e = currentEntry();
    if (!e) {
        refused(tr("Pick a stamp in the Library first; nothing %1").arg(what));
        return nullptr;
    }
    if (e->bundled) {
        refused(tr("%1 is a bundled stamp, which can't be changed; nothing %2. Add to Collection copies it into one "
                   "of your own")
                    .arg(qtext(e->stamp.displayName()), what));
        return nullptr;
    }
    return e;
}

bool LibraryDock::createCollection(const QString& name, QString* error) {
    if (user_.empty()) {
        *error = tr("there is no user library directory");
        return false;
    }
    try {
        library::createCollection(user_, name.trimmed().toStdString());
    } catch (const std::exception& e) {
        *error = errorText(e);
        rescan();
        return false;
    }
    rescan();
    emit message(tr("Made the collection \"%1\"").arg(name.trimmed()));
    return true;
}

bool LibraryDock::deleteCollection(const QString& name, QString* error, library::Removal how, bool* trashRefused) {
    if (trashRefused)
        *trashRefused = false;
    library::Removed r;
    try {
        if (!findCollection(name)) {
            *error = tr("\"%1\" is not one of your collections").arg(name);
            return false;
        }
        r = library::deleteCollection(user_, name.toStdString(), how);
    } catch (const library::TrashUnavailable& e) {
        if (trashRefused)
            *trashRefused = true;
        *error = errorText(e);
        rescan();
        return false;
    } catch (const std::exception& e) {
        *error = errorText(e);
        // what the dock showed may be gone from the disk, or changed
        rescan();
        return false;
    }
    rescan();
    if (!r.stamps)
        emit message((r.trashed ? tr("Moved the empty collection \"%1\" to the trash")
                                : tr("Deleted the empty collection \"%1\""))
                         .arg(name));
    else
        emit message((r.trashed ? tr("Moved the collection \"%1\" and %2 to the trash")
                                : tr("Deleted the collection \"%1\" and %2"))
                         .arg(name, plural(int(r.stamps), tr("its %n stamp"), tr("its %n stamps"))));
    return true;
}

bool LibraryDock::renameCollection(const QString& from, const QString& to, QString* error) {
    const QString name = to.trimmed();
    try {
        if (!findCollection(from)) {
            *error = tr("\"%1\" is not one of your collections").arg(from);
            return false;
        }
        library::renameCollection(user_, from.toStdString(), name.toStdString());
    } catch (const std::exception& e) {
        *error = errorText(e);
        // a rename that moved the folder but not every sidecar changed it
        rescan();
        return false;
    }
    // the picked collection follows its new name
    if (pickedCategory() == from) {
        const QSignalBlocker block(categories_);
        categories_->addItem(name);
        categories_->setCurrentRow(categories_->count() - 1);
    }
    rescan();
    emit message(tr("Renamed the collection \"%1\" to \"%2\"").arg(from, name));
    return true;
}

bool LibraryDock::addToCollection(const std::filesystem::path& stamp, const QString& collection, QString* error) {
    library::Added added;
    try {
        if (!findCollection(collection)) {
            *error = tr("\"%1\" is not one of your collections").arg(collection);
            return false;
        }
        added = library::addStampFile(user_, collection.toStdString(), stamp);
    } catch (const std::exception& e) {
        *error = errorText(e);
        rescan();
        return false;
    }
    rescan();
    emit message(tr("Added %1 to the collection \"%2\"").arg(fromCp437(added.name), collection));
    return true;
}

int LibraryDock::addFilesToCollection(const QString& collection, const QStringList& files, QStringList* failed,
                                      QString* refusal) {
    // the collection itself refused: nothing is tried, so no file is
    // refused; the status says why, and how many files that leaves out
    const auto refuseAll = [&](const QString& why, qsizetype n) {
        failed->clear();
        if (refusal)
            *refusal = why;
        rescan();
        QString text = tr("Nothing added: %1").arg(why);
        if (n > 0)
            text += tr("; %1 not added").arg(plural(int(n), tr("%n file"), tr("%n files")));
        emit message(text);
        return 0;
    };
    if (refusal)
        refusal->clear();
    // a folder is its own .ans files, not its subfolders' (the CLI's rule)
    QStringList paths;
    for (const QString& f : files) {
        if (QFileInfo(f).isDir()) {
            try {
                const std::vector<std::filesystem::path> found = library::folderArt(fspath(f));
                if (found.empty())
                    failed->push_back(tr("%1: no .ans files in the folder").arg(f));
                for (const std::filesystem::path& p : found)
                    paths << QString::fromStdString(p.string());
            } catch (const std::exception& e) {
                failed->push_back(tr("%1: %2").arg(f, errorText(e)));
            }
        } else {
            paths << f;
        }
    }
    if (!findCollection(collection))
        return refuseAll(tr("\"%1\" is not one of your collections").arg(collection), paths.size());
    std::vector<std::filesystem::path> list;
    list.reserve(std::size_t(paths.size()));
    for (const QString& f : paths)
        list.push_back(fspath(f));
    // One scan for the lot (library::addStamps). A batch that takes a while
    // shows its progress, keeping the window painted, and Cancel stops it
    // between files.
    QProgressDialog progress(tr("Adding files to the collection \"%1\"...").arg(collection), tr("Cancel"), 0,
                             int(list.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    std::vector<library::AddOutcome> outcomes;
    try {
        outcomes = library::addStamps(user_, collection.toStdString(), list, [&progress](std::size_t done) {
            progress.setValue(int(done));
            return !progress.wasCanceled();
        });
    } catch (const std::exception& e) {
        // addStamps refuses the collection (gone since the last scan, or
        // the bundled library) before it tries any file
        return refuseAll(errorText(e), paths.size());
    }
    int added = 0;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (outcomes[i].added)
            ++added;
        else
            failed->push_back(tr("%1: %2").arg(paths[qsizetype(i)], QString::fromStdString(outcomes[i].error)));
    }
    const int notTried = int(list.size() - outcomes.size());
    const bool cancelled = progress.wasCanceled() && notTried > 0;
    rescan();
    QString text = tr("Added %1 to the collection \"%2\"").arg(plural(added, tr("%n stamp"), tr("%n stamps")),
                                                                collection);
    if (!failed->isEmpty())
        text += tr("; %1 refused").arg(plural(int(failed->size()), tr("%n file"), tr("%n files")));
    if (cancelled)
        text += tr("; cancelled, %1 not added").arg(plural(notTried, tr("%n file"), tr("%n files")));
    emit message(text);
    return added;
}

bool LibraryDock::removeStamp(const std::filesystem::path& stamp, QString* error, library::Removal how,
                              bool* trashRefused) {
    if (trashRefused)
        *trashRefused = false;
    library::Removed r;
    try {
        r = library::removeStamp(user_, stamp, how);
    } catch (const library::TrashUnavailable& e) {
        if (trashRefused)
            *trashRefused = true;
        *error = errorText(e);
        rescan();
        return false;
    } catch (const std::exception& e) {
        *error = errorText(e);
        rescan();
        return false;
    }
    rescan();
    emit message((r.trashed ? tr("Moved %1 to the trash") : tr("Removed %1")).arg(qpath(stamp)));
    return true;
}

bool LibraryDock::editStamp(const std::filesystem::path& stamp, const QString& name, const QString& category,
                            const QStringList& tags, QString* error) {
    const library::Stamp* s = find(stamp);
    if (!s) {
        *error = tr("%1 is not a stamp in the Library").arg(qpath(stamp));
        return false;
    }
    library::StampEdit edit;
    const std::optional<std::string> title = toCp437(name.trimmed());
    if (!title) {
        *error = tr("the name holds a character CP437 has no glyph for");
        return false;
    }
    if (*title != s->name)
        edit.name = *title;
    if (category.trimmed().toStdString() != s->meta.category)
        edit.category = category.trimmed().toStdString();
    if (std::vector<std::string> t = tagList(tags); t != s->meta.tags)
        edit.tags = std::move(t);
    std::filesystem::path now;
    try {
        now = library::editStamp(user_, stamp, edit);
    } catch (const std::exception& e) {
        *error = errorText(e);
        rescan();
        return false;
    }
    rescan();
    emit message(tr("Changed %1").arg(qpath(now)));
    return true;
}

QString LibraryDock::askCollection(const QString& title) {
    QStringList names;
    for (const library::Collection& c : collections_)
        names << qtext(c.name);
    const QString picked = pickedCategory();
    const int current = std::max(0, int(names.indexOf(picked)));
    bool ok = false;
    const QString name =
        QInputDialog::getItem(this, title, tr("Collection (pick one, or type a name for a new one):"), names, current,
                              true, &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return {};
    QString why;
    if (!uniqueCollection(name, &why) && !why.isEmpty()) {
        refused(tr("No collection picked: %1").arg(why));
        return {};
    }
    if (!findCollection(name)) {
        QString error;
        if (!createCollection(name, &error)) {
            refused(tr("No collection made: %1").arg(error));
            return {};
        }
    }
    return name;
}

void LibraryDock::askNewCollection() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Collection"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;
    QString error;
    if (!createCollection(name, &error))
        refused(tr("No collection made: %1").arg(error));
}

void LibraryDock::askRenameCollection() {
    const library::Collection* c = pickedCollection(tr("renamed"));
    if (!c)
        return;
    const QString from = qtext(c->name);
    bool ok = false;
    const QString to = QInputDialog::getText(this, tr("Rename Collection"), tr("New name for \"%1\":").arg(from),
                                             QLineEdit::Normal, from, &ok);
    if (!ok || to.trimmed() == from)
        return;
    QString error;
    if (!renameCollection(from, to, &error))
        refused(tr("Collection not renamed: %1").arg(error));
}

void LibraryDock::askDeleteCollection() {
    const library::Collection* c = pickedCollection(tr("deleted"));
    if (!c)
        return;
    const QString name = qtext(c->name);
    const bool bundledToo = std::any_of(entries_.begin(), entries_.end(),
                                        [&](const Entry& e) { return e.bundled && e.stamp.meta.category == c->name; });
    QString text = c->stamps ? tr("Delete the collection \"%1\" and %2?")
                                   .arg(name, plural(int(c->stamps), tr("the %n stamp in it"), tr("the %n stamps in it")))
                             : tr("Delete the empty collection \"%1\"?").arg(name);
    text += QLatin1Char('\n') + (c->stamps ? tr("They go to the trash.") : tr("It goes to the trash."));
    if (bundledToo)
        text += QLatin1Char('\n') + tr("The bundled stamps in \"%1\" stay.").arg(name);
    if (bw::question(this, tr("Delete Collection"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes)
        return;
    const int stamps = int(c->stamps);
    QString error;
    bool noTrash = false;
    if (deleteCollection(name, &error, library::Removal::Trash, &noTrash))
        return;
    if (!noTrash) {
        refused(tr("Collection not deleted: %1").arg(error));
        return;
    }
    // The trash refused and nothing went: deleting for good is asked for
    // again, by name, default No.
    const QString again =
        stamps ? tr("The trash can't take the collection \"%1\" (%2).\n"
                    "Delete the collection \"%1\" and %3 permanently? This can't be undone.")
                     .arg(name, error, plural(stamps, tr("the %n stamp in it"), tr("the %n stamps in it")))
               : tr("The trash can't take the collection \"%1\" (%2).\n"
                    "Delete the empty collection \"%1\" permanently? This can't be undone.")
                     .arg(name, error);
    if (bw::warning(this, tr("Delete Collection"), again, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes) {
        refused(tr("Collection not deleted: the trash refused it, and nothing was removed"));
        return;
    }
    if (!deleteCollection(name, &error, library::Removal::Delete))
        refused(tr("Collection not deleted: %1").arg(error));
}

void LibraryDock::askAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add Files to Collection"), QString(),
                                                            tr("ANSI art (*.ans *.ANS);;All files (*)"));
    addPicked(tr("Add Files to Collection"), files);
}

void LibraryDock::askAddFolder() {
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add Folder to Collection"));
    if (!folder.isEmpty())
        addPicked(tr("Add Folder to Collection"), {folder});
}

void LibraryDock::addPicked(const QString& title, const QStringList& files) {
    if (files.isEmpty())
        return;
    const QString collection = askCollection(title);
    if (collection.isEmpty())
        return;
    QStringList failed;
    QString refusal;
    addFilesToCollection(collection, files, &failed, &refusal);
    if (!refusal.isEmpty())
        bw::warning(this, title, tr("Nothing was added: %1").arg(refusal));
    else if (!failed.isEmpty())
        bw::warning(this, title, tr("These files were not added:\n%1").arg(failed.join(QLatin1Char('\n'))));
}

void LibraryDock::askAddToCollection() {
    const QListWidgetItem* it = grid_->currentItem();
    if (it && grid_->selectedItems().contains(const_cast<QListWidgetItem*>(it)) && it->data(kStyleRole).toBool()) {
        refused(tr("%1 is a text style, not a stamp; nothing added").arg(it->text()));
        return;
    }
    const Entry* e = currentEntry();
    if (!e) {
        refused(tr("Pick a stamp in the Library first; nothing added"));
        return;
    }
    const std::filesystem::path path = e->stamp.path;
    const QString collection = askCollection(tr("Add to Collection"));
    if (collection.isEmpty())
        return;
    QString error;
    if (!addToCollection(path, collection, &error))
        refused(tr("Stamp not added: %1").arg(error));
}

void LibraryDock::askEditStamp() {
    const Entry* e = currentUserStamp(tr("changed"));
    if (!e)
        return;
    const library::Stamp s = e->stamp;
    StampDialog dialog(categories(), this, StampDialog::Purpose::Edit);
    dialog.nameEdit()->setText(qtext(s.displayName()));
    dialog.categoryBox()->setCurrentText(qtext(s.meta.category));
    QStringList tags;
    for (const std::string& t : s.meta.tags)
        tags << qtext(t);
    dialog.tagsEdit()->setText(tags.join(QStringLiteral(", ")));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!editStamp(s.path, dialog.name(), dialog.category(), dialog.tags(), &error))
        refused(tr("Stamp not changed: %1").arg(error));
}

void LibraryDock::askRemoveStamp() {
    const Entry* e = currentUserStamp(tr("removed"));
    if (!e)
        return;
    const std::filesystem::path path = e->stamp.path;
    const QString name = qtext(e->stamp.displayName());
    // The core puts a sidecar it already moved back when the art can't go;
    // only if that fails too is the stamp left half removed, which is said.
    // A sidecar it can't read beforehand is refused before the trash takes
    // anything (TrashUnavailable too, as Delete still takes it), so the
    // core's reason is what the box and the status give, not "the trash
    // refused".
    const std::filesystem::path side = library::pairedSidecar(path);
    const auto there = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::symlink_status(p, ec));
    };
    const bool hadSidecar = there(side);
    const auto notRemoved = [&](const QString& why) {
        refused((hadSidecar && !there(side) && there(path) ? tr("Stamp only partly removed: %1")
                                                            : tr("Stamp not removed: %1"))
                    .arg(why));
    };
    QString text = tr("Remove the stamp %1 (%2)?").arg(name, qpath(path));
    text += QLatin1Char('\n') + tr("It goes to the trash.");
    if (bw::question(this, tr("Remove Stamp"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes)
        return;
    QString error;
    bool noTrash = false;
    if (removeStamp(path, &error, library::Removal::Trash, &noTrash))
        return;
    if (!noTrash) {
        notRemoved(error);
        return;
    }
    if (bw::warning(this, tr("Remove Stamp"),
                    tr("The stamp %1 can't go to the trash: %2.\n"
                       "Delete the stamp %1 (%3) permanently? This can't be undone.")
                        .arg(name, error, qpath(path)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        notRemoved(error);
        return;
    }
    if (!removeStamp(path, &error, library::Removal::Delete))
        notRemoved(error);
}

StampDropFilter::StampDropFilter(CanvasView* view) : QObject(view), view_(view) {
    view->setAcceptDrops(true);
    view->installEventFilter(this);
}

bool StampDropFilter::eventFilter(QObject* watched, QEvent* e) {
    if (watched != view_)
        return false;
    const QString stamp = QString::fromLatin1(kStampMimeType), style = QString::fromLatin1(kTextStyleMimeType);
    switch (e->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* d = static_cast<QDragMoveEvent*>(e);
        if (!d->mimeData()->hasFormat(stamp) && !d->mimeData()->hasFormat(style))
            return false;
        // Always a copy: a move would have the grid drop the item.
        d->setDropAction(Qt::CopyAction);
        d->accept();
        return true;
    }
    case QEvent::Drop: {
        auto* d = static_cast<QDropEvent*>(e);
        const bool isStamp = d->mimeData()->hasFormat(stamp);
        if (!isStamp && !d->mimeData()->hasFormat(style))
            return false;
        d->setDropAction(Qt::CopyAction);
        d->accept();
        const QPoint cell = view_->cellAt(view_->toView(d->position()));
        if (isStamp)
            emit dropped(d->mimeData()->data(stamp), cell);
        else
            emit styleDropped(d->mimeData()->data(style), cell);
        return true;
    }
    default: return false;
    }
}

StampDialog::StampDialog(const QStringList& categories, QWidget* parent, Purpose purpose)
    : QDialog(parent), purpose_(purpose) {
    setWindowTitle(purpose == Purpose::Stamp  ? tr("Save to Library")
                   : purpose == Purpose::Edit ? tr("Edit Stamp")
                                              : tr("Save as Preset"));
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("stampName"));
    if (purpose != Purpose::Preset)
        name_->setMaxLength(int(Sauce::kTitleLen));
    category_ = new QComboBox;
    category_->setObjectName(QStringLiteral("stampCategory"));
    category_->setEditable(true);
    category_->addItems(categories);
    category_->setCurrentText(QString());
    tags_ = new QLineEdit;
    tags_->setObjectName(QStringLiteral("stampTags"));
    tags_->setPlaceholderText(tr("comma-separated, e.g. tree, pine, night"));
    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("stampProblem"));
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name_);
    form->addRow(tr("&Category:"), category_);
    form->addRow(tr("&Tags:"), tags_);
    if (purpose == Purpose::Stamp) {
        // A frame or band (stamp_slice.h): the insets are the cells kept
        // whole at each side; what lies between them repeats.
        resize_ = new QComboBox;
        resize_->setObjectName(QStringLiteral("stampResize"));
        resize_->addItems({tr("Fixed size"), tr("Width only (a band)"), tr("Width and height (a frame)")});
        form->addRow(tr("&Resize:"), resize_);
        auto* row = new QHBoxLayout;
        const QString names[4] = {tr("Left"), tr("Right"), tr("Top"), tr("Bottom")};
        for (int i = 0; i < 4; ++i) {
            auto* box = new QSpinBox;
            box->setObjectName(QStringLiteral("stampInset%1").arg(i));
            box->setRange(0, i < 2 ? kMaxColumns : kMaxRows);  // a column or a row count
            box->setValue(1);
            box->setPrefix(names[i] + QStringLiteral(" "));
            insets_[std::size_t(i)] = box;
            row->addWidget(box);
        }
        form->addRow(tr("Insets:"), row);
        const auto enable = [this] {
            const int k = resize_->currentIndex();
            for (int i = 0; i < 4; ++i)
                insets_[std::size_t(i)]->setEnabled(i < 2 ? k > 0 : k == 2);
        };
        connect(resize_, &QComboBox::currentIndexChanged, this, enable);
        enable();
    }
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(problem_);
    layout->addWidget(buttons_);

    connect(name_, &QLineEdit::textChanged, this, &StampDialog::check);
    connect(category_, &QComboBox::currentTextChanged, this, &StampDialog::check);
    connect(tags_, &QLineEdit::textChanged, this, &StampDialog::check);
    check();
}

QString StampDialog::name() const { return name_->text().trimmed(); }
QString StampDialog::category() const { return category_->currentText().trimmed(); }

QStringList StampDialog::tags() const {
    std::vector<std::string> given;
    for (const QString& t : tags_->text().split(QLatin1Char(',')))
        given.push_back(t.toStdString());
    QStringList out;
    for (const std::string& t : library::stampTags(given))
        out << QString::fromStdString(t);
    return out;
}

std::optional<StampSlice> StampDialog::slice() const {
    if (!resize_ || resize_->currentIndex() == 0)
        return std::nullopt;
    const bool frame = resize_->currentIndex() == 2;
    return StampSlice{insets_[0]->value(), insets_[1]->value(), frame ? insets_[2]->value() : 0,
                      frame ? insets_[3]->value() : 0, true, frame};
}

void StampDialog::setSlice(const std::optional<StampSlice>& s) {
    if (!resize_)
        return;
    if (!s || !s->width) {
        resize_->setCurrentIndex(0);
        return;
    }
    resize_->setCurrentIndex(s->height ? 2 : 1);
    const int v[4] = {s->left, s->right, s->top, s->bottom};
    for (int i = 0; i < 4; ++i)
        insets_[std::size_t(i)]->setValue(v[i]);
}

QString StampDialog::problem() const {
    const bool stamp = purpose_ != Purpose::Preset;
    if (name().isEmpty())
        return stamp ? tr("The stamp needs a name.") : tr("The preset needs a name.");
    if (!stamp)
        return category().isEmpty() ? tr("The preset needs a category.") : QString();
    const std::optional<std::string> b = toCp437(name());
    if (!b)
        return tr("The name holds a character CP437 has no glyph for.");
    if (b->size() > Sauce::kTitleLen)
        return tr("The name is longer than %1 characters.").arg(Sauce::kTitleLen);
    if (category().isEmpty())
        return tr("The stamp needs a category.");
    return {};
}

void StampDialog::check() {
    const QString p = problem();
    problem_->setText(p);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(p.isEmpty());
}

} // namespace bw
