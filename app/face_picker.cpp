#include "face_picker.h"

#include "settings.h"

#include "blockwright/render.h"
#include "blockwright/text.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QListView>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QComboBox>
#include <QLabel>
#include <QPixmap>

#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>

namespace bw {

class FaceListModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;
    std::function<QVariant(int, int)> value;
    int count = 0;
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : count; }
    QVariant data(const QModelIndex& index, int role) const override {
        return index.isValid() && index.row() < count ? value(index.row(), role) : QVariant();
    }
    void reset(int n, const std::function<void()>& change) {
        beginResetModel();
        change();
        count = n;
        endResetModel();
    }
    void refresh() { if (count) emit dataChanged(index(0), index(count - 1)); }
};

namespace {

constexpr int kMarkedLabelRole = Qt::UserRole + 1;

// A fixed size avoids asking every row for decorations or glyph metrics
// during layout. QStyledItemDelegate requests data only when painting.
class FaceDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {240, std::max(40, option.fontMetrics.height() * 2 + 8)};
    }
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->text = index.data(kMarkedLabelRole).toString();
    }
};


constexpr int kPreviewHeight = 32;
constexpr int kPreviewWidth = 112;
const std::u32string kSample = U"Abc";

QIcon preview(const Face& face) {
    TextSource s;
    s.text = kSample;
    TextRender r;
    try {
        r = renderText(s, face);
    } catch (const TextError&) {
        return {};
    }
    if (r.cells.width() <= 0 || r.cells.height() <= 0)
        return {};
    const QImage img = renderCanvas(r.cells, 8);
    QPixmap pm = QPixmap::fromImage(img.scaled(QSize(kPreviewWidth, kPreviewHeight), Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
    return QIcon(pm);
}

QString charsOf(const std::u32string& chars) {
    QString out;
    for (char32_t ch : chars)
        out += QString::fromUcs4(&ch, 1);
    return out;
}

} // namespace

std::vector<FaceEntry> bundledFaceEntries() {
    std::vector<FaceEntry> out;
    for (const Face* f : bundledFaces()) {
        FaceRef ref;
        ref.kind = FaceRef::Kind::Pixel;
        ref.name = std::string(f->name());
        out.push_back({ref, ref.name, f, {}, "pixel"});
    }
    return out;
}

FacePicker::FacePicker(std::vector<FaceEntry> entries, QWidget* parent) : QWidget(parent) {
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("faceSearch"));
    search_->setPlaceholderText(tr("Search faces"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search faces"));
    search_->setToolTip(tr("Search face names, file names and types"));
    favourite_ = new QToolButton;
    favourite_->setObjectName(QStringLiteral("faceFavourite"));
    favourite_->setCheckable(true);
    favourite_->setText(QStringLiteral("☆"));
    type_ = new QComboBox;
    type_->setObjectName(QStringLiteral("faceType"));
    type_->setAccessibleName(tr("Font type"));
    type_->setToolTip(tr("Show all faces or only one font type"));
    type_->addItem(tr("All types"), QString());
    for (const char* type : {"pixel", "outline", "block", "colour"})
        type_->addItem(QString::fromLatin1(type), QString::fromLatin1(type));
    scanning_ = new QLabel(tr("Scanning fonts…"));
    scanning_->setObjectName(QStringLiteral("fontsScanning"));
    scanning_->hide();
    list_ = new QListView;
    model_ = new FaceListModel(this);
    model_->value = [this](int row, int role) { return rowData(row, role); };
    list_->setModel(model_);
    list_->setItemDelegate(new FaceDelegate(list_));
    list_->setObjectName(QStringLiteral("faceList"));
    list_->setIconSize(QSize(kPreviewWidth, kPreviewHeight));
    list_->setUniformItemSizes(true);
    list_->setMinimumHeight(3 * (kPreviewHeight + 8));
    list_->setToolTip(tr("The face the text is set in; a face is marked with the characters of the text it lacks "
                         "and those that misread for each other in it"));

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(search_, 1);
    top->addWidget(favourite_);
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->addLayout(top);
    // The type filter has a row of its own: beside the search field it makes the Text tool's options wider than the
    // default left column.
    box->addWidget(type_);
    box->addWidget(scanning_);
    box->addWidget(list_);

    const auto settings = appSettings();
    const auto favourites = settings->value(favouritesKey()).toStringList();
    favourites_ = QSet<QString>(favourites.begin(), favourites.end());

    connect(search_, &QLineEdit::textChanged, this, &FacePicker::rebuild);
    connect(type_, &QComboBox::currentIndexChanged, this, &FacePicker::rebuild);
    connect(favourite_, &QToolButton::toggled, this, [this](bool on) { setFavourite(current_, on); });
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        const int i = rows_[size_t(index.row())];
        if (entries_[size_t(i)].ref == current_)
            return;
        current_ = entries_[size_t(i)].ref;
        updateFavouriteButton();
        emit picked(current_);
    });

    setEntries(entries.empty() ? bundledFaceEntries() : std::move(entries));
}

