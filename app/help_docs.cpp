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

// An attribute's value as written in either quote, with the character
// references HTML decodes in it (&amp;, &quot;, &#39;, &#x27; and the like).
QString attributeValue(const QRegularExpressionMatch& m, int dq, int sq, int uq) {
    QString v = m.capturedStart(dq) >= 0 ? m.captured(dq) : m.capturedStart(sq) >= 0 ? m.captured(sq) : m.captured(uq);
    if (!v.contains(QLatin1Char('&')))
        return v;
    static const QHash<QString, QString> named = {
        {QStringLiteral("amp"), QStringLiteral("&")},  {QStringLiteral("quot"), QStringLiteral("\"")},
        {QStringLiteral("apos"), QStringLiteral("'")}, {QStringLiteral("lt"), QStringLiteral("<")},
        {QStringLiteral("gt"), QStringLiteral(">")},   {QStringLiteral("nbsp"), QString(QChar(0xa0))},
    };
    static const QRegularExpression ref(QStringLiteral("&(?:#([0-9]{1,7})|#[xX]([0-9A-Fa-f]{1,6})|([A-Za-z]+));"));
    QString out;
    qsizetype at = 0;
    for (auto it = ref.globalMatch(v); it.hasNext();) {
        const auto r = it.next();
        out += v.mid(at, r.capturedStart() - at);
        at = r.capturedEnd();
        char32_t c = 0;
        if (r.capturedStart(1) >= 0)
            c = r.captured(1).toUInt();
        else if (r.capturedStart(2) >= 0)
            c = r.captured(2).toUInt(nullptr, 16);
        if (r.capturedStart(3) >= 0 && named.contains(r.captured(3)))
            out += named.value(r.captured(3));
        else if (c > 0 && c <= 0x10ffff)
            out += QString::fromUcs4(&c, 1);
        else
            out += r.captured(0);
    }
    return out + v.mid(at);
}

// The ids and names a page's anchors go to.
QSet<QString> anchorsIn(const QString& html) {
    static const QRegularExpression re(QStringLiteral(R"re(\s(?:id|name)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))re"),
                                         QRegularExpression::CaseInsensitiveOption);
    QSet<QString> out;
    for (auto it = re.globalMatch(html); it.hasNext();)
        out.insert(attributeValue(it.next(), 1, 2, 3));
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
    const QJsonValue contents = o.value(QStringLiteral("contents"));
    if (!contents.isArray() || contents.toArray().isEmpty()) {
        why = QCoreApplication::translate("bw::helpdocs", "%1 has no contents").arg(QDir::toNativeSeparators(file));
        return {};
    }
    for (const QJsonValue& sv : contents.toArray()) {
        Section s;
        Page head;
        if (!page(sv.toObject(), &head))
            return {};
        s.title = head.title;
        s.page = head.page;
        const QJsonValue pages = sv.toObject().value(QStringLiteral("pages"));
        if (!pages.isUndefined() && !pages.isArray()) {
            why = QCoreApplication::translate("bw::helpdocs", "%1 has a section whose pages aren't a list")
                      .arg(QDir::toNativeSeparators(file));
            return {};
        }
        for (const QJsonValue& pv : pages.toArray()) {
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

QUrl websiteUrl(const QString& page) {
    // The page is a path under the website, never a URL of its own: a name
    // like "evil:x.html" must not become a scheme.
    const qsizetype hash = page.indexOf(QLatin1Char('#'));
    QString path = hash < 0 ? page : page.left(hash);
    while (path.startsWith(QLatin1Char('/')))
        path.remove(0, 1);
    QUrl url(QString::fromLatin1(kWebsite));
    url.setPath(url.path() + path);
    if (hash >= 0)
        url.setFragment(page.mid(hash + 1));
    return url;
}

QString toolPage(const QString& toolId) {
    const auto it = toolAnchors().find(toolId);
    return it == toolAnchors().end() ? QString() : QStringLiteral("manual/04-tools.html#") + *it;
}

QStringList check(const Bundle& bundle, const QStringList& toolIds) {
    QStringList problems;
    if (!bundle.valid())
        return {QStringLiteral("no bundle")};
    const QDir root(bundle.dir);
    const QString realRoot = QFileInfo(bundle.dir).canonicalFilePath();
    QHash<QString, QSet<QString>> anchors;  // by page, read once
    const auto anchorsOf = [&](const QString& page) -> const QSet<QString>& {
        auto it = anchors.find(page);
        if (it == anchors.end())
            it = anchors.insert(page, anchorsIn(readText(root.filePath(page))));
        return *it;
    };
    // `target` ("page#fragment", relative to the bundle, decoded) names a
    // file in it and, when it has a fragment, an anchor in that page.
    const auto resolves = [&](const QString& target) {
        const qsizetype hash = target.indexOf(QLatin1Char('#'));
        const QString page = hash < 0 ? target : target.left(hash);
        if (!isPageName(page))
            return false;
        // As the help window reads it: a file whose real path is under the
        // bundle's, so a symlink out of it is no page.
        const QFileInfo file(root.filePath(page));
        const QString real = file.canonicalFilePath();
        if (!file.isFile() || real.isEmpty() || !real.startsWith(realRoot + QLatin1Char('/')))
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

    static const QRegularExpression link(QStringLiteral(R"re(\s(href|src)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))re"),
                                       QRegularExpression::CaseInsensitiveOption);
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
            // As the browser reads it: character references, then the
            // percent-encoding of the path and the fragment.
            const QString target = attributeValue(m, 2, 3, 4);
            const QString written = m.capturedStart(2) >= 0 ? m.captured(2)
                                    : m.capturedStart(3) >= 0 ? m.captured(3)
                                                              : m.captured(4);
            // The help window opens only http and https links, so a page
            // holding any other scheme (mailto:, javascript:, ftp:) or a
            // scheme-relative //host is refused; an http(s) link is the browser's.
            const auto sm = scheme.match(target);
            if (sm.hasMatch() || target.startsWith(QLatin1String("//"))) {
                const QString name = sm.hasMatch() ? sm.captured(0).chopped(1).toLower() : QString();
                if (name != QLatin1String("http") && name != QLatin1String("https"))
                    problems << QStringLiteral("%1: %2=\"%3\" isn't an http or https link, the only schemes the help "
                                               "window opens")
                                    .arg(p.page, m.captured(1).toLower(), written);
                continue;
            }
            const qsizetype hash = target.indexOf(QLatin1Char('#'));
            const QString path = QUrl::fromPercentEncoding((hash < 0 ? target : target.left(hash)).toUtf8());
            const QString fragment = hash < 0 ? QString() : QUrl::fromPercentEncoding(target.mid(hash + 1).toUtf8());
            QString resolved = path.isEmpty() ? p.page
                                              : QDir::cleanPath(dir == QLatin1String(".") ? path
                                                                                          : dir + QLatin1Char('/') + path);
            if (hash >= 0)
                resolved += QLatin1Char('#') + fragment;
            if (!resolves(resolved))
                problems << QStringLiteral("%1: %2=\"%3\" doesn't resolve")
                                .arg(p.page, m.captured(1).toLower(), written);
        }
    }
    return problems;
}

} // namespace bw::helpdocs
