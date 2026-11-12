#include "start_screen.h"

#include "main_window.h"
#include "message_box.h"
#include "recent_files.h"
#include "settings.h"

#include "blockwright/ansi.h"
#include "blockwright/render.h"
#include "blockwright/version.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <vector>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>

namespace bw {

namespace {

const QString kShowKey = QStringLiteral("startScreen/showAtStartup");
// The logo panel's margin on each side, which the logo's scale has to fit.
constexpr int kLogoMargin = 16;
// The recent list scrolls past this height, so ten entries don't push the
// screen past a 720-high screen.
constexpr int kRecentHeight = 190;
// A sample's title is cut short past this width, its whole title in the
// tooltip.
constexpr int kSampleTitleWidth = 260;

std::optional<AnsiDocument> readLogo(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QByteArray bytes = f.readAll();
    try {
        AnsiDocument doc = parseAnsi(std::string_view(bytes.constData(), size_t(bytes.size())));
        if (doc.canvas.width() == 0 || doc.canvas.height() == 0)
            return std::nullopt;
        return doc;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QImage renderLogo(const AnsiDocument& doc, int scale) {
    try {
        RenderOptions opts;
        opts.scale = scale;
        return renderDocument(doc, opts);
    } catch (const std::exception&) {
        return {};
    }
}

// A scroll area that asks for a fixed height, whatever it holds: the recent
// list's, and the samples'.
class RecentScroll : public QScrollArea {
public:
    QSize sizeHint() const override { return {QScrollArea::sizeHint().width(), kRecentHeight}; }
};

// The samples' list: no horizontal scrollbar, and as wide as its entries
// with room kept for the vertical one, so however many there are the
// scrollbar covers none of them, and the screen can't squeeze them.
class SamplesScroll : public RecentScroll {
public:
    SamplesScroll() { setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); }
    QSize sizeHint() const override {
        return RecentScroll::sizeHint() + QSize(verticalScrollBar()->sizeHint().width(), 0);
    }
    QSize minimumSizeHint() const override { return {sizeHint().width(), RecentScroll::minimumSizeHint().height()}; }
};

QLabel* heading(const QString& text) {
    auto* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

} // namespace

bool startScreenAtStartup() { return appSettings()->value(kShowKey, true).toBool(); }

void setStartScreenAtStartup(bool on) { appSettings()->setValue(kShowKey, on); }

QImage renderStartLogo(const QString& path) {
    const auto doc = readLogo(path);
    return doc ? renderLogo(*doc, 1) : QImage();
}

int startLogoScale(QSize logo, QSize rest, QSize available) {
    const int width = std::max(rest.width(), 2 * logo.width() + 2 * kLogoMargin);
    const int height = rest.height() + 2 * logo.height();
    return width <= available.width() && height <= available.height() - 60 ? 2 : 1;
}

StartScreen::StartScreen(MainWindow* window, const Options& options)
    : QDialog(options.launch ? nullptr : window), window_(window), launch_(options.launch) {
    setObjectName(QStringLiteral("startScreen"));
    // A launch screen has no parent to take it down with the window.
    if (launch_)
        connect(window, &QObject::destroyed, this, [this] { delete this; });
    setWindowTitle(tr("Blockwright"));
    // Anything that opens or makes a document while the screen is up closes
    // it, whatever started it: its own buttons, File > Open Recent, or a file
    // the system hands the app (a QFileOpenEvent on macOS). A cancelled or
    // failed open leaves the document, and the screen, as they were.
    connect(window->document(), &Document::reset, this, [this] {
        if (!closing_)
            accept();
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // The logo and version on black.
    auto* panel = new QWidget;
    panel->setObjectName(QStringLiteral("startLogoPanel"));
    panel->setAutoFillBackground(true);
    QPalette pal = panel->palette();
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::WindowText, QColor(0xaa, 0xaa, 0xaa));
    panel->setPalette(pal);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(kLogoMargin, kLogoMargin, kLogoMargin, 12);
    auto* logo = new QLabel;
    logo->setAlignment(Qt::AlignCenter);
    const auto doc = readLogo(options.logoPath);
    const QImage logo1x = doc ? renderLogo(*doc, 1) : QImage();
    if (logo1x.isNull()) {
        // No logo yet, or one that doesn't parse: the name in text.
        logo->setObjectName(QStringLiteral("startWordmark"));
        logo->setText(tr("Blockwright"));
        QFont f = logo->font();
        f.setPointSizeF(f.pointSizeF() * 3);
        f.setBold(true);
        logo->setFont(f);
        QPalette lp = logo->palette();
        lp.setColor(QPalette::WindowText, Qt::white);
        logo->setPalette(lp);
    } else {
        logo->setObjectName(QStringLiteral("startLogo"));
        logo->setPixmap(QPixmap::fromImage(logo1x));
        logoScale_ = 1;
    }
    panelLayout->addWidget(logo, 0, Qt::AlignHCenter);
    auto* version = new QLabel(QStringLiteral("blockwright " BLOCKWRIGHT_VERSION " (" BLOCKWRIGHT_GIT_COMMIT ")"));
    version->setObjectName(QStringLiteral("startVersion"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panelLayout->addWidget(version, 0, Qt::AlignHCenter);
    root->addWidget(panel);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(16, 16, 16, 16);
    body->setSpacing(24);
    root->addLayout(body);
    // Each column spaced as a dialog is, not as far apart as the columns.
    actions_ = new QVBoxLayout;
    actions_->setSpacing(6);
    body->addLayout(actions_);
    // The sections side by side, so one more doesn't make the screen taller.
    sections_ = new QHBoxLayout;
    sections_->setSpacing(24);
    body->addLayout(sections_, 1);

    // New: its size and iCE setting, then the button.
    actions_->addWidget(heading(tr("New")));
    auto* grid = new QGridLayout;
    width_ = new QSpinBox;
    width_->setObjectName(QStringLiteral("startWidth"));
    width_->setRange(1, kMaxColumns);
    width_->setValue(kStartNewWidth);
    height_ = new QSpinBox;
    height_->setObjectName(QStringLiteral("startHeight"));
    height_->setRange(1, kMaxRows);
    height_->setValue(kStartNewHeight);
    auto* wl = new QLabel(tr("&Width:"));
    wl->setBuddy(width_);
    auto* hl = new QLabel(tr("&Height:"));
    hl->setBuddy(height_);
    grid->addWidget(wl, 0, 0);
    grid->addWidget(width_, 0, 1);
    grid->addWidget(hl, 0, 2);
    grid->addWidget(height_, 0, 3);
    actions_->addLayout(grid);
    auto* presets = new QHBoxLayout;
    for (const QSize s : {QSize(80, 25), QSize(80, 40), QSize(80, 50)}) {
        auto* b = new QPushButton(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()));
        b->setObjectName(QStringLiteral("startPreset%1x%2").arg(s.width()).arg(s.height()));
        b->setAutoDefault(false);
        b->setToolTip(tr("A canvas %1 columns wide and %2 rows high").arg(s.width()).arg(s.height()));
        connect(b, &QPushButton::clicked, this, [this, s] {
            width_->setValue(s.width());
            height_->setValue(s.height());
        });
        presets->addWidget(b);
    }
    actions_->addLayout(presets);
    ice_ = new QCheckBox(tr("&iCE colours (16 background colours, no blink)"));
    ice_->setObjectName(QStringLiteral("startIce"));
    ice_->setChecked(true);
    actions_->addWidget(ice_);
    QPushButton* create = addAction(QStringLiteral("startNew"), tr("&New"), {QKeySequence(Qt::Key_N)},
                                    [this] { newCanvas(); });
    // Enter, as well as N, unless another button has the focus: Enter runs
    // that one (each is autoDefault), as Space does.
    create->setDefault(true);
    create->setToolTip(tr("A new canvas of this size (N or Enter)"));
    actions_->addSpacing(8);

    QPushButton* open = addAction(QStringLiteral("startOpen"), tr("&Open..."),
                                  {QKeySequence(Qt::Key_O), QKeySequence(QKeySequence::Open)}, [this] { openFile(); });
    open->setToolTip(tr("An .ans, XBIN or BIN file, or a .bwp project (O, %1)")
                         .arg(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText)));
    QPushButton* dir = addAction(QStringLiteral("startOpenDirectory"), tr("Open Project &Directory..."), {},
                                 [this] { openDirectory(); });
    dir->setToolTip(tr("A project saved as a directory"));
    actions_->addStretch(1);
    auto* show = new QCheckBox(tr("&Show at startup"));
    show->setObjectName(QStringLiteral("startShowAtStartup"));
    show->setChecked(startScreenAtStartup());
    connect(show, &QCheckBox::toggled, this, [](bool on) { setStartScreenAtStartup(on); });
    actions_->addWidget(show);

    // Recent.
    auto* scroll = new RecentScroll;
    scroll->setObjectName(QStringLiteral("startRecent"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    recentBox_ = new QWidget;
    recent_ = new QVBoxLayout(recentBox_);
    recent_->setContentsMargins(0, 0, 0, 0);
    recent_->setSpacing(2);
    scroll->setWidget(recentBox_);
    addSection(tr("Recent"), scroll);
    refreshRecent();

    // Samples, when any came with the app: each opens as an untitled copy.
    // The list scrolls in the recent list's height, and a long title is cut
    // short, so however many there are the screen keeps its size.
    const std::vector<samples::Sample> samples = MainWindow::bundledSamples();
    if (!samples.empty()) {
        auto* scroll = new SamplesScroll;
        scroll->setObjectName(QStringLiteral("startSamples"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* box = new QWidget;
        auto* list = new QVBoxLayout(box);
        list->setContentsMargins(0, 0, 0, 0);
        list->setSpacing(2);
        for (const samples::Sample& sample : samples) {
            const QString path = QFile::decodeName(QByteArray::fromStdString(sample.path.string()));
            const QString title = QString::fromStdString(sample.title);
            // Elided before '&' is doubled, so the width is the text's own.
            QString shown = fontMetrics().elidedText(title, Qt::ElideRight, kSampleTitleWidth);
            const bool cut = shown != title;
            auto* entry = new QPushButton(shown.replace(QLatin1Char('&'), QStringLiteral("&&")));
            entry->setObjectName(QStringLiteral("startSampleEntry"));
            entry->setProperty("path", path);
            entry->setAutoDefault(true);
            entry->setFlat(true);
            entry->setStyleSheet(QStringLiteral("text-align: left; padding: 2px 6px;"));
            entry->setEnabled(sample.problem.empty());
            const QString tip = sample.problem.empty()
                                    ? QString::fromStdString(sample.summary)
                                    : tr("Can't be read: %1").arg(QString::fromStdString(sample.problem));
            entry->setToolTip(cut ? title + QLatin1Char('\n') + tip : tip);
            connect(entry, &QPushButton::clicked, this, [this, path] { openSample(path); });
            list->addWidget(entry);
        }
        list->addStretch(1);
        scroll->setWidget(box);
        addSection(tr("Samples"), scroll, 0);
    }

    // The logo at 2x when the screen has room for it.
    if (logoScale_) {
        QSize available = options.available;
        if (available.isEmpty())
            available = (window && window->screen() ? window->screen() : QGuiApplication::primaryScreen())
                            ->availableGeometry()
                            .size();
        const QSize hint = sizeHint();
        const QSize rest(hint.width(), hint.height() - logo1x.height());
        if (startLogoScale(logo1x.size(), rest, available) == 2) {
            const QImage logo2x = renderLogo(*doc, 2);
            if (!logo2x.isNull()) {
                logo->setPixmap(QPixmap::fromImage(logo2x));
                logoScale_ = 2;
            }
        }
    }
    // A launch screen has no window under it to come up over: it is centred
    // on the window's screen, the one its logo was sized for and the window
    // shows on, not on whichever screen the pointer is on.
    if (launch_) {
        QScreen* on = window->screen() ? window->screen() : QGuiApplication::primaryScreen();
        const QRect area = on->availableGeometry();
        // Its top-left kept on the work area, title bar and all, when it is
        // taller or wider than the area.
        const QPoint at = QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, sizeHint(), area).topLeft();
        move(std::max(at.x(), area.left()), std::max(at.y(), area.top()));
    }
    create->setFocus();
}

QPushButton* StartScreen::addAction(const QString& name, const QString& text, const QList<QKeySequence>& keys,
                                    const std::function<void()>& run) {
    auto* b = new QPushButton(text);
    b->setObjectName(name);
    b->setAutoDefault(true);
    connect(b, &QPushButton::clicked, this, run);
    for (const QKeySequence& k : keys) {
        auto* s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, run);
    }
    actions_->addWidget(b);
    return b;
}

void StartScreen::addSection(const QString& title, QWidget* body, int stretch) {
    auto* column = new QVBoxLayout;
    column->setSpacing(6);
    column->addWidget(heading(title));
    column->addWidget(body, 1);
    sections_->addLayout(column, stretch);
}

void StartScreen::refreshRecent() {
    while (QLayoutItem* item = recent_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            // Out of the screen now; deleted later, as its Remove button may
            // be what called this.
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    const QStringList list = recentFiles();
    if (list.isEmpty()) {
        auto* none = new QLabel(tr("Nothing opened or saved yet"));
        none->setObjectName(QStringLiteral("startRecentEmpty"));
        none->setEnabled(false);
        recent_->addWidget(none);
    }
    for (const QString& path : list) {
        const QFileInfo fi(path);
        const bool there = fi.exists();
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("startRecentRow"));
        row->setProperty("path", path);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* entry = new QPushButton(fi.fileName().isEmpty() ? path : fi.fileName());
        entry->setObjectName(QStringLiteral("startRecentEntry"));
        entry->setProperty("path", path);
        entry->setAutoDefault(true);
        entry->setFlat(true);
        entry->setStyleSheet(QStringLiteral("text-align: left; padding: 2px 6px;"));
        entry->setEnabled(there);
        entry->setToolTip(there ? QDir::toNativeSeparators(path)
                                : tr("%1 is missing").arg(QDir::toNativeSeparators(path)));
        connect(entry, &QPushButton::clicked, this, [this, path] { openRecent(path); });
        h->addWidget(entry, 1);
        auto* where = new QLabel;
        where->setEnabled(false);
        where->setText(there ? fontMetrics().elidedText(QDir::toNativeSeparators(fi.absolutePath()), Qt::ElideMiddle,
                                                        200)
                             : tr("missing"));
        h->addWidget(where);
        if (!there) {
            // A push button, so Enter on it removes (a QToolButton would
            // pass Enter on to New).
            auto* remove = new QPushButton(tr("Remove"));
            remove->setObjectName(QStringLiteral("startRecentRemove"));
            remove->setAutoDefault(true);
            remove->setToolTip(tr("Take %1 off the recent list").arg(QDir::toNativeSeparators(path)));
            connect(remove, &QPushButton::clicked, this, [this, path] {
                removeRecentFile(path);
                refreshRecent();
            });
            h->addWidget(remove);
        }
        recent_->addWidget(row);
    }
    recent_->addStretch(1);
}

// Each action closes the screen through Document::reset (see the
// constructor) when it opens or makes a document.
void StartScreen::newCanvas() {
    QString error;
    if (!window_->newCanvas(QSize(width_->value(), height_->value()), ice_->isChecked(), &error))
        bw::warning(this, tr("New"), tr("Could not make the canvas:\n%1").arg(error));
}

void StartScreen::openFile() { window_->open(); }

void StartScreen::openDirectory() { window_->openDirectory(); }

void StartScreen::openRecent(const QString& path) { window_->openRequested(path); }

void StartScreen::openSample(const QString& path) { window_->openSample(path); }

void StartScreen::done(int result) {
    if (closing_)
        return;
    // Esc or closing it: a new default canvas, as a start without the
    // screen. Only a screen still over its own blank document gets here
    // rejected: anything opened while it was up has closed it already.
    closing_ = true;
    if (result == QDialog::Rejected)
        window_->document()->clear();
    // At launch the window shows now, before the screen goes, so the app
    // never has no window open and quits.
    if (launch_)
        window_->show();
    QDialog::done(result);
    closing_ = false;
}

StartScreen* startUp(MainWindow* window, const QString& file) {
    if (!file.isEmpty() || !startScreenAtStartup()) {
        window->show();
        if (!file.isEmpty())
            window->openInteractive(file);
        return nullptr;
    }
    StartScreen::Options options;
    options.launch = !window->isVisible();
    auto* screen = new StartScreen(window, options);
    screen->setAttribute(Qt::WA_DeleteOnClose);
    screen->open();
    return screen;
}

} // namespace bw