QString FacePicker::favouritesKey() { return QStringLiteral("text/favouriteFaces"); }

QString FacePicker::keyOf(const FaceRef& ref) {
    const QString kind = QString::fromUtf8(faceKindName(ref.kind));
    if (ref.kind == FaceRef::Kind::Tdf)
        return QStringLiteral("%1:%2:%3:%4").arg(kind, QString::fromStdString(ref.file)).arg(ref.index).arg(
            QString::fromStdString(ref.name));
    return kind + QLatin1Char(':') + QString::fromStdString(ref.name);
}

const FaceEntry* FacePicker::find(const FaceRef& ref) const {
    for (const FaceEntry& e : entries_)
        if (e.ref == ref)
            return &e;
    return nullptr;
}

void FacePicker::setEntries(std::vector<FaceEntry> entries) {
    const auto same = [](const FaceEntry& a, const FaceEntry& b) {
        return a.ref == b.ref && a.displayName == b.displayName && a.face == b.face && a.fontFile == b.fontFile && a.type == b.type;
    };
    if (std::equal(entries.begin(), entries.end(), entries_.begin(), entries_.end(), same))
        return;
    // the previews are kept: a key names the same face (a TheDraw font's
    // by its file's hash)
    entries_ = std::move(entries);
    keys_.clear();
    searchText_.clear();
    for (const auto& e : entries_) {
        keys_ << keyOf(e.ref);
        searchText_ << QString::fromStdString(e.displayName + " " + e.type);
    }
    rebuild();
}

void FacePicker::setCurrent(const FaceRef& ref) {
    if (ref == current_)
        return;
    current_ = ref;
    selectCurrent();
    updateFavouriteButton();
}

void FacePicker::setText(std::u32string text) {
    if (text == text_)
        return;
    text_ = std::move(text);
    labels_.clear();
    model_->refresh();
}

bool FacePicker::isFavourite(const FaceRef& ref) const { return favourites_.contains(keyOf(ref)); }

void FacePicker::setFavourite(const FaceRef& ref, bool on) {
    if (on == isFavourite(ref))
        return;
    if (on)
        favourites_.insert(keyOf(ref));
    else
        favourites_.remove(keyOf(ref));
    const auto settings = appSettings();
    settings->setValue(favouritesKey(), QStringList(favourites_.begin(), favourites_.end()));
    rebuild();
}

QString FacePicker::missingIn(const FaceRef& ref) const {
    const FaceEntry* e = find(ref);
    if (!e || text_.empty())
        return {};
    const Face* face = e->resolvedFace();
    if (!face)
        return {};
    // each character once, looked up in the face: no layout, so no raster
    std::u32string missing;
    std::unordered_set<char32_t> seen;
    for (const char32_t ch : text_)
        if (ch != U'\n' && seen.insert(ch).second && !face->glyph(ch))
            missing += ch;
    return charsOf(missing);
}

