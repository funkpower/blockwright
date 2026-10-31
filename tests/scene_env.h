#pragma once
// What the scene import tests need: a Python to run scenes with
// (scene::findPython: python3 on PATH, or on Windows the first of py -3,
// python and python3 that is Python 3) and ansidraw.py in BW_ANSIDRAW_DIR
// (the build's setting, empty by default),
// which tests/data/scenes' scenes import through $ANSIDRAW_DIR. Without
// either the tests skip.

#include "blockwright/scene_import.h"

#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QtGlobal>

#include <cstdlib>
#include <string>

namespace bwtest {

// Why the scene tests can't run here, or empty when they can; when they
// can, ANSIDRAW_DIR is set for the scenes they run.
inline std::string sceneSkipReason() {
#ifdef _WIN32
    try {
        bw::scene::findPython();
    } catch (const bw::scene::SceneError& e) {
        return e.what();
    }
#else
    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty())
        return "no python3 on PATH";
#endif
    const QString dir = QStringLiteral(BW_ANSIDRAW_DIR);
    if (dir.isEmpty() || !QFileInfo(dir + QStringLiteral("/ansidraw.py")).isFile())
        return "no ansidraw.py in BW_ANSIDRAW_DIR (" BW_ANSIDRAW_DIR ")";
    qputenv("ANSIDRAW_DIR", BW_ANSIDRAW_DIR);
    return {};
}

} // namespace bwtest
