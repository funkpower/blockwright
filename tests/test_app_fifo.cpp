// #453: the app's Open of a flat file and its Open Reference Image read
// through one handle checked by fstat (bw::readRegular), not QFile, whose
// open of a FIFO waits for a writer.

#include "document.h"
#include "main_window.h"

#include "fifo_guard.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace bw;

namespace {

std::filesystem::path native(const QString& p) { return std::filesystem::path(QFile::encodeName(p).toStdString()); }

// Document::read's refusal of `path`, or "(read)".
std::string openRefusal(const QString& path) {
    AnsiDocument doc;
    QString error;
    return Document::read(path, &doc, &error) ? std::string("(read)") : error.toStdString();
}

} // namespace

TEST_CASE("Open refuses a FIFO or a directory at a flat file's path promptly and still opens a file", "[app][io]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString plain = dir.filePath(QStringLiteral("x.ans"));
    {
        QFile f(plain);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("\x1b[0;31mhi\x1b[0m\r\n");
    }
    CHECK(openRefusal(plain) == "(read)");
    const QString folder = dir.filePath(QStringLiteral("d.ans"));
    REQUIRE(QDir().mkdir(folder));
    CHECK(openRefusal(folder) == folder.toStdString() + " is a directory");
    const QString missing = dir.filePath(QStringLiteral("nope.ans"));
    // a missing or unreadable file in the system's words, as QFile gave them
    CHECK(openRefusal(missing) == "No such file or directory");
#if defined(__unix__) || defined(__APPLE__)
    const QString fifo = dir.filePath(QStringLiteral("pipe.ans"));
    REQUIRE(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    CHECK(bwtest::within(native(fifo), [&] { return openRefusal(fifo); }) ==
          fifo.toStdString() + " is a FIFO, not a regular file");
    CHECK(openRefusal(QStringLiteral("/dev/null")) == "/dev/null is a device, not a regular file");
    const QString locked = dir.filePath(QStringLiteral("locked.ans"));
    QFile::copy(plain, locked);
    REQUIRE(QFile::setPermissions(locked, QFileDevice::Permissions()));
    if (::access(QFile::encodeName(locked).constData(), R_OK) != 0)
        CHECK(openRefusal(locked) == "Permission denied");
    // and through the window, as File > Open does it, on this thread
    MainWindow w;
    QString error;
    {
        bwtest::FifoUnblocker guard(native(fifo));
        CHECK_FALSE(w.document()->load(fifo, &error));
        CHECK_FALSE(guard.unblocked());
    }
    CHECK(error.toStdString() == fifo.toStdString() + " is a FIFO, not a regular file");
#endif
}

TEST_CASE("Open Reference Image refuses a FIFO or a directory promptly and still opens a picture", "[app][io]") {
    MainWindow w;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QString error;
    const QString folder = dir.filePath(QStringLiteral("d.png"));
    REQUIRE(QDir().mkdir(folder));
    CHECK_FALSE(w.loadReferenceImage(folder, &error));
    CHECK(error.toStdString() == folder.toStdString() + " is a directory");
    CHECK_FALSE(w.loadReferenceImage(dir.filePath(QStringLiteral("nope.png")), &error));
    CHECK(error.toStdString() == "No such file or directory");
#if defined(__unix__) || defined(__APPLE__)
    const QString fifo = dir.filePath(QStringLiteral("pipe.png"));
    REQUIRE(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    {
        bwtest::FifoUnblocker guard(native(fifo));
        CHECK_FALSE(w.loadReferenceImage(fifo, &error));
        CHECK_FALSE(guard.unblocked());
    }
    CHECK(error.toStdString() == fifo.toStdString() + " is a FIFO, not a regular file");
#endif
    QImage red(16, 8, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString png = dir.filePath(QStringLiteral("ref.png"));
    REQUIRE(red.save(png));
    CHECK(w.loadReferenceImage(png, &error));
}
