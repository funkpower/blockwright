// File > Publish to Library... and File > Send to WeeChat... (PLAN §9). The
// library is a scratch folder. ssh is a stand-in that records what it was
// given and answers as told (RecordingSsh, on Windows too), or a script that
// runs the remote command locally, with a scratch home, a pgrep that names a
// WeeChat and a scratch runtime folder, so the fifo handling runs for real
// without a host (FakeSsh, which needs a POSIX sh).

#include "scratch_settings.h"
#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "publish.h"
#include "select_tool.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"
#include "blockwright/sauce.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>

#include <optional>
#include <string>

#include <sys/stat.h>

using namespace bw;
namespace fs = std::filesystem;

namespace {

fs::path fspath(const QString& s) { return fs::path(QFile::encodeName(s).toStdString()); }

void writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray readAll(const QString& path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

// A small piece and one whose second row has a colour change every cell,
// too many bytes for one IRC line.
const QByteArray kPiece = "\x1b[0;1;31mHELLO\x1b[0m\r\n\x1b[44m  \xdb\xdb  \x1b[0m\r\n";

QByteArray overBudget() {
    QByteArray s = "ok\r\n";
    for (int i = 0; i < 80; ++i)
        s += "\x1b[3" + QByteArray::number(1 + i % 6) + ";4" + QByteArray::number(i % 7) + "m\xdf";
    return s + "\x1b[0m\r\n";
}

struct Fixture {
    QTemporaryDir dir;
    MainWindow w;
    Publisher* p = nullptr;

    explicit Fixture(const QByteArray& ans = kPiece, const QString& name = QStringLiteral("Neon Skull.ans")) {
        REQUIRE(dir.isValid());
        p = w.findChild<Publisher*>();
        REQUIRE(p);
        const QString path = dir.filePath(name);
        writeFile(path, ans);
        QString error;
        REQUIRE(w.openFile(path, &error));
    }
    QString library() {
        QDir(dir.path()).mkpath(QStringLiteral("ascii/funkpower"));
        return dir.filePath(QStringLiteral("ascii"));
    }
};

// An ssh that logs its arguments and runs the last one as the remote shell
// would, in a scratch home with a pgrep that prints 4242. With `exitCode`
// it fails without running anything; with `forward` it passes only that
// many bytes of its input to the remote command, as a dropped connection
// would, then exits with `exitCode` (or the command's status for -1).
// A file in the library as the core names it, with the system's separator
// before the file's name.
QString inLibrary(const QString& lib, const char* name) {
    return QString::fromStdString((std::filesystem::path(lib.toStdString()) / name).string());
}

// An ssh that runs nothing: it writes its arguments and its input to files,
// prints `out` and `err` and exits with `exitCode`. On Windows it is a copy
// of bw-gen-helper (gen_helper_win.cpp), so Publisher starts a real program
// there and hands it its arguments through a Windows command line.
struct RecordingSsh {
    QTemporaryDir dir;
    QString program;

    explicit RecordingSsh(const QByteArray& out, int exitCode = 0, const QByteArray& err = {}) {
        REQUIRE(dir.isValid());
        writeFile(file("out"), out);
        writeFile(file("err"), err);
        writeFile(file("exit"), QByteArray::number(exitCode));
#ifdef _WIN32
        program = file("exe");
        const QString helper = QCoreApplication::applicationDirPath() + QStringLiteral("/bw-gen-helper.exe");
        REQUIRE(QFile::copy(helper, program));
        writeFile(file("mode"), "ssh");
#else
        program = dir.filePath(QStringLiteral("ssh"));
        const QByteArray at = QFile::encodeName(dir.filePath(QStringLiteral("ssh.")));
        writeFile(program, "#!/bin/sh\nfor a; do printf '%s\\0' \"$a\"; done > '" + at + "args'\ncat > '" + at +
                               "stdin'\ncat '" + at + "out'\ncat '" + at + "err' >&2\nexit $(cat '" + at +
                               "exit')\n");
        chmod(QFile::encodeName(program).constData(), 0755);
#endif
    }
    QString file(const char* ext) const { return dir.filePath(QStringLiteral("ssh.") + QLatin1String(ext)); }
    bool ran() const { return QFile::exists(file("args")); }
    QStringList args() const {
        QStringList out;
        for (const QByteArray& a : readAll(file("args")).split('\0'))
            out << QString::fromUtf8(a);
        out.removeLast();  // after the last NUL
        return out;
    }
    QByteArray input() const { return readAll(file("stdin")); }
};

QStringList sshArguments(const QString& host, const QString& wip, const QString& slug, int round, std::size_t size,
                         std::optional<std::size_t> textSize = {}) {
    QStringList out;
    for (const std::string& a : pipeline::sshArguments({host.toStdString(), wip.toStdString()}, slug.toStdString(),
                                                       round, size, textSize))
        out << QString::fromStdString(a);
    return out;
}

struct FakeSsh {
    QTemporaryDir dir;
    QString script, log, home, run, fifo;

    explicit FakeSsh(int exitCode = 0, int forward = -1) {
#ifdef _WIN32
        SKIP("the remote command runs in a POSIX sh, which Windows hasn't");
#endif
        REQUIRE(dir.isValid());
        script = dir.filePath(QStringLiteral("ssh"));
        log = dir.filePath(QStringLiteral("args"));
        home = dir.filePath(QStringLiteral("home"));
        run = dir.filePath(QStringLiteral("run"));
        fifo = run + QStringLiteral("/weechat/weechat_fifo_4242");
        const QString bin = dir.filePath(QStringLiteral("bin"));
        QDir().mkpath(home);
        QDir().mkpath(run + QStringLiteral("/weechat"));
        QDir().mkpath(bin);
        writeFile(bin + QStringLiteral("/pgrep"), "#!/bin/sh\n[ \"$1 $2\" = \"-x weechat\" ] && echo 4242\n");
        QByteArray body = "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + QFile::encodeName(log) + "'\n";
        const QByteArray remote = "HOME='" + QFile::encodeName(home) + "' PATH='" + QFile::encodeName(bin) +
                                  ":'\"$PATH\" XDG_RUNTIME_DIR='" + QFile::encodeName(run) + "' ";
        if (forward >= 0)
            body += "for last; do :; done\nhead -c " + QByteArray::number(forward) + " | " + remote +
                    "sh -c \"$last\"\nst=$?\ncat > /dev/null\nexit " +
                    (exitCode < 0 ? QByteArray("$st") : QByteArray::number(exitCode)) + "\n";
        else if (exitCode)
            body += "cat > /dev/null; echo 'no route to host' >&2; exit " + QByteArray::number(exitCode) + "\n";
        else
            body += "for last; do :; done\n" + remote + "exec sh -c \"$last\"\n";
        writeFile(script, body);
        chmod(QFile::encodeName(script).constData(), 0755);
        chmod(QFile::encodeName(bin + QStringLiteral("/pgrep")).constData(), 0755);
    }
    QString wip(const QString& name) const { return home + QStringLiteral("/ascii/wip/") + name; }
    void makeFifo() const {
#ifdef _WIN32
        SKIP("no FIFOs on Windows");
#else
        REQUIRE(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
#endif
    }
    // A timeout that, like uutils' (Ubuntu 26.04), leaves the signals its
    // caller ignores ignored in the command it runs.
    void plainTimeout() const {
        const QString t = dir.filePath(QStringLiteral("bin/timeout"));
        writeFile(t, "#!/bin/sh\ns=TERM\n[ \"$1\" = -s ] && { s=$2; shift 2; }\nn=$1\nshift\n\"$@\" &\np=$!\n"
                     "( sleep \"$n\"; kill -s \"$s\" \"$p\" ) > /dev/null 2>&1 &\nw=$!\nwait \"$p\"\nst=$?\n"
                     "kill -s KILL \"$w\" 2> /dev/null\n[ $st -gt 128 ] && exit 124\nexit $st\n");
        chmod(QFile::encodeName(t).constData(), 0755);
    }
};

// Moves the piece's first row one cell right and leaves it floating, not yet
// stamped, as a paste or a Library stamp is until Enter.
void floatFirstRow(MainWindow& w) {
    w.findChild<QAction*>(QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 5, 1});
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(w.canvasView(), &down);
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(w.canvasView(), &up);
    REQUIRE(w.selectTool()->floating());
}

// Answers the next message box with `button`, keeping its text.
void answer(QWidget* context, QMessageBox::StandardButton button, QString* text, bool* ran) {
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        REQUIRE(box);
        *ran = true;
        *text = box->text();
        box->button(button)->click();
    });
}

} // namespace

