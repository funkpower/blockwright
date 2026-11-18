#pragma once
// The tutorials and the manual offline (#210, PLAN §11): a help bundle the
// docs repo builds from the website's Markdown, installed to
// <datadir>/blockwright/help/. It holds the site's pages without its menu,
// header and footer, at the site's own paths (tutorials/…html,
// manual/…html, their images and sample files), and bundle.json:
//
//   {"format": 1,
//    "docsCommit": "abc1234",          the docs commit it was built from
//    "checkedWith": "0.1.1 (bd67fae)", the Blockwright it was checked against
//    "contents": [                     the contents tree, in order
//      {"title": "Tutorials", "page": "tutorials/index.html",
//       "pages": [{"title": "What ANSI art is",
//                  "page": "tutorials/01-what-ansi-art-is.html"}, …]},
//      …]}
//
// A page is named by its path in the bundle, which is its path on the
// website too, so with no bundle installed the same name, fragment and all,
// opens on https://blockwright.pumpin.online/.

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace bw::helpdocs {

inline constexpr const char* kWebsite = "https://blockwright.pumpin.online/";
// The pages Help > Getting Started, Help > Tutorials and Help > Manual open.
inline constexpr const char* kGettingStartedPage = "tutorials/01-what-ansi-art-is.html";
inline constexpr const char* kTutorialsPage = "tutorials/index.html";
inline constexpr const char* kManualPage = "manual/index.html";

struct Page {
    QString title;
    QString page;
};

struct Section {
    QString title;
    QString page;
    QList<Page> pages;
};

struct Bundle {
    QString dir;  // absolute and clean; empty when there is no bundle
    QString docsCommit;
    QString checkedWith;
    QList<Section> contents;

    bool valid() const { return !dir.isEmpty(); }
    // Every page the contents name, each section's own first, in order.
    QList<Page> pages() const;
    // The page's title from the contents; empty for a page they don't name.
    QString title(const QString& page) const;
};

// Where the installed bundle is: <datadir>/blockwright/help/ from the real
// binary's directory, as the bundled samples and Library are found.
QString installedDir();
// Reads `dir`/bundle.json. A bundle that is missing, isn't format 1, has no
// contents (`contents` missing, not an array or empty) or names a page
// outside the bundle gives an invalid Bundle and says why in `error`.
Bundle read(const QString& dir, QString* error = nullptr);

// A page name ("manual/04-tools.html#fill") on the website.
QUrl websiteUrl(const QString& page);
// The manual page and anchor of a tool id's section
// ("manual/04-tools.html#half-block-brush"); empty for an unknown id.
QString toolPage(const QString& toolId);

// What is wrong with the bundle: a page the contents name that is missing,
// an internal link or image of a page that doesn't resolve to a file in
// the bundle or to an anchor in its page, and a page the app opens
// (the three Help pages and every tool's section) that doesn't exist.
// An `href` or `src` is read as a browser reads it: in double or single
// quotes, its character references (&amp;) decoded, then its path and
// fragment percent-decoded. Empty when nothing is.
QStringList check(const Bundle& bundle, const QStringList& toolIds);

} // namespace bw::helpdocs
