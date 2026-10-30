#pragma once
// What the scene import tests need: python3 on PATH and ansidraw.py in
// BW_ANSIDRAW_DIR (the build's setting, empty
// tools by default), which tests/data/scenes' scenes import through
// $ANSIDRAW_DIR. Without either the tests skip.

#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

#include <cstdlib>
#include <string>

namespace bwtest {

// Why the scene tests can't run here, or empty when they can; when they
// can, ANSIDRAW_DIR is set for the scenes they run.
inline std::string sceneSkipReason() {
    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty())
        return "no python3 on PATH";
    const QString dir = QStringLiteral(BW_ANSIDRAW_DIR);
    if (dir.isEmpty() || !QFileInfo(dir + QStringLiteral("/ansidraw.py")).isFile())
        return "no ansidraw.py in BW_ANSIDRAW_DIR (" BW_ANSIDRAW_DIR ")";
    ::setenv("ANSIDRAW_DIR", BW_ANSIDRAW_DIR, 1);
    return {};
}

} // namespace bwtest