TEST_CASE("Publish to Library and Send to WeeChat are in the File menu") {
    MainWindow w;
    CHECK(w.findChild<QAction*>(QStringLiteral("publishToLibrary")));
    CHECK(w.findChild<QAction*>(QStringLiteral("sendToWeechat")));
}

TEST_CASE("The piece name defaults to the file's name as a slug") {
    Fixture f;
    CHECK(f.p->defaultSlug() == QStringLiteral("neon-skull"));
    f.w.document()->clear();
    CHECK(f.p->defaultSlug().isEmpty());
}

TEST_CASE("Publish to Library writes the piece, its IRC text and its index entries") {
    Fixture f;
    const QString lib = f.library();
    writeFile(lib + QStringLiteral("/.tags.json"), "{\n \"funkpower/old\": [\n  \"funkpower\"\n ]\n}\n");
    writeFile(lib + QStringLiteral("/.dates.json"), "{\n \"funkpower/old\": 100\n}");

    QString error;
    const auto plan = f.p->planPublish(lib, QStringLiteral("neon-skull"), QStringLiteral("LRH, comic"), &error);
    REQUIRE(plan);
    CHECK_FALSE(plan->replaces);
    CHECK_FALSE(QFile::exists(lib + QStringLiteral("/funkpower/neon-skull.ans")));
    REQUIRE(f.p->publish(*plan, &error));

    const std::string ans = f.w.document()->encode().bytes;
    CHECK(readAll(lib + QStringLiteral("/funkpower/neon-skull.ans")).toStdString() == ans);
    CHECK(readAll(lib + QStringLiteral("/funkpower/neon-skull.txt")).toStdString() ==
          irc::joinLines(pipeline::ircLines(ans)));
    CHECK(readAll(lib + QStringLiteral("/.tags.json")) ==
          "{\n \"funkpower/neon-skull\": [\n  \"comic\",\n  \"funkpower\",\n  \"lrh\"\n ],\n"
          " \"funkpower/old\": [\n  \"funkpower\"\n ]\n}\n");
    const QByteArray dates = readAll(lib + QStringLiteral("/.dates.json"));
    CHECK(dates.startsWith("{\n \"funkpower/neon-skull\": "));
    CHECK(dates.endsWith(",\n \"funkpower/old\": 100\n}"));
}

TEST_CASE("Publish to Library refuses what it can't publish") {
    SECTION("an IRC line over the budget") {
        Fixture f(overBudget());
        QString error;
        CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("x"), {}, &error));
        CHECK(error.contains(QStringLiteral("line 2 is")));
    }
    SECTION("a folder that isn't the ascii library") {
        Fixture f;
        QString error;
        CHECK_FALSE(f.p->planPublish(f.dir.path(), QStringLiteral("x"), {}, &error));
        CHECK(error.contains(QStringLiteral("has no funkpower folder")));
    }
    SECTION("a bad piece name") {
        Fixture f;
        QString error;
        CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("Neon Skull"), {}, &error));
        CHECK(error.contains(QStringLiteral("is not a piece name")));
    }
    SECTION("an .ans that loses data, unless allowed") {
        Fixture f;
        AnsiDocument doc = f.w.document()->ansi();
        doc.canvas.at(0, 0).lostBlink = true;  // blink lost on open
        f.w.document()->replace(doc, f.w.document()->path());
        REQUIRE(f.w.document()->encode().blinkDropped == 1);
        QString error;
        CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("x"), {}, &error));
        CHECK(error.contains(QStringLiteral("lose data")));
        CHECK(f.p->planPublish(f.library(), QStringLiteral("x"), {}, &error, true));
    }
}

TEST_CASE("Publish to Library through its dialogs asks before writing") {
    Fixture f;
    const QString lib = f.library();
    appSettings()->remove(QStringLiteral("pipeline"));
    bool asked = false, confirmed = false;
    QString question;
    const auto run = [&](QMessageBox::StandardButton button) {
        asked = confirmed = false;
        QTimer::singleShot(0, &f.w, [&, button] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            REQUIRE(d->objectName() == QStringLiteral("publishDialog"));
            asked = true;
            auto* library = d->findChild<QLineEdit*>(QStringLiteral("publishLibrary"));
            auto* slug = d->findChild<QLineEdit*>(QStringLiteral("publishSlug"));
            QPushButton* ok = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            CHECK(slug->text() == QStringLiteral("neon-skull"));
            if (library->text().isEmpty()) {
                CHECK_FALSE(ok->isEnabled());  // no library yet
                library->setText(lib);
            } else {
                CHECK(library->text() == lib);  // remembered
            }
            slug->setText(QStringLiteral("Neon"));
            CHECK_FALSE(ok->isEnabled());
            slug->setText(QStringLiteral("neon-skull"));
            d->findChild<QLineEdit*>(QStringLiteral("publishTags"))->setText(QStringLiteral("lrh"));
            REQUIRE(ok->isEnabled());
            answer(&f.w, button, &question, &confirmed);
            ok->click();
        });
        f.p->publishInteractive();
        CHECK(asked);
        CHECK(confirmed);
    };

    run(QMessageBox::No);
    CHECK(question.contains(QStringLiteral("Publish funkpower/neon-skull, tagged funkpower, lrh")));
    CHECK(question.contains(inLibrary(lib, ".tags.json")));
    CHECK_FALSE(QFile::exists(lib + QStringLiteral("/funkpower/neon-skull.ans")));

    run(QMessageBox::Yes);
    CHECK(QFile::exists(lib + QStringLiteral("/funkpower/neon-skull.ans")));
    CHECK(f.w.statusBar()->currentMessage() == QStringLiteral("Published funkpower/neon-skull"));

    run(QMessageBox::No);
    CHECK(question.startsWith(QStringLiteral("Replace funkpower/neon-skull")));
}

TEST_CASE("Publish to Library through its dialogs refuses a library that changed while it asked") {
    Fixture f;
    const QString lib = f.library();
    appSettings()->remove(QStringLiteral("pipeline"));
    const QByteArray other = "{\n \"funkpower/other\": [\n  \"funkpower\"\n ]\n}\n";
    bool asked = false, confirmed = false, warned = false;
    QString warning;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        asked = true;
        d->findChild<QLineEdit*>(QStringLiteral("publishLibrary"))->setText(lib);
        QTimer::singleShot(0, &f.w, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(box);
            confirmed = true;
            // Another publish lands while this one waits for its answer.
            writeFile(lib + QStringLiteral("/.tags.json"), other);
            answer(&f.w, QMessageBox::Ok, &warning, &warned);
            box->button(QMessageBox::Yes)->click();
        });
        d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    f.p->publishInteractive();
    CHECK(asked);
    CHECK(confirmed);
    REQUIRE(warned);
    INFO(warning.toStdString());
    CHECK(warning.startsWith(QStringLiteral("Could not publish:\n") + inLibrary(lib, ".tags.json") +
                             QStringLiteral(" changed after the publish was worked out")));
    CHECK(readAll(lib + QStringLiteral("/.tags.json")) == other);
    CHECK_FALSE(QFile::exists(lib + QStringLiteral("/funkpower/neon-skull.ans")));
    CHECK_FALSE(QFile::exists(lib + QStringLiteral("/.dates.json")));
}

