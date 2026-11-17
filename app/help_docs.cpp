#include "help_docs.h"

#include "blockwright/library.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <filesystem>

#ifndef BW_HELP_FROM_BINDIR
#define BW_HELP_FROM_BINDIR "../share/blockwright/help"
#endif

namespace bw::helpdocs {

namespace {

std::filesystem::path fspath(const QString& s) { return std::filesystem::path(QFile::encodeName(s).toStdString()); }

// The tool sections of manual/04-tools.html, by tool id: the anchors the
// docs build gives the headings ("Line, Rectangle and Ellipse" is one).
const QHash<QString, QString>& toolAnchors() {
    static const QHash<QString, QString> anchors = {
        {QStringLiteral("keyboard"), QStringLiteral("keyboard")},
        {QStringLiteral("select"), QStringLiteral("select")},
        {QStringLiteral("move"), QStringLiteral("movetransform")},
        {QStringLiteral("halfBlock"), QStringLiteral("half-block-brush")},
        {QStringLiteral("shade"), QStringLiteral("shade-brush")},
        {QStringLiteral("glyph"), QStringLiteral("glyph-brush")},
        {QStringLiteral("colorize"), QStringLiteral("colorize-brush")},
        {QStringLiteral("rampShade"), QStringLiteral("ramp-shade-brush")},
        {QStringLiteral("form"), QStringLiteral("form-brush")},
        {QStringLiteral("soft"), QStringLiteral("soft-brush")},
        {QStringLiteral("despeckle"), QStringLiteral("despeckle-brush")},
        {QStringLiteral("smudge"), QStringLiteral("smudge-brush")},
        {QStringLiteral("outline"), QStringLiteral("outline-brush")},
        {QStringLiteral("rimLight"), QStringLiteral("rim-light-brush")},
        {QStringLiteral("texture"), QStringLiteral("texture-stamp")},
        {QStringLiteral("clone"), QStringLiteral("clone-stamp")},
        {QStringLiteral("spray"), QStringLiteral("glyph-spray")},
        {QStringLiteral("line"), QStringLiteral("line-rectangle-and-ellipse")},
        {QStringLiteral("rectangle"), QStringLiteral("line-rectangle-and-ellipse")},
        {QStringLiteral("ellipse"), QStringLiteral("line-rectangle-and-ellipse")},
        {QStringLiteral("frame"), QStringLiteral("frame")},
        {QStringLiteral("fill"), QStringLiteral("fill")},
        {QStringLiteral("gradient"), QStringLiteral("ramp-gradient")},
        {QStringLiteral("eyedropper"), QStringLiteral("eyedropper")},
        {QStringLiteral("text"), QStringLiteral("text")},
    };
    return anchors;
}

// A page name as the bundle holds it: relative, '/' between directories,
// no "..", ".", empty part or backslash. Its fragment is not part of it.
bool isPageName(const QString& page) {
    if (page.isEmpty() || page.startsWith(QLatin1Char('/')) || page.contains(QLatin1Char('\\')) ||
        page.contains(QLatin1Char('#')) || page.contains(QLatin1Char(':')))
        return false;
    for (const QString& part : page.split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    return true;
}

QString readText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// The ids and names a page's anchors go to.
QSet<QString> anchorsIn(const QString& html) {
    static const QRegularExpression re(QStringLiteral(R"re(\s(?:id|name)="([^"]*)")re"));
    QSet<QString> out;
    for (auto it = re.globalMatch(html); it.hasNext();)
        out.insert(it.next().captured(1));
    return out;
}

} // namespace

QList<Page> Bundle::pages() const {
    QList<Page> out;
    for (const Section& s : contents) {
        out.append({s.title, s.page});
        out.append(s.pages);
    }
    return out;
}

QString Bundle::title(const QString& page) const {
    for (const Page& p : pages())
        if (p.page == page)
            return p.title;
    return QString();
}

QString installedDir() {
    const std::filesystem::path binDir = library::realBinDir(fspath(QCoreApplication::applicationFilePath()),
                                                             fspath(QCoreApplication::applicationDirPath()));
    const std::filesystem::path dir = (binDir / BW_HELP_FROM_BINDIR).lexically_normal();
    return QFile::decodeName(QByteArray::fromStdString(dir.string()));
}

Bundle read(const QString& dir, QString* error) {
    QString ignored;
    QString& why = error ? *error : ignored;
    const QString file = QDir(dir).filePath(QStringLiteral("bundle.json"));
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        why = QCoreApplication::translate("bw::helpdocs", "no %1").arg(QDir::toNativeSeparators(file));
        return {};
    }
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (!doc.isObject()) {
        why = QCoreApplication::translate("bw::helpdocs", "%1 isn't a JSON object").arg(QDir::toNativeSeparators(file));
        return {};
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("format")).toInt() != 1) {
        why = QCoreApplication::translate("bw::helpdocs", "%1 isn't help bundle format 1").arg(QDir::toNativeSeparators(file));
        return {};
    }
    Bundle b;
    b.docsCommit = o.value(QStringLiteral("docsCommit")).toString();
    b.checkedWith = o.value(QStringLiteral("checkedWith")).toString();
    if (b.docsCommit.isEmpty() || b.checkedWith.isEmpty()) {
        why = QCoreApplication::translate("bw::helpdocs", "%1 doesn't say which docs commit and Blockwright it holds")
                  .arg(QDir::toNativeSeparators(file));
        return {};
    }
    const auto page = [&](const QJsonObject& e, Page* out) {
        out->title = e.value(QStringLiteral("title")).toString();
        out->page = e.value(QStringLiteral("page")).toString();
        if (out->title.isEmpty() || !isPageName(out->page)) {
            why = QCoreApplication::translate("bw::helpdocs", "%1 names a page without a title or outside the bundle")
                      .arg(QDir::toNativeSeparators(file));
            return false;
        }
        return true;
    };
    for (const QJsonValue& sv : o.value(QStringLiteral("contents")).toArray()) {
        Section s;
        Page head;
        if (!page(sv.toObject(), &head))
            return {};
        s.title = head.title;
        s.page = head.page;
        for (const QJsonValue& pv : sv.toObject().value(QStringLiteral("pages")).toArray()) {
            Page p;
            if (!page(pv.toObject(), &p))
                return {};
            s.pages.append(p);
        }
        b.contents.append(s);
    }
    b.dir = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    return b;
}

QUrl websiteUrl(const QString& page) { return QUrl(QString::fromLatin1(kWebsite)).resolved(QUrl(page)); }

QString toolPage(const QString& toolId) {
    const auto it = toolAnchors().find(toolId);
    return it == toolAnchors().end() ? QString() : QStringLiteral("manual/04-tools.html#") + *it;
}

QStringList check(const Bundle& bundle, const QStringList& toolIds) {
    QStringList problems;
    if (!bundle.valid())
        return {QStringLiteral("no bundle")};
    const QDir root(bundle.dir);
    QHash<QString, QSet<QString>> anchors;  // by page, read once
    const auto anchorsOf = [&](const QString& page) -> const QSet<QString>& {
        auto it = anchors.find(page);
        if (it == anchors.end())
            it = anchors.insert(page, anchorsIn(readText(root.filePath(page))));
        return *it;
    };
    // `target` ("page#fragment", relative to the bundle) names a file in it
    // and, when it has a fragment, an anchor in that page.
    const auto resolves = [&](const QString& target) {
        const qsizetype hash = target.indexOf(QLatin1Char('#'));
        const QString page = hash < 0 ? target : target.left(hash);
        if (!isPageName(page) || !QFileInfo(root.filePath(page)).isFile())
            return false;
        return hash < 0 || anchorsOf(page).contains(target.mid(hash + 1));
    };

    for (const char* p : {kGettingStartedPage, kTutorialsPage, kManualPage})
        if (!resolves(QString::fromLatin1(p)))
            problems << QStringLiteral("the app opens %1, which isn't in the bundle").arg(QString::fromLatin1(p));
    for (const QString& id : toolIds) {
        const QString page = toolPage(id);
        if (page.isEmpty())
            problems << QStringLiteral("the tool %1 has no section in the manual").arg(id);
        else if (!resolves(page))
            problems << QStringLiteral("the tool %1's section %2 isn't in the bundle").arg(id, page);
    }

    static const QRegularExpression link(QStringLiteral(R"re(\s(href|src)="([^"]*)")re"));
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    for (const Page& p : bundle.pages()) {
        const QString file = root.filePath(p.page);
        if (!QFileInfo(file).isFile()) {
            problems << QStringLiteral("%1 is in the contents but not in the bundle").arg(p.page);
            continue;
        }
        const QString html = readText(file);
        const QString dir = QFileInfo(p.page).path();
        for (auto it = link.globalMatch(html); it.hasNext();) {
            const auto m = it.next();
            const QString target = m.captured(2);
            if (scheme.match(target).hasMatch() || target.startsWith(QLatin1String("//")))
                continue;  // the browser's
            QString resolved;
            if (target.startsWith(QLatin1Char('#')))
                resolved = p.page + target;
            else {
                const qsizetype hash = target.indexOf(QLatin1Char('#'));
                const QString path = hash < 0 ? target : target.left(hash);
                resolved = QDir::cleanPath(dir == QLatin1String(".") ? path : dir + QLatin1Char('/') + path);
                if (hash >= 0)
                    resolved += target.mid(hash);
            }
            if (!resolves(resolved))
                problems << QStringLiteral("%1: %2=\"%3\" doesn't resolve").arg(p.page, m.captured(1), target);
        }
    }
    return problems;
}

} // namespace bw::helpdocs
