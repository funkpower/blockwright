#pragma once
// Messages with a count. The app installs no translator, and without one
// tr(s, nullptr, n) only puts n in for %n and shows line(s) as written, so
// the caller gives both English forms, each through tr, and this picks one.

#include <QString>

namespace bw {

// `one` when n is 1, else `many`, with each %n in it replaced by n.
inline QString plural(int n, const QString& one, const QString& many) {
    QString s = n == 1 ? one : many;
    return s.replace(QLatin1String("%n"), QString::number(n));
}

} // namespace bw