TEST_CASE("Publish to Library and Send to WeeChat stamp a floating selection first, as Save does") {
    Fixture f;
    floatFirstRow(f.w);
    const std::string unstamped = f.w.document()->encode().bytes;
    SECTION("publish") {
        QString error;
        const auto plan = f.p->planPublish(f.library(), QStringLiteral("neon-skull"), QString(), &error);
        INFO(error.toStdString());
        REQUIRE(plan);
        CHECK_FALSE(f.w.selectTool()->floating());
        const std::string ans = f.w.document()->encode().bytes;
        CHECK(ans != unstamped);
        CHECK(plan->ans == ans);
        CHECK(plan->txt == irc::joinLines(pipeline::ircLines(ans)));
    }
    SECTION("send") {
        RecordingSsh ssh("NOFIFO\n");
        f.p->setSshProgram(ssh.program);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        INFO(r.message.toStdString());
        REQUIRE(r.ok);
        CHECK_FALSE(f.w.selectTool()->floating());
        const std::string ans = f.w.document()->encode().bytes;
        CHECK(ans != unstamped);
        CHECK(ssh.input().toStdString() == ans);
    }
}

TEST_CASE("Send to WeeChat copies the round and reloads through a live fifo") {
    Fixture f;
    FakeSsh ssh;
    ssh.makeFifo();
    f.p->setSshProgram(ssh.script);
    appSettings()->remove(QStringLiteral("pipeline"));
    CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 1);

    QProcess weechat;
    weechat.start(QStringLiteral("cat"), {ssh.fifo});
    REQUIRE(weechat.waitForStarted());
    const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                      QStringLiteral("neon-skull"), 2);
    INFO(r.message.toStdString());
    CHECK(r.ok);
    CHECK(r.reloaded);
    REQUIRE(weechat.waitForFinished(5000));
    CHECK(weechat.readAllStandardOutput() == "core.weechat */ascii reload\n");
    CHECK(readAll(ssh.wip(QStringLiteral("neon-skull-r2.ans"))).toStdString() == f.w.document()->encode().bytes);
    CHECK(QDir(ssh.wip({})).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot) ==
          QStringList{QStringLiteral("neon-skull-r2.ans")});
    // The round gets the mode `cat >` would give it, not mktemp's 0600.
    const mode_t mask = ::umask(0);
    ::umask(mask);
    struct stat st {};
    REQUIRE(::stat(QFile::encodeName(ssh.wip(QStringLiteral("neon-skull-r2.ans"))).constData(), &st) == 0);
    CHECK((st.st_mode & 0777) == (0666 & ~mask));
    const QStringList args = QString::fromUtf8(readAll(ssh.log)).split(QLatin1Char('\n'));
    CHECK(args.mid(0, 5) == QStringList{QStringLiteral("-o"), QStringLiteral("BatchMode=yes"), QStringLiteral("-o"),
                                        QStringLiteral("ConnectTimeout=10"), QStringLiteral("irc-host")});
    CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 3);
}

TEST_CASE("Send to WeeChat says when no WeeChat takes the reload") {
    Fixture f;
    FakeSsh ssh;
    f.p->setSshProgram(ssh.script);
    SECTION("no fifo") {}
    SECTION("a stale fifo, which would block without the timeout") { ssh.makeFifo(); }
    SECTION("a stale fifo and a timeout that passes an ignored TERM on") {
        ssh.makeFifo();
        ssh.plainTimeout();
    }
    const auto r =
        f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/wip"), QStringLiteral("neon-skull"), 1);
    INFO(r.message.toStdString());
    CHECK(r.ok);
    CHECK_FALSE(r.reloaded);
    CHECK(r.message.contains(QStringLiteral("/fifo enable")));
    CHECK(QFile::exists(ssh.home + QStringLiteral("/wip/neon-skull-r1.ans")));
}

TEST_CASE("Send to WeeChat never writes through a file already in wip") {
    // A predictable temporary name would follow a link someone left there.
    Fixture f;
    FakeSsh ssh;
    f.p->setSshProgram(ssh.script);
    const QString sentinel = ssh.dir.filePath(QStringLiteral("sentinel"));
    writeFile(sentinel, "keep me");
    QDir().mkpath(ssh.wip({}));
    REQUIRE(QFile::link(sentinel, ssh.wip(QStringLiteral("neon-skull-r1.ans.tmp"))));
    const auto r =
        f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("neon-skull"), 1);
    INFO(r.message.toStdString());
    CHECK(r.ok);
    CHECK(readAll(sentinel) == "keep me");
    CHECK(readAll(ssh.wip(QStringLiteral("neon-skull-r1.ans"))).toStdString() == f.w.document()->encode().bytes);
    // Nothing but the round and the link is left in wip.
    CHECK(QDir(ssh.wip({})).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot) ==
          QStringList{QStringLiteral("neon-skull-r1.ans"), QStringLiteral("neon-skull-r1.ans.tmp")});
}

TEST_CASE("Send to WeeChat leaves the round there when the upload is cut short") {
    // The remote cat sees end of input either way; only the length tells a
    // whole upload from a dropped one.
    Fixture f;
    appSettings()->remove(QStringLiteral("pipeline"));
    const int sent = 20;
    REQUIRE(f.w.document()->encode().bytes.size() > std::size_t(sent));
    SECTION("the connection drops") {
        FakeSsh ssh(255, sent);
        f.p->setSshProgram(ssh.script);
        QDir().mkpath(ssh.wip({}));
        writeFile(ssh.wip(QStringLiteral("neon-skull-r1.ans")), "the round sent before");
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        CHECK_FALSE(r.ok);
        CHECK(r.message.contains(QStringLiteral("exit 255")));
        CHECK(readAll(ssh.wip(QStringLiteral("neon-skull-r1.ans"))) == "the round sent before");
        CHECK(QDir(ssh.wip({})).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot) ==
              QStringList{QStringLiteral("neon-skull-r1.ans")});
    }
    SECTION("the host says how much arrived") {
        FakeSsh ssh(-1, sent);
        f.p->setSshProgram(ssh.script);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        INFO(r.message.toStdString());
        CHECK_FALSE(r.ok);
        CHECK(r.message.contains(QStringLiteral("exit 1")));
        CHECK(r.message.contains(QStringLiteral("only %1 of %2 bytes arrived")
                                     .arg(sent)
                                     .arg(f.w.document()->encode().bytes.size())));
        CHECK_FALSE(QFile::exists(ssh.wip(QStringLiteral("neon-skull-r1.ans"))));
        CHECK(QDir(ssh.wip({})).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                  .isEmpty());
        CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 1);
    }
}

TEST_CASE("Send to WeeChat hands ssh the remote command and the piece, and reads its answer") {
    Fixture f;
    appSettings()->remove(QStringLiteral("pipeline"));
    const std::string ans = f.w.document()->encode().bytes;
    const QStringList expected = sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                              QStringLiteral("neon-skull"), 2, ans.size());
    const auto send = [&](RecordingSsh& ssh) {
        f.p->setSshProgram(ssh.program);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 2);
        INFO(r.message.toStdString());
        CHECK(ssh.args() == expected);
        CHECK(ssh.input().toStdString() == ans);
        return r;
    };
    SECTION("a WeeChat reloads") {
        RecordingSsh ssh("RELOADED\n");
        const auto r = send(ssh);
        CHECK(r.ok);
        CHECK(r.reloaded);
        CHECK(r.message.isEmpty());
        CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 3);
    }
    SECTION("no WeeChat has a fifo") {
        RecordingSsh ssh("NOFIFO\n");
        const auto r = send(ssh);
        CHECK(r.ok);
        CHECK_FALSE(r.reloaded);
        CHECK(r.message.contains(QStringLiteral("/fifo enable")));
        CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 3);
    }
    SECTION("an answer that is neither, though ssh exits 0") {
        RecordingSsh ssh("RELOADED\nsomething else\n");
        const auto r = send(ssh);
        CHECK_FALSE(r.ok);
        CHECK(r.message == QStringLiteral("Sending to irc-host failed (exit 0)"));
        CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 1);
    }
    SECTION("ssh fails after the remote command answered") {
        RecordingSsh ssh("RELOADED\n", 255, "Connection to irc-host closed.\n");
        const auto r = send(ssh);
        CHECK_FALSE(r.ok);
        CHECK(r.message == QStringLiteral("Sending to irc-host failed (exit 255):\nConnection to irc-host closed."));
        CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 1);
    }
}

