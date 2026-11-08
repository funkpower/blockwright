#include "library_dock.h"

#include "canvas_view.h"
#include "plural.h"
#include "sauce_dialog.h"

#include "blockwright/render.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPixmap>
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

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(problemLabel_, 1);
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
    setWindowTitle(purpose == Purpose::Stamp ? tr("Save to Library") : tr("Save as Preset"));
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("stampName"));
    if (purpose == Purpose::Stamp)
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
    QStringList out;
    for (const QString& t : tags_->text().split(QLatin1Char(','))) {
        const QString s = t.trimmed();
        if (!s.isEmpty() && !out.contains(s))
            out << s;
    }
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
    const bool stamp = purpose_ == Purpose::Stamp;
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