QString FacePicker::confusablesIn(const FaceRef& ref) const {
    const FaceEntry* e = find(ref);
    if (!e || text_.empty())
        return {};
    const Face* face = e->resolvedFace();
    if (!face)
        return {};
    QStringList pairs;
    for (const ConfusablePair& p : bw::confusablesIn(*face, text_))
        pairs << charsOf({p.a, p.b});
    return pairs.join(QLatin1Char(' '));
}

QStringList FacePicker::listed() const {
    QStringList out;
    for (int i : rows_)
        out << QString::fromStdString(entries_[size_t(i)].displayName);
    return out;
}

void FacePicker::setScanning(bool on) { scanning_->setVisible(on); }
bool FacePicker::scanning() const { return !scanning_->isHidden(); }

void FacePicker::selectCurrent() {
    const QSignalBlocker block(list_->selectionModel());
    QModelIndex selected;
    for (int row = 0; row < int(rows_.size()); ++row)
        if (entries_[size_t(rows_[size_t(row)])].ref == current_) {
            selected = model_->index(row);
            break;
        }
    list_->setCurrentIndex(selected);
}

void FacePicker::rebuild() {
    const QString filter = search_->text().trimmed();
    const QString type = type_->currentData().toString();
    std::vector<int> order;
    for (int i = 0; i < int(entries_.size()); ++i)
        if ((type.isEmpty() || QString::fromStdString(entries_[size_t(i)].type) == type) &&
            (filter.isEmpty() || searchText_[i].contains(filter, Qt::CaseInsensitive)))
            order.push_back(i);
    std::stable_partition(order.begin(), order.end(), [this](int i) { return favourites_.contains(keys_[i]); });
    const QSignalBlocker block(list_->selectionModel());
    const int count = int(order.size());
    model_->reset(count, [this, &order] { rows_ = std::move(order); labels_.clear(); });
    selectCurrent();
    updateFavouriteButton();
}

QVariant FacePicker::rowData(int row, int role) const {
    const int i = rows_[size_t(row)];
    const FaceEntry& e = entries_[size_t(i)];
    if (role == Qt::UserRole)
        return i;
    if (role == Qt::DecorationRole) {
        const QString& key = keys_[i];
        if (const auto* icon = previews_.object(key))
            return *icon;
        const Face* face = e.resolvedFace();
        auto* icon = new QIcon(face ? preview(*face) : QIcon());
        const QIcon result = *icon;
        previews_.insert(key, icon);
        ++previewsBuilt_;
        return result;
    }
    // Text queries (type-ahead and accessibility included) need only metadata.
    if (role == Qt::DisplayRole) {
        QString label = QString::fromStdString(e.displayName);
        if (isFavourite(e.ref))
            label.prepend(QStringLiteral("★ "));
        return label;
    }
    if (role == kMarkedLabelRole || role == Qt::ToolTipRole) {
        if (!labels_.contains(i)) {
            QString label = QString::fromStdString(e.displayName);
            if (isFavourite(e.ref))
                label.prepend(QStringLiteral("★ "));
            QStringList marks;
            const QString missing = missingIn(e.ref), confusable = confusablesIn(e.ref);
            if (!missing.isEmpty())
                marks << tr("missing %1").arg(missing);
            if (!confusable.isEmpty())
                marks << tr("confusable %1").arg(confusable);
            if (!marks.isEmpty())
                label += QLatin1Char('\n') + marks.join(QStringLiteral(" · "));
            labels_.insert(i, label);
        }
        return labels_.value(i);
    }
    return {};
}

void FacePicker::updateFavouriteButton() {
    const QSignalBlocker block(favourite_);
    const bool on = isFavourite(current_);
    favourite_->setChecked(on);
    favourite_->setText(on ? QStringLiteral("★") : QStringLiteral("☆"));
    favourite_->setToolTip(on ? tr("Remove the face from the favourites") : tr("Add the face to the favourites"));
}

} // namespace bw