TEST_CASE("Send to WeeChat refuses bad input and reports ssh failing") {
    Fixture f;
    SECTION("ssh fails") {
        RecordingSsh ssh({}, 255, "no route to host\n");
        f.p->setSshProgram(ssh.program);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        CHECK_FALSE(r.ok);
        CHECK(r.message.contains(QStringLiteral("exit 255")));
        CHECK(r.message.contains(QStringLiteral("no route to host")));
    }
    SECTION("bad input never runs ssh") {
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        CHECK_FALSE(f.p->sendToWeechat(QStringLiteral("-oProxyCommand=x"), QStringLiteral("~/w"),
                                       QStringLiteral("a"), 1).ok);
        CHECK_FALSE(f.p->sendToWeechat(QStringLiteral("h"), QStringLiteral("~/w;rm"), QStringLiteral("a"), 1).ok);
        CHECK_FALSE(f.p->sendToWeechat(QStringLiteral("h"), QStringLiteral("~/w"), QStringLiteral("A"), 1).ok);
        CHECK_FALSE(f.p->sendToWeechat(QStringLiteral("h"), QStringLiteral("~/w"), QStringLiteral("a"), 0).ok);
        AnsiDocument doc = f.w.document()->ansi();
        doc.canvas.at(0, 0).lostBlink = true;  // blink lost on open
        f.w.document()->replace(doc, f.w.document()->path());
        REQUIRE(f.w.document()->encode().blinkDropped == 1);
        const auto lossy = f.p->sendToWeechat(QStringLiteral("h"), QStringLiteral("~/w"), QStringLiteral("a"), 1);
        CHECK_FALSE(lossy.ok);
        CHECK(lossy.message.contains(QStringLiteral("lose data")));
        CHECK_FALSE(ssh.ran());
    }
    SECTION("a missing ssh") {
        f.p->setSshProgram(f.dir.filePath(QStringLiteral("no-ssh")));
        const auto r = f.p->sendToWeechat(QStringLiteral("h"), QStringLiteral("~/w"), QStringLiteral("a"), 1);
        CHECK_FALSE(r.ok);
        CHECK(r.message.startsWith(QStringLiteral("Could not run")));
    }
}

TEST_CASE("Send to WeeChat through its dialogs remembers the host and the round") {
    Fixture f;
    RecordingSsh ssh("NOFIFO\n");
    f.p->setSshProgram(ssh.program);
    appSettings()->remove(QStringLiteral("pipeline"));
    bool asked = false, confirmed = false, told = false;
    QString question, info;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        REQUIRE(d->objectName() == QStringLiteral("sendDialog"));
        asked = true;
        QPushButton* ok = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        CHECK(d->findChild<QLineEdit*>(QStringLiteral("sendWipDir"))->text() == QStringLiteral("~/ascii/wip"));
        CHECK(d->findChild<QSpinBox*>(QStringLiteral("sendRound"))->value() == 1);
        CHECK_FALSE(ok->isEnabled());  // no host yet
        d->findChild<QLineEdit*>(QStringLiteral("sendHost"))->setText(QStringLiteral("irc-host"));
        REQUIRE(ok->isEnabled());
        QTimer::singleShot(0, &f.w, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(box);
            confirmed = true;
            question = box->text();
            answer(&f.w, QMessageBox::Ok, &info, &told);  // no fifo
            box->button(QMessageBox::Yes)->click();
        });
        ok->click();
    });
    f.p->sendInteractive();
    CHECK(asked);
    CHECK(confirmed);
    CHECK(told);
    CHECK(question.contains(QStringLiteral("irc-host:~/ascii/wip/neon-skull-r1.ans")));
    CHECK(info.contains(QStringLiteral("/fifo enable")));
    CHECK(ssh.args() == sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                     QStringLiteral("neon-skull"), 1, f.w.document()->encode().bytes.size()));
    CHECK(ssh.input().toStdString() == f.w.document()->encode().bytes);

    CHECK(appSettings()->value(QStringLiteral("pipeline/host")).toString() == QStringLiteral("irc-host"));
    CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 2);
}

namespace {

// The same piece as a CP437 .ans and as a UTF-8 one (tux-utf8.ans is
// golden/tux.ans written as UTF-8).
QByteArray tux(const char* file) { return QByteArray::fromStdString(bwtest::readFile(bwtest::dataDir() / file)); }

// The first .txt line of tux: its first row's half blocks, read right.
const QString kTuxBlocks = QStringLiteral("▄▄▄▄▄▄");

// The document with a ★ (no CP437 code) at 0,0.
void putStar(Fixture& f) {
    AnsiDocument doc = f.w.document()->ansi();
    doc.canvas.at(0, 0).ch = U'★';
    f.w.document()->replace(doc, f.w.document()->path());
}

// What putStar's ★ costs a CP437 .ans, named with its cell.
const QString kStarLoss =
    QStringLiteral("1 cell holds a glyph with no CP437 code and will be written as '?': ★ at 0,0");
// Publish's and Send's own questions about it, and the refusal without one.
const QString kPublishStarLoss =
    QStringLiteral("Before publishing:\n") + kStarLoss + QStringLiteral(".\n\nPublish anyway?");
const QString kSendStarLoss = QStringLiteral("Before sending:\n") + kStarLoss + QStringLiteral(".\n\nSend anyway?");
const QString kStarRefusal = QStringLiteral("The .ans would lose data:\n") + kStarLoss + QLatin1Char('.');
// The questions when a .txt from the canvas goes beside the .ans.
const QString kCanvasText =
    QStringLiteral("\n\nThat is the .ans only: the .txt is made from the canvas, and WeeChat's ascii script plays "
                   "the .txt.");
const QString kPublishStarLossText =
    QStringLiteral("Before publishing:\n") + kStarLoss + QLatin1Char('.') + kCanvasText + QStringLiteral("\n\nPublish anyway?");
const QString kSendStarLossText =
    QStringLiteral("Before sending:\n") + kStarLoss + QLatin1Char('.') + kCanvasText + QStringLiteral("\n\nSend anyway?");

// The .txt the document's cells make, as File > Export IRC Text and
// `cli export --irc` write them.
std::string canvasText(const Document& doc) {
    return irc::joinLines(irc::exportCanvas(irc::sentCanvas(doc.canvas(), doc.ansi().ice, doc.target())));
}

// mIRC colour `n` as a cell colour.
Color mirc(int n) {
    const std::uint32_t c = irc::kMircRgb[n];
    return Color::fromRgb({std::uint8_t(c >> 16), std::uint8_t(c >> 8), std::uint8_t(c)});
}

// kPiece under the IRC target with a ★ in mIRC 77 at 0,0, where the 'H' was:
// a glyph and a colour the CP437 .ans can't hold.
void ircStar(Fixture& f) {
    AnsiDocument doc = f.w.document()->ansi();
    doc.canvas.at(0, 0).ch = U'★';
    doc.canvas.at(0, 0).fg = mirc(77);
    f.w.document()->replace(doc, f.w.document()->path());
    REQUIRE(f.w.document()->setTarget(Target::Irc));
    REQUIRE(f.w.document()->target() == Target::Irc);
}

} // namespace

