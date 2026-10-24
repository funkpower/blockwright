// File > Publish to Library... and File > Send to WeeChat... (PLAN §9). The
// library is a scratch folder; ssh is a script that runs the remote command
// locally, with a scratch home, a pgrep that names a WeeChat and a scratch
// runtime folder, so the fifo handling runs for real without a host.

#include "scratch_settings.h"
#include "util.h"

#include "canvas_view.h"
#include "document.h"
#include "main_window.h"
#include "publish.h"
#include "select_tool.h"

#include "blockwright/irc.h"

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
struct FakeSsh {
    QTemporaryDir dir;
    QString script, log, home, run, fifo;

    explicit FakeSsh(int exitCode = 0, int forward = -1) {
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
    void makeFifo() const { REQUIRE(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0); }
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
    CHECK(question.contains(lib + QStringLiteral("/.tags.json")));
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
    CHECK(warning.startsWith(QStringLiteral("Could not publish:\n") + lib +
                             QStringLiteral("/.tags.json changed after the publish was worked out")));
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
        FakeSsh ssh;
        f.p->setSshProgram(ssh.script);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        INFO(r.message.toStdString());
        REQUIRE(r.ok);
        CHECK_FALSE(f.w.selectTool()->floating());
        const std::string ans = f.w.document()->encode().bytes;
        CHECK(ans != unstamped);
        CHECK(readAll(ssh.wip(QStringLiteral("neon-skull-r1.ans"))).toStdString() == ans);
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

TEST_CASE("Send to WeeChat refuses bad input and reports ssh failing") {
    Fixture f;
    SECTION("ssh fails") {
        FakeSsh ssh(255);
        f.p->setSshProgram(ssh.script);
        const auto r = f.p->sendToWeechat(QStringLiteral("irc-host"), QStringLiteral("~/ascii/wip"),
                                          QStringLiteral("neon-skull"), 1);
        CHECK_FALSE(r.ok);
        CHECK(r.message.contains(QStringLiteral("exit 255")));
        CHECK(r.message.contains(QStringLiteral("no route to host")));
    }
    SECTION("bad input never runs ssh") {
        FakeSsh ssh;
        f.p->setSshProgram(ssh.script);
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
        CHECK_FALSE(QFile::exists(ssh.log));
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
    FakeSsh ssh;
    f.p->setSshProgram(ssh.script);
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
    CHECK(QFile::exists(ssh.wip(QStringLiteral("neon-skull-r1.ans"))));

    CHECK(appSettings()->value(QStringLiteral("pipeline/host")).toString() == QStringLiteral("irc-host"));
    CHECK(f.p->nextRound(QStringLiteral("neon-skull")) == 2);
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
