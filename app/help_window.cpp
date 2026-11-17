#include "help_window.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace bw {

namespace {

constexpr int kPageRole = Qt::UserRole;

QString pageWithoutFragment(const QString& page) {
    const qsizetype hash = page.indexOf(QLatin1Char('#'));
    return hash < 0 ? page : page.left(hash);
}

} // namespace

HelpWindow::HelpWindow(helpdocs::Bundle bundle, QWidget* parent)
    : QWidget(parent, Qt::Window), bundle_(std::move(bundle)) {
    setObjectName(QStringLiteral("helpWindow"));
    setWindowTitle(tr("Blockwright Help"));

    back_ = new QToolButton;
    back_->setObjectName(QStringLiteral("helpBack"));
    back_->setText(tr("Back"));
    back_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    back_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    back_->setShortcut(QKeySequence::Back);
    back_->setEnabled(false);
    forward_ = new QToolButton;
    forward_->setObjectName(QStringLiteral("helpForward"));
    forward_->setText(tr("Forward"));
    forward_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    forward_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    forward_->setShortcut(QKeySequence::Forward);
    forward_->setEnabled(false);
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("helpSearch"));
    search_->setPlaceholderText(tr("Search the tutorials and the manual"));
    search_->setClearButtonEnabled(true);
    auto* bar = new QHBoxLayout;
    bar->addWidget(back_);
    bar->addWidget(forward_);
    bar->addWidget(search_, 1);

    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("helpContents"));
    tree_->setHeaderHidden(true);
    for (const helpdocs::Section& s : bundle_.contents) {
        auto* top = new QTreeWidgetItem(tree_, {s.title});
        top->setData(0, kPageRole, s.page);
        for (const helpdocs::Page& p : s.pages)
            (new QTreeWidgetItem(top, {p.title}))->setData(0, kPageRole, p.page);
        top->setExpanded(true);
    }
    results_ = new QListWidget;
    results_->setObjectName(QStringLiteral("helpResults"));
    side_ = new QStackedWidget;
    side_->addWidget(tree_);
    side_->addWidget(results_);

    browser_ = new QTextBrowser;
    browser_->setObjectName(QStringLiteral("helpPage"));
    // Every link goes through followLink: some open here, some elsewhere.
    browser_->setOpenLinks(false);

    auto* split = new QSplitter;
    split->addWidget(side_);
    split->addWidget(browser_);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 660});
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(split, 1);
    resize(900, 640);

    connect(back_, &QToolButton::clicked, browser_, &QTextBrowser::backward);
    connect(forward_, &QToolButton::clicked, browser_, &QTextBrowser::forward);
    connect(browser_, &QTextBrowser::backwardAvailable, back_, &QToolButton::setEnabled);
    connect(browser_, &QTextBrowser::forwardAvailable, forward_, &QToolButton::setEnabled);
    connect(browser_, &QTextBrowser::anchorClicked, this, &HelpWindow::followLink);
    connect(browser_, &QTextBrowser::sourceChanged, this, &HelpWindow::selectInContents);
    const auto openItem = [this](QTreeWidgetItem* item) { open(item->data(0, kPageRole).toString()); };
    connect(tree_, &QTreeWidget::itemClicked, this, openItem);
    connect(tree_, &QTreeWidget::itemActivated, this, openItem);
    const auto openResult = [this](QListWidgetItem* item) { open(item->data(kPageRole).toString()); };
    connect(results_, &QListWidget::itemClicked, this, openResult);
    connect(results_, &QListWidget::itemActivated, this, openResult);
    connect(search_, &QLineEdit::textChanged, this, &HelpWindow::showResults);
}

void HelpWindow::open(const QString& page) {
    const qsizetype hash = page.indexOf(QLatin1Char('#'));
    QUrl url = QUrl::fromLocalFile(QDir(bundle_.dir).filePath(pageWithoutFragment(page)));
    if (hash >= 0)
        url.setFragment(page.mid(hash + 1));
    browser_->setSource(url);
}

QString HelpWindow::pageOf(const QUrl& url) const {
    if (!url.isLocalFile())
        return QString();
    const QString path = QDir::cleanPath(url.toLocalFile());
    const QString root = bundle_.dir + QLatin1Char('/');
    return path.startsWith(root) ? path.mid(root.size()) : QString();
}

QString HelpWindow::currentPage() const {
    const QUrl url = browser_->source();
    const QString page = pageOf(url);
    return page.isEmpty() || !url.hasFragment() ? page : page + QLatin1Char('#') + url.fragment();
}

void HelpWindow::followLink(const QUrl& url) {
    const QUrl to = browser_->source().resolved(url);
    if (!to.isLocalFile()) {
        QDesktopServices::openUrl(to);
        return;
    }
    // Nothing outside the bundle opens from a page.
    const QString page = pageOf(to);
    if (page.isEmpty())
        return;
    const QString suffix = QFileInfo(page).suffix().toLower();
    if (suffix == QLatin1String("ans") || suffix == QLatin1String("bwp"))
        emit fileRequested(to.toLocalFile());
    else if (suffix == QLatin1String("html") || suffix == QLatin1String("htm"))
        browser_->setSource(to);
    else
        QDesktopServices::openUrl(to);
}

QStringList HelpWindow::search(const QString& text) {
    const QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};
    const auto holdsAll = [&](const QString& s) {
        for (const QString& w : words)
            if (!s.contains(w, Qt::CaseInsensitive))
                return false;
        return true;
    };
    QStringList byTitle, byText;
    for (const helpdocs::Page& p : bundle_.pages()) {
        if (byTitle.contains(p.page) || byText.contains(p.page))
            continue;
        if (holdsAll(p.title)) {
            byTitle << p.page;
            continue;
        }
        auto it = text_.find(p.page);
        if (it == text_.end()) {
            QFile f(QDir(bundle_.dir).filePath(p.page));
            QTextDocument doc;
            if (f.open(QIODevice::ReadOnly))
                doc.setHtml(QString::fromUtf8(f.readAll()));
            it = text_.insert(p.page, doc.toPlainText());
        }
        if (holdsAll(p.title + QLatin1Char('\n') + *it))
            byText << p.page;
    }
    return byTitle + byText;
}

void HelpWindow::showResults(const QString& text) {
    results_->clear();
    if (text.trimmed().isEmpty()) {
        side_->setCurrentWidget(tree_);
        return;
    }
    const QStringList found = search(text);
    for (const QString& page : found) {
        auto* item = new QListWidgetItem(bundle_.title(page), results_);
        item->setData(kPageRole, page);
    }
    if (found.isEmpty()) {
        auto* none = new QListWidgetItem(tr("Nothing found"), results_);
        none->setFlags(Qt::NoItemFlags);
    }
    side_->setCurrentWidget(results_);
}

// Called with the page being shown: source() may still be the last one.
void HelpWindow::selectInContents(const QUrl& url) {
    const QString page = pageOf(url);
    const QString title = bundle_.title(page);
    setWindowTitle(title.isEmpty() ? tr("Blockwright Help") : tr("%1 - Blockwright Help").arg(title));
    const QSignalBlocker block(tree_);
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->data(0, kPageRole).toString() == page) {
            tree_->setCurrentItem(*it);
            return;
        }
    tree_->clearSelection();
}

} // namespace bw