TEST_CASE("Publish to Library writes a UTF-8 document as CP437, as it publishes the same art from a CP437 one") {
    Fixture u(tux("utf8/tux-utf8.ans"), QStringLiteral("tux.ans"));
    Fixture c(tux("golden/tux.ans"), QStringLiteral("tux.ans"));
    REQUIRE(u.w.document()->textEncoding() == TextEncoding::Utf8);
    REQUIRE(c.w.document()->textEncoding() == TextEncoding::Cp437);
    const auto published = [](Fixture& f) {
        const QString lib = f.library();
        QString error;
        const auto plan = f.p->planPublish(lib, QStringLiteral("tux"), {}, &error);
        INFO(error.toStdString());
        REQUIRE(plan);
        REQUIRE(f.p->publish(*plan, &error));
        return std::pair{readAll(lib + QStringLiteral("/funkpower/tux.ans")),
                         QString::fromUtf8(readAll(lib + QStringLiteral("/funkpower/tux.txt")))};
    };
    const auto [uAns, uTxt] = published(u);
    const auto [cAns, cTxt] = published(c);

    CHECK(uAns == cAns);
    CHECK(uAns.toStdString() == u.w.document()->encode(ArtFormat::Ansi, TextEncoding::Cp437).bytes);
    CHECK(detectEncoding(splitSauce(uAns.toStdString()).content) == TextEncoding::Cp437);
    // Read back as the library reads it, it is the art the document opened with.
    const Canvas art = parseAnsi(tux("utf8/tux-utf8.ans").toStdString(), {.encoding = TextEncoding::Utf8}).canvas;
    CHECK(parseAnsi(uAns.toStdString(), {.encoding = TextEncoding::Cp437}).canvas == art);
    CHECK(uTxt == cTxt);
    CHECK(uTxt.toStdString() == irc::joinLines(irc::exportCanvas(art)));
    CHECK(uTxt.section(QLatin1Char('\n'), 0, 0).endsWith(kTuxBlocks));
    // The document's own encoding is left as it was.
    CHECK(u.w.document()->textEncoding() == TextEncoding::Utf8);
}

TEST_CASE("Send to WeeChat sends a UTF-8 document as CP437") {
    Fixture u(tux("utf8/tux-utf8.ans"), QStringLiteral("tux.ans"));
    Fixture c(tux("golden/tux.ans"), QStringLiteral("tux.ans"));
    REQUIRE(u.w.document()->textEncoding() == TextEncoding::Utf8);
    const AnsiOutput out = u.p->payload();
    CHECK(out.encoding == TextEncoding::Cp437);
    CHECK_FALSE(out.lossy());
    CHECK(out.bytes == c.p->payload().bytes);
    CHECK(out.bytes == u.w.document()->encode(ArtFormat::Ansi, TextEncoding::Cp437).bytes);

    // And what ssh is handed is that payload, then the .txt the canvas
    // makes (#336 slice 3), their lengths included.
    const std::string txt = canvasText(*u.w.document());
    REQUIRE(u.p->ircText());
    CHECK(irc::joinLines(*u.p->ircText()) == txt);
    CHECK_FALSE(c.p->ircText());
    RecordingSsh ssh("RELOADED\n");
    u.p->setSshProgram(ssh.program);
    const auto r = u.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                      QStringLiteral("tux"), 1);
    INFO(r.message.toStdString());
    CHECK(r.ok);
    CHECK(ssh.input().toStdString() == out.bytes + txt);
    CHECK(ssh.args() == sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("tux"),
                                     1, out.bytes.size(), txt.size()));
}

TEST_CASE("Publish to Library and Send to WeeChat warn of loss without the Save As UTF-8 hint") {
    SECTION("the hint is Save's alone") {
        Fixture f;
        putStar(f);
        CHECK(saveWarning(f.w.document()->encode()).contains(QStringLiteral("Save As")));
        CHECK(pipelineWarning(f.w.document()->encode(), PipelineAction::Publish) == kPublishStarLoss);
        CHECK(pipelineWarning(f.w.document()->encode(), PipelineAction::Send) == kSendStarLoss);
        CHECK_FALSE(kPublishStarLoss.contains(QStringLiteral("sav"), Qt::CaseInsensitive));
        CHECK_FALSE(kSendStarLoss.contains(QStringLiteral("sav"), Qt::CaseInsensitive));
    }
    SECTION("a UTF-8 document with a glyph CP437 lacks loses it, and says so") {
        Fixture f(tux("utf8/tux-utf8.ans"), QStringLiteral("tux.ans"));
        putStar(f);
        REQUIRE_FALSE(f.w.document()->encode().lossy());  // its own Save keeps the star
        QString error;
        CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("tux"), {}, &error));
        CHECK(error == kStarRefusal);
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("tux"), 1);
        CHECK_FALSE(r.ok);
        CHECK(r.message == kStarRefusal);
        CHECK_FALSE(ssh.ran());
    }
    SECTION("refused without allowLossy") {
        Fixture f;
        putStar(f);
        QString error;
        CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("x"), {}, &error));
        CHECK(error == kStarRefusal);
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("x"), 1);
        CHECK(r.message == kStarRefusal);
        CHECK_FALSE(ssh.ran());
    }
    SECTION("Publish's question") {
        Fixture f;
        putStar(f);
        const QString lib = f.library();
        appSettings()->remove(QStringLiteral("pipeline"));
        QString error;
        const auto plan = f.p->planPublish(lib, QStringLiteral("neon-skull"), {}, &error, true);
        REQUIRE(plan);
        const auto q = [](const fs::path& p) { return QFile::decodeName(QByteArray::fromStdString(p.string())); };
        const QString expected = QStringLiteral("Publish funkpower/neon-skull, tagged funkpower.\n\nThis writes:\n") +
                                 q(plan->ansPath) + QLatin1Char('\n') + q(plan->txtPath) + QLatin1Char('\n') +
                                 q(plan->tagsPath) + QLatin1Char('\n') + q(plan->datesPath) + QStringLiteral("\n\n") +
                                 kPublishStarLoss;
        bool asked = false, confirmed = false;
        QString question;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            asked = true;
            d->findChild<QLineEdit*>(QStringLiteral("publishLibrary"))->setText(lib);
            answer(&f.w, QMessageBox::No, &question, &confirmed);
            d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        f.p->publishInteractive();
        CHECK(asked);
        CHECK(confirmed);
        CHECK(question == expected);
    }
    SECTION("Send's question") {
        Fixture f;
        putStar(f);
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        appSettings()->remove(QStringLiteral("pipeline"));
        bool asked = false, confirmed = false;
        QString question;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            asked = true;
            d->findChild<QLineEdit*>(QStringLiteral("sendHost"))->setText(QStringLiteral("irc-host"));
            answer(&f.w, QMessageBox::No, &question, &confirmed);
            d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        f.p->sendInteractive();
        CHECK(asked);
        CHECK(confirmed);
        CHECK(question == QStringLiteral("Copy the piece to irc-host:~/ascii/wip/neon-skull-r1.ans and reload "
                                         "WeeChat's ascii script?\n\n") +
                              kSendStarLoss);
        CHECK_FALSE(ssh.ran());
    }
}

