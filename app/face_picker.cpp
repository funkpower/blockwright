#include "face_picker.h"

#include "settings.h"

#include "blockwright/render.h"
#include "blockwright/text.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_set>

namespace bw {

namespace {

// Previews drawn as the list is built; the rest are drawn a batch at a
// time once the event loop is idle, so a large fonts folder lists at once.
constexpr int kPreviewsAtOnce = 48;
constexpr int kPreviewBatch = 16;
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
        out.push_back({ref, ref.name, f});
    }
    return out;
}

FacePicker::FacePicker(std::vector<FaceEntry> entries, QWidget* parent) : QWidget(parent) {
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("faceSearch"));
    search_->setPlaceholderText(tr("Search faces"));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(tr("Show only the faces whose names hold this"));
    favourite_ = new QToolButton;
    favourite_->setObjectName(QStringLiteral("faceFavourite"));
    favourite_->setCheckable(true);
    favourite_->setText(QStringLiteral("☆"));
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("faceList"));
    list_->setIconSize(QSize(kPreviewWidth, kPreviewHeight));
    list_->setUniformItemSizes(false);
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
    box->addWidget(list_);

    const auto settings = appSettings();
    favourites_ = settings->value(favouritesKey()).toStringList();

    connect(search_, &QLineEdit::textChanged, this, &FacePicker::rebuild);
    connect(favourite_, &QToolButton::toggled, this, [this](bool on) { setFavourite(current_, on); });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        const int i = item->data(Qt::UserRole).toInt();
        if (i < 0 || i >= int(entries_.size()) || entries_[size_t(i)].ref == current_)
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
        return a.ref == b.ref && a.displayName == b.displayName && a.face == b.face;
    };
    if (std::equal(entries.begin(), entries.end(), entries_.begin(), entries_.end(), same))
        return;
    // the previews are kept: a key names the same face (a TheDraw font's
    // by its file's hash)
    entries_ = std::move(entries);
    rebuild();
}

void FacePicker::setCurrent(const FaceRef& ref) {
    if (ref == current_)
        return;
    current_ = ref;
    rebuild();
}

void FacePicker::setText(std::u32string text) {
    if (text == text_)
        return;
    text_ = std::move(text);
    rebuild();
}

bool FacePicker::isFavourite(const FaceRef& ref) const { return favourites_.contains(keyOf(ref)); }

void FacePicker::setFavourite(const FaceRef& ref, bool on) {
    if (on == isFavourite(ref))
        return;
    if (on)
        favourites_.append(keyOf(ref));
    else
        favourites_.removeAll(keyOf(ref));
    const auto settings = appSettings();
    settings->setValue(favouritesKey(), favourites_);
    rebuild();
}

QString FacePicker::missingIn(const FaceRef& ref) const {
    const FaceEntry* e = find(ref);
    if (!e || !e->face || text_.empty())
        return {};
    // each character once, looked up in the face: no layout, so no raster
    std::u32string missing;
    std::unordered_set<char32_t> seen;
    for (const char32_t ch : text_)
        if (ch != U'\n' && seen.insert(ch).second && !e->face->glyph(ch))
            missing += ch;
    return charsOf(missing);
}

QString FacePicker::confusablesIn(const FaceRef& ref) const {
    const FaceEntry* e = find(ref);
    if (!e || !e->face || text_.empty())
        return {};
    QStringList pairs;
    for (const ConfusablePair& p : bw::confusablesIn(*e->face, text_))
        pairs << charsOf({p.a, p.b});
    return pairs.join(QLatin1Char(' '));
}

QStringList FacePicker::listed() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i) {
        const int e = list_->item(i)->data(Qt::UserRole).toInt();
        out << QString::fromStdString(entries_[size_t(e)].displayName);
    }
    return out;
}

void FacePicker::rebuild() {
    const QString filter = search_->text().trimmed();
    std::vector<int> order;
    for (int i = 0; i < int(entries_.size()); ++i)
        if (filter.isEmpty() || QString::fromStdString(entries_[size_t(i)].displayName).contains(filter, Qt::CaseInsensitive))
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return isFavourite(entries_[size_t(a)].ref) && !isFavourite(entries_[size_t(b)].ref);
    });

    const QSignalBlocker block(list_);
    list_->clear();
    int drawn = 0;
    bool waiting = false;
    for (int i : order) {
        const FaceEntry& e = entries_[size_t(i)];
        const QString name = QString::fromStdString(e.displayName);
        QString label = name;
        if (isFavourite(e.ref))
            label = QStringLiteral("★ ") + label;
        QStringList marks, tips;
        const QString missing = missingIn(e.ref), confusable = confusablesIn(e.ref);
        if (!missing.isEmpty()) {
            marks << tr("missing %1").arg(missing);
            tips << tr("Can't draw: %1").arg(missing);
        }
        if (!confusable.isEmpty()) {
            marks << tr("confusable %1").arg(confusable);
            tips << tr("Misread for each other: %1").arg(confusable);
        }
        if (!marks.isEmpty())
            label += QLatin1Char('\n') + marks.join(QStringLiteral(" · "));
        auto icon = previews_.find(keyOf(e.ref));
        if (icon == previews_.end() && drawn < kPreviewsAtOnce) {
            icon = previews_.insert(keyOf(e.ref), e.face ? preview(*e.face) : QIcon());
            ++drawn;
        }
        waiting = waiting || icon == previews_.end();
        auto* item = new QListWidgetItem(icon == previews_.end() ? QIcon() : *icon, label);
        item->setData(Qt::UserRole, i);
        item->setToolTip(tips.isEmpty() ? name : name + QLatin1Char('\n') + tips.join(QLatin1Char('\n')));
        list_->addItem(item);
        if (e.ref == current_)
            list_->setCurrentItem(item);
    }
    updateFavouriteButton();
    if (waiting && !previewsQueued_) {
        previewsQueued_ = true;
        QTimer::singleShot(0, this, &FacePicker::drawPreviews);
    }
}

void FacePicker::drawPreviews() {
    previewsQueued_ = false;
    int drawn = 0;
    for (int row = 0; row < list_->count(); ++row) {
        QListWidgetItem* item = list_->item(row);
        const FaceEntry& e = entries_[size_t(item->data(Qt::UserRole).toInt())];
        const QString key = keyOf(e.ref);
        if (previews_.contains(key))
            continue;
        if (drawn == kPreviewBatch) {
            previewsQueued_ = true;
            QTimer::singleShot(0, this, &FacePicker::drawPreviews);
            return;
        }
        item->setIcon(*previews_.insert(key, e.face ? preview(*e.face) : QIcon()));
        ++drawn;
    }
}

void FacePicker::updateFavouriteButton() {
    const QSignalBlocker block(favourite_);
    const bool on = isFavourite(current_);
    favourite_->setChecked(on);
    favourite_->setText(on ? QStringLiteral("★") : QStringLiteral("☆"));
    favourite_->setToolTip(on ? tr("Remove the face from the favourites") : tr("Add the face to the favourites"));
}

} // namespace bw
