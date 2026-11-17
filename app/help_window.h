#pragma once
// The help window (#210, PLAN §11): the installed help bundle's tutorials
// and manual (help_docs.h), read offline. The contents tree on the left,
// the page on the right, Back and Forward, and a search over the pages'
// titles and text, whose results take the tree's place while there is
// text to search for. Links inside the bundle open in the window; a link
// to a sample file in it (.ans, .bwp) asks the main window to open it as
// an untitled copy; any other link opens in the system's browser.

#include "help_docs.h"

#include <QHash>
#include <QWidget>

class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QUrl;

namespace bw {

class HelpWindow : public QWidget {
    Q_OBJECT
public:
    // A window of its own, kept by `parent` and closed with it.
    HelpWindow(helpdocs::Bundle bundle, QWidget* parent);

    // Shows `page` (a bundle page name, with an optional #fragment).
    void open(const QString& page);
    // The page shown, as a bundle page name with its fragment, if any.
    QString currentPage() const;
    // The pages whose title and text hold every word of `text`, case
    // folded: those whose title holds them all first, each in contents
    // order.
    QStringList search(const QString& text);
    // What a click on the link `url` (as written in the page shown) does.
    void followLink(const QUrl& url);

    const helpdocs::Bundle& bundle() const { return bundle_; }
    QTextBrowser* browser() const { return browser_; }
    QTreeWidget* contents() const { return tree_; }
    QLineEdit* searchBox() const { return search_; }
    QListWidget* results() const { return results_; }
    QToolButton* backButton() const { return back_; }
    QToolButton* forwardButton() const { return forward_; }

signals:
    // A link to a sample file in the bundle: open a copy of it.
    void fileRequested(const QString& path);

private:
    // The bundle page name of a local file URL in the bundle, or empty.
    QString pageOf(const QUrl& url) const;
    void showResults(const QString& text);
    void selectInContents(const QUrl& url);

    helpdocs::Bundle bundle_;
    QTextBrowser* browser_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* results_ = nullptr;
    QStackedWidget* side_ = nullptr;
    QToolButton* back_ = nullptr;
    QToolButton* forward_ = nullptr;
    // Each page's plain text, by page name, read on the first search.
    QHash<QString, QString> text_;
};

} // namespace bw