TEST_CASE("Publish to Library and Send to WeeChat write a UTF-8 document's ★ as '?' once told to go ahead") {
    // A UTF-8 document's own Save keeps the ★; Publish and Send write CP437,
    // ask, and on Yes write it as '?' and leave the document as it was.
    const Canvas art = parseAnsi(tux("utf8/tux-utf8.ans").toStdString(), {.encoding = TextEncoding::Utf8}).canvas;
    Canvas starless = art;
    starless.at(0, 0).ch = U'?';

    SECTION("Publish") {
        Fixture f(tux("utf8/tux-utf8.ans"), QStringLiteral("tux.ans"));
        putStar(f);
        REQUIRE(f.w.document()->textEncoding() == TextEncoding::Utf8);
        REQUIRE_FALSE(f.w.document()->encode().lossy());
        const QString lib = f.library();
        appSettings()->remove(QStringLiteral("pipeline"));
        bool asked = false, confirmed = false;
        QString question;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            asked = true;
            d->findChild<QLineEdit*>(QStringLiteral("publishLibrary"))->setText(lib);
            answer(&f.w, QMessageBox::Yes, &question, &confirmed);
            d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        f.p->publishInteractive();
        CHECK(asked);
        CHECK(confirmed);
        CHECK(question.endsWith(QStringLiteral("\n\n") + kPublishStarLossText));

        const QByteArray ans = readAll(lib + QStringLiteral("/funkpower/tux.ans"));
        CHECK(detectEncoding(splitSauce(ans.toStdString()).content) == TextEncoding::Cp437);
        CHECK(ans.toStdString() == f.w.document()->encode(ArtFormat::Ansi, TextEncoding::Cp437).bytes);
        CHECK(parseAnsi(ans.toStdString(), {.encoding = TextEncoding::Cp437}).canvas == starless);
        const QString txt = QString::fromUtf8(readAll(lib + QStringLiteral("/funkpower/tux.txt")));
        // The .txt is the canvas's, ★ and all (#336 slice 3).
        CHECK(txt.toStdString() == canvasText(*f.w.document()));
        CHECK(txt.section(QLatin1Char('\n'), 0, 0).endsWith(kTuxBlocks));
        CHECK_FALSE(txt.contains(QStringLiteral("Γ")));  // no UTF-8 bytes read as CP437
        // The document keeps its ★ and its encoding.
        CHECK(f.w.document()->canvas().at(0, 0).ch == U'★');
        CHECK(f.w.document()->textEncoding() == TextEncoding::Utf8);
    }
    SECTION("Send") {
        Fixture f(tux("utf8/tux-utf8.ans"), QStringLiteral("tux.ans"));
        putStar(f);
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        appSettings()->remove(QStringLiteral("pipeline"));
        bool asked = false, confirmed = false;
        QString question;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            asked = true;
            d->findChild<QLineEdit*>(QStringLiteral("sendHost"))->setText(QStringLiteral("irc-host"));
            answer(&f.w, QMessageBox::Yes, &question, &confirmed);
            d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        f.p->sendInteractive();
        CHECK(asked);
        CHECK(confirmed);
        CHECK(question.startsWith(QStringLiteral("Copy the piece to irc-host:~/ascii/wip/tux-r1.ans and tux-r1.txt "
                                                 "and reload WeeChat's ascii script?")));
        CHECK(question.endsWith(QStringLiteral("\n\n") + kSendStarLossText));

        REQUIRE(ssh.ran());
        const std::string cp437 = f.w.document()->encode(ArtFormat::Ansi, TextEncoding::Cp437).bytes;
        const std::string txt = canvasText(*f.w.document());
        const QByteArray input = ssh.input();
        REQUIRE(std::size_t(input.size()) == cp437.size() + txt.size());
        const QByteArray sent = input.left(qsizetype(cp437.size()));
        CHECK(sent.toStdString() == cp437);
        CHECK(input.mid(qsizetype(cp437.size())).toStdString() == txt);
        CHECK(std::size_t(sent.size()) < f.w.document()->encode().bytes.size());  // not the UTF-8 bytes
        CHECK(detectEncoding(splitSauce(sent.toStdString()).content) == TextEncoding::Cp437);
        CHECK(ssh.args() == sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                         QStringLiteral("tux"), 1, cp437.size(), txt.size()));
        CHECK(parseAnsi(sent.toStdString(), {.encoding = TextEncoding::Cp437}).canvas == starless);
        CHECK(f.w.document()->canvas().at(0, 0).ch == U'★');
    }
}

TEST_CASE("Publish to Library writes an IRC-target document's .txt from its canvas, glyphs and mIRC colours kept",
          "[utf8]") {
    // #336 slice 3, #350 slice 4: the .ans stays CP437 and loses the ★ and
    // mIRC 77; the .txt, which WeeChat's ascii script plays, keeps both.
    Fixture f;
    ircStar(f);
    const std::string txt = canvasText(*f.w.document());
    REQUIRE(txt.find("\xE2\x98\x85") != std::string::npos);  // ★
    REQUIRE(txt.find("\x03" "77") != std::string::npos);
    REQUIRE(f.p->ircText());
    CHECK(irc::joinLines(*f.p->ircText()) == txt);
    // The .txt is what File > Export IRC Text writes too.
    const QString exported = f.dir.filePath(QStringLiteral("export.txt"));
    QString error;
    REQUIRE(f.w.exportIrcText(exported, &error));
    CHECK(readAll(exported).toStdString() == txt);

    const AnsiOutput out = f.p->payload();
    REQUIRE(out.lossy());
    const QString lib = f.library();
    CHECK_FALSE(f.p->planPublish(lib, QStringLiteral("star"), {}, &error));
    CHECK(error.startsWith(QStringLiteral("The .ans would lose data:\n")));
    const auto plan = f.p->planPublish(lib, QStringLiteral("star"), {}, &error, true);
    INFO(error.toStdString());
    REQUIRE(plan);
    REQUIRE(f.p->publish(*plan, &error));
    const QByteArray ans = readAll(lib + QStringLiteral("/funkpower/star.ans"));
    CHECK(ans.toStdString() == out.bytes);
    CHECK(detectEncoding(splitSauce(ans.toStdString()).content) == TextEncoding::Cp437);
    CHECK(parseAnsi(ans.toStdString(), {.encoding = TextEncoding::Cp437}).canvas.at(0, 0).ch == U'?');
    CHECK(readAll(lib + QStringLiteral("/funkpower/star.txt")).toStdString() == txt);
    CHECK(txt != irc::joinLines(pipeline::ircLines(out.bytes)));  // not the .ans read back
}

TEST_CASE("Publish to Library keeps an ANSI-target CP437 document's .txt as the .ans read back", "[utf8]") {
    Fixture f;
    CHECK_FALSE(f.p->ircText());
    const QString lib = f.library();
    QString error;
    const auto plan = f.p->planPublish(lib, QStringLiteral("neon-skull"), {}, &error);
    REQUIRE(plan);
    CHECK(plan->txt == irc::joinLines(pipeline::ircLines(f.p->payload().bytes)));
}

TEST_CASE("Send to WeeChat puts an IRC-target document's .txt beside the .ans", "[utf8]") {
    Fixture f;
    ircStar(f);
    const std::string txt = canvasText(*f.w.document());
    const AnsiOutput out = f.p->payload();

    SECTION("what ssh is handed") {
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        CHECK(f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("star"), 1)
                  .message == QStringLiteral("The .ans would lose data:\n") + lossDescription(out, true) +
                                  QLatin1Char('.'));
        CHECK_FALSE(ssh.ran());
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("star"), 1, true);
        INFO(r.message.toStdString());
        CHECK(r.ok);
        CHECK(ssh.input().toStdString() == out.bytes + txt);
        CHECK(ssh.args() == sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                         QStringLiteral("star"), 1, out.bytes.size(), txt.size()));
    }
    SECTION("what lands in wip, and what a later ANSI send of the round leaves") {
        FakeSsh ssh;
        f.p->setSshProgram(ssh.script);
        auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                    QStringLiteral("star"), 1, true);
        INFO(r.message.toStdString());
        CHECK(r.ok);
        const auto entries = [&] {
            return QDir(ssh.wip({})).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        };
        CHECK(entries() == QStringList{QStringLiteral("star-r1.ans"), QStringLiteral("star-r1.txt")});
        CHECK(readAll(ssh.wip(QStringLiteral("star-r1.ans"))).toStdString() == out.bytes);
        CHECK(readAll(ssh.wip(QStringLiteral("star-r1.txt"))).toStdString() == txt);
        const mode_t mask = ::umask(0);
        ::umask(mask);
        for (const char* name : {"star-r1.ans", "star-r1.txt"}) {
            struct stat st {};
            REQUIRE(::stat(QFile::encodeName(ssh.wip(QLatin1String(name))).constData(), &st) == 0);
            CHECK((st.st_mode & 0777) == (0666 & ~mask));
        }

        // Back on the ANSI target the document is CP437 again: the round
        // sent again goes without a .txt, and the old one goes, since the
        // ascii script would play it instead.
        REQUIRE(f.w.document()->setTarget(Target::Ansi));
        REQUIRE(f.w.document()->textEncoding() == TextEncoding::Cp437);
        REQUIRE_FALSE(f.p->ircText());
        r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("star"), 1,
                               true);
        CHECK(r.ok);
        CHECK(entries() == QStringList{QStringLiteral("star-r1.ans")});
        CHECK(readAll(ssh.wip(QStringLiteral("star-r1.ans"))).toStdString() == f.p->payload().bytes);
    }
    SECTION("the question") {
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        appSettings()->remove(QStringLiteral("pipeline"));
        bool asked = false, confirmed = false;
        QString question;
        QTimer::singleShot(0, &f.w, [&] {
            auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            REQUIRE(d);
            asked = true;
            d->findChild<QLineEdit*>(QStringLiteral("sendHost"))->setText(QStringLiteral("irc-host"));
            answer(&f.w, QMessageBox::No, &question, &confirmed);
            d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        f.p->sendInteractive();
        CHECK(asked);
        CHECK(confirmed);
        CHECK(question == QStringLiteral("Copy the piece to irc-host:~/ascii/wip/neon-skull-r1.ans and "
                                         "neon-skull-r1.txt and reload WeeChat's ascii script?\n\n") +
                              pipelineWarning(out, PipelineAction::Send, true));
        CHECK(question.contains(kCanvasText + QStringLiteral("\n\nSend anyway?")));
        CHECK_FALSE(ssh.ran());
    }
}

TEST_CASE("Publish to Library and Send to WeeChat keep bold, italic and underline in an IRC-target .txt",
          "[utf8][attrs]") {
    // #362 review: two rows of A, mIRC 0 on 1, the second bold, italic and
    // underlined. The .ans has no way to write the formatting and drops it;
    // the .txt keeps it, as File > Export IRC Text writes it (#372).
    Fixture f;
    AnsiDocument doc = f.w.document()->ansi();
    doc.canvas = Canvas(1, 2);
    for (int y = 0; y < 2; ++y) {
        Cell& c = doc.canvas.at(0, y);
        c.ch = U'A';
        c.fg = Color::indexed(White);
        c.bg = Color::indexed(Black);
    }
    doc.canvas.at(0, 1).attrs = attr::Bold | attr::Italic | attr::Underline;
    f.w.document()->replace(doc, f.w.document()->path());
    REQUIRE(f.w.document()->setTarget(Target::Irc));
    const std::string want = "\x03" "0,1A\n"
                             "\x03" "0,1\x02\x1d\x1f" "A\n";
    REQUIRE(f.p->ircText());
    CHECK(irc::joinLines(*f.p->ircText()) == want);
    const AnsiOutput out = f.p->payload();
    REQUIRE(out.formattingDropped == 1);
    REQUIRE(out.lossy());
    const QString refusal = QStringLiteral("The .ans would lose data:\n") + lossDescription(out, true) +
                            QLatin1Char('.');

    SECTION("Publish") {
        const QString lib = f.library();
        QString error;
        CHECK_FALSE(f.p->planPublish(lib, QStringLiteral("biu"), {}, &error));
        CHECK(error == refusal);
        CHECK_FALSE(QFile::exists(lib + QStringLiteral("/funkpower/biu.txt")));
        const auto plan = f.p->planPublish(lib, QStringLiteral("biu"), {}, &error, true);
        INFO(error.toStdString());
        REQUIRE(plan);
        CHECK(plan->txt == want);
        REQUIRE(f.p->publish(*plan, &error));
        CHECK(readAll(lib + QStringLiteral("/funkpower/biu.ans")).toStdString() == out.bytes);
        CHECK(readAll(lib + QStringLiteral("/funkpower/biu.txt")).toStdString() == want);
    }
    SECTION("Send") {
        RecordingSsh ssh("RELOADED\n");
        f.p->setSshProgram(ssh.program);
        CHECK(f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("biu"), 1)
                  .message == refusal);
        CHECK_FALSE(ssh.ran());
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("biu"), 1, true);
        INFO(r.message.toStdString());
        CHECK(r.ok);
        const QByteArray input = ssh.input();
        REQUIRE(std::size_t(input.size()) == out.bytes.size() + want.size());
        CHECK(input.left(qsizetype(out.bytes.size())).toStdString() == out.bytes);
        CHECK(input.mid(qsizetype(out.bytes.size())).toStdString() == want);
        CHECK(ssh.args() == sshArguments(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                         QStringLiteral("biu"), 1, out.bytes.size(), want.size()));
    }
}

TEST_CASE("Publish to Library and Send to WeeChat refuse an IRC-target .txt line over the ascii script's budget",
          "[utf8]") {
    // A row of ★ alternating mIRC 16 and 17: a colour code and three bytes
    // of glyph every cell on IRC, while the CP437 .ans brings both colours
    // to one ANSI colour and the ★ to '?'.
    Fixture f;
    AnsiDocument doc = f.w.document()->ansi();
    doc.canvas.resize(80, doc.canvas.height());
    for (int x = 0; x < doc.canvas.width(); ++x) {
        doc.canvas.at(x, 1).ch = U'★';
        doc.canvas.at(x, 1).fg = mirc(16 + x % 2);
    }
    f.w.document()->replace(doc, f.w.document()->path());
    REQUIRE(f.w.document()->setTarget(Target::Irc));
    REQUIRE(f.p->ircText());
    REQUIRE_FALSE(irc::overBudget(*f.p->ircText(), irc::kEfnetWeechatBudget).empty());
    const AnsiOutput out = f.p->payload();
    REQUIRE(irc::overBudget(pipeline::ircLines(out.bytes), irc::kEfnetWeechatBudget).empty());

    QString error;
    CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("bars"), {}, &error, true));
    CHECK(error.startsWith(QStringLiteral("over the 407-byte IRC budget as WeeChat's ascii script sends it, so it would be cut on IRC: line 2 is ")));
    CHECK_FALSE(QFile::exists(f.library() + QStringLiteral("/funkpower/bars.txt")));

    RecordingSsh ssh("RELOADED\n");
    f.p->setSshProgram(ssh.program);
    const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                      QStringLiteral("bars"), 1, true);
    CHECK_FALSE(r.ok);
    CHECK(r.message == error);
    CHECK_FALSE(ssh.ran());
}

TEST_CASE("Publish to Library and Send to WeeChat refuse a .txt the ascii script cuts though its lines fit as "
          "written",
          "[utf8]") {
    // Codex's repro on #362: an IRC-target row of ★, mIRC 4 and 5 by turns on
    // 1. The encoder writes one-digit colour codes, 402 bytes; WeeChat's ascii
    // script reads the .txt back and writes them with two (compact), 483
    // bytes, and cuts the line at 407 to 67 stars.
    Fixture f;
    AnsiDocument doc = f.w.document()->ansi();
    doc.canvas.resize(80, 1);
    for (int x = 0; x < doc.canvas.width(); ++x) {
        doc.canvas.at(x, 0).ch = U'★';
        doc.canvas.at(x, 0).fg = mirc(4 + x % 2);
        doc.canvas.at(x, 0).bg = mirc(1);
    }
    f.w.document()->replace(doc, f.w.document()->path());
    REQUIRE(f.w.document()->setTarget(Target::Irc));
    const auto lines = f.p->ircText();
    REQUIRE(lines);
    REQUIRE(lines->size() == 1);
    std::string want = "\x03" "4,1\xE2\x98\x85";
    for (int x = 1; x < 80; ++x)
        want += (x % 2 ? "\x03" "5" : "\x03" "4") + std::string("\xE2\x98\x85");
    REQUIRE(lines->front() == want);
    REQUIRE(want.size() == 402);
    CHECK(irc::overBudget(*lines, irc::kEfnetWeechatBudget).empty());
    CHECK(irc::txtSentBytes(want) == 483);
    REQUIRE(f.p->payload().lossy());

    QString error;
    CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("stars"), {}, &error, true));
    CHECK(error == QStringLiteral("over the 407-byte IRC budget as WeeChat's ascii script sends it, so it would be "
                                  "cut on IRC: line 1 is 483 bytes"));
    CHECK_FALSE(QFile::exists(f.library() + QStringLiteral("/funkpower/stars.txt")));

    RecordingSsh ssh("RELOADED\n");
    f.p->setSshProgram(ssh.program);
    const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                      QStringLiteral("stars"), 1, true);
    CHECK_FALSE(r.ok);
    CHECK(r.message == error);
    CHECK_FALSE(ssh.ran());
}

namespace {

// The document as one row of 'x's under the IRC target, as many as make its
// .txt one line of `sent` bytes as WeeChat's ascii script sends it (the
// line's colour code included), with `budget` as its own IRC budget (none:
// it keeps the one it has).
void plainRow(Fixture& f, std::size_t sent, std::optional<std::size_t> budget) {
    const auto put = [&](int width) {
        AnsiDocument doc = f.w.document()->ansi();
        doc.canvas = Canvas(width, 1);
        for (int x = 0; x < width; ++x)
            doc.canvas.at(x, 0).ch = U'x';
        f.w.document()->replace(doc, f.w.document()->path());
        f.w.document()->setTarget(Target::Irc);
        REQUIRE(f.w.document()->target() == Target::Irc);
        if (budget)
            f.w.document()->setIrcBudget({std::string(irc::kCustomProfile), *budget});
        const auto lines = f.p->ircText();
        REQUIRE(lines);
        REQUIRE(lines->size() == 1);
        return irc::txtSentBytes(lines->front());
    };
    const std::size_t extra = put(int(sent)) - sent;  // the line's colour code
    REQUIRE(put(int(sent - extra)) == sent);
}

// Publish's and Send's answers for the document: Publish's error (empty if
// it would publish) and whether Send went ahead (`message` its refusal).
struct Outcome {
    QString publishError;
    bool sent = false;
    QString sendMessage;
};
Outcome tryBoth(Fixture& f) {
    Outcome o;
    QString error;
    if (!f.p->planPublish(f.library(), QStringLiteral("row"), {}, &error, true))
        o.publishError = error.isEmpty() ? QStringLiteral("?") : error;
    RecordingSsh ssh("RELOADED\n");
    f.p->setSshProgram(ssh.program);
    const auto r =
        f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"), QStringLiteral("row"), 1, true);
    o.sent = r.ok;
    o.sendMessage = r.message;
    CHECK(ssh.ran() == r.ok);
    return o;
}

const QString kCutAt407 =
    QStringLiteral("over the 407-byte IRC budget as WeeChat's ascii script sends it, so it would be cut on IRC: ");

} // namespace

TEST_CASE("Publish to Library and Send to WeeChat hold the .txt to a document budget tighter than 407") {
    // #393: the row meter calls the line over, so Publish and Send refuse it,
    // and say which budget they enforced.
    Fixture f;
    plainRow(f, 400, 350);
    CHECK(f.p->sendBudget() == pipeline::SendBudget{350, pipeline::BudgetSource::Document});
    const QString refusal = QStringLiteral(
        "over the document's own 350-byte IRC budget as WeeChat's ascii script sends it: line 1 is 400 bytes");
    Outcome o = tryBoth(f);
    CHECK(o.publishError == refusal);
    CHECK_FALSE(o.sent);
    CHECK(o.sendMessage == refusal);
    CHECK_FALSE(QFile::exists(f.library() + QStringLiteral("/funkpower/row.txt")));

    // Through the dialog, the warning shows the same budget.
    const QString lib = f.library();
    bool warned = false;
    QString warning;
    QTimer::singleShot(0, &f.w, [&] {
        auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        REQUIRE(d);
        REQUIRE(d->objectName() == QStringLiteral("publishDialog"));
        d->findChild<QLineEdit*>(QStringLiteral("publishLibrary"))->setText(lib);
        d->findChild<QLineEdit*>(QStringLiteral("publishSlug"))->setText(QStringLiteral("row"));
        answer(&f.w, QMessageBox::Ok, &warning, &warned);
        d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    f.p->publishInteractive();
    CHECK(warned);
    CHECK(warning == QStringLiteral("Could not publish:\n") + refusal);

    // At the document's budget the line goes.
    f.w.document()->setIrcBudget({std::string(irc::kCustomProfile), 400});
    CHECK(f.p->sendBudget() == pipeline::SendBudget{400, pipeline::BudgetSource::Document});
    o = tryBoth(f);
    CHECK(o.publishError.isEmpty());
    CHECK(o.sent);
}

TEST_CASE("Publish to Library writes nothing when the document's budget changed after the plan") {
    // Codex, #408 round 1: a plan worked out under one budget is not written
    // under another.
    Fixture f;
    plainRow(f, 400, 400);
    QString error;
    auto plan = f.p->planPublish(f.library(), QStringLiteral("row"), {}, &error, true);
    REQUIRE(plan);
    CHECK(plan->budget == pipeline::SendBudget{400, pipeline::BudgetSource::Document});
    f.w.document()->setIrcBudget({std::string(irc::kCustomProfile), 350});
    CHECK_FALSE(f.p->publish(*plan, &error));
    CHECK(error == QStringLiteral("The document's IRC budget changed after the publish was worked out, so nothing "
                                  "was written; publish again to hold the lines to it"));
    CHECK_FALSE(QFile::exists(f.library() + QStringLiteral("/funkpower/row.txt")));
    CHECK_FALSE(QFile::exists(f.library() + QStringLiteral("/funkpower/row.ans")));
    CHECK_FALSE(f.p->planPublish(f.library(), QStringLiteral("row"), {}, &error, true));

    // A change that leaves the budget Publish enforces as it was is no change:
    // 450 and 500 both hold the line to 407.
    plainRow(f, 407, 450);
    plan = f.p->planPublish(f.library(), QStringLiteral("row"), {}, &error, true);
    REQUIRE(plan);
    CHECK(plan->budget == pipeline::SendBudget{});
    f.w.document()->setIrcBudget({std::string(irc::kCustomProfile), 500});
    CHECK(f.p->publish(*plan, &error));
    CHECK(QFile::exists(f.library() + QStringLiteral("/funkpower/row.txt")));
}

TEST_CASE("Publish to Library and Send to WeeChat hold the .txt to 407 under a looser document budget") {
    // #393: the library's limit holds whatever the document allows.
    Fixture f;
    plainRow(f, 450, 450);
    CHECK(f.p->sendBudget() == pipeline::SendBudget{407, pipeline::BudgetSource::Library});
    Outcome o = tryBoth(f);
    CHECK(o.publishError == kCutAt407 + QStringLiteral("line 1 is 450 bytes"));
    CHECK_FALSE(o.sent);
    CHECK(o.sendMessage == o.publishError);

    plainRow(f, 408, 450);
    o = tryBoth(f);
    CHECK(o.publishError == kCutAt407 + QStringLiteral("line 1 is 408 bytes"));
    CHECK_FALSE(o.sent);

    plainRow(f, 407, 450);
    o = tryBoth(f);
    CHECK(o.publishError.isEmpty());
    CHECK(o.sent);
}

TEST_CASE("Publish to Library and Send to WeeChat hold the .txt to 407 for a document with no budget of its own") {
    Fixture f;
    REQUIRE(f.w.document()->extras().ircBudget == project::IrcBudget{});
    CHECK(f.p->sendBudget() == pipeline::SendBudget{});
    CHECK(f.p->sendBudget().bytes == 407);

    plainRow(f, 450, std::nullopt);
    REQUIRE(f.w.document()->extras().ircBudget == project::IrcBudget{});
    Outcome o = tryBoth(f);
    CHECK(o.publishError == kCutAt407 + QStringLiteral("line 1 is 450 bytes"));
    CHECK_FALSE(o.sent);
    CHECK(o.sendMessage == o.publishError);

    plainRow(f, 407, std::nullopt);
    o = tryBoth(f);
    CHECK(o.publishError.isEmpty());
    CHECK(o.sent);
}

int main(int argc, char** argv) {
    // The settings and the Library go to scratch folders, never the user's.
    QTemporaryDir scratch;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(scratch.filePath(QStringLiteral("config"))));
    qputenv("BLOCKWRIGHT_BUNDLED_LIBRARY", QFile::encodeName(scratch.filePath(QStringLiteral("bundled"))));
    qputenv("BLOCKWRIGHT_USER_LIBRARY", QFile::encodeName(scratch.filePath(QStringLiteral("user"))));
    bwtest::useScratchSettings(scratch);
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
