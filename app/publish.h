#pragma once
// File > Publish to Library... and File > Send to WeeChat... (PLAN §9): the
// two pipeline actions that write outside the document's own file. Each asks
// for what it needs, shows every path it will write and asks before writing.
// The folders, the host and each piece's last round are remembered.

#include "blockwright/ansi.h"
#include "blockwright/pipeline.h"

#include <QObject>
#include <QString>

#include <optional>
#include <string>
#include <vector>

class QMenu;

namespace bw {

class MainWindow;

class Publisher : public QObject {
    Q_OBJECT
public:
    // A child of `window`, publishing its document.
    explicit Publisher(MainWindow* window);

    // Adds both actions to `file` (objectNames publishToLibrary and
    // sendToWeechat).
    void addActions(QMenu* file);

    // The .ans Publish writes and Send to WeeChat sends: the document's
    // cells as Save writes them, always as CP437 (PLAN §9), whatever its own
    // encoding. A floating selection is not stamped here; both actions stamp
    // it first.
    AnsiOutput payload() const;

    // The lines of the .txt Publish writes and Send to WeeChat sends beside
    // the .ans when the document holds what the CP437 .ans can't (#336,
    // #350): under the IRC target or for a UTF-8 document, the cells as
    // File > Export IRC Text and `cli export --irc` send them
    // (irc::exportCanvas of irc::sentCanvas), so its glyphs, mIRC colours
    // and anything else the IRC encoder carries reach WeeChat's ascii
    // script, which plays a .txt before an .ans. Empty for an ANSI-target
    // CP437 document, whose .txt Publish reads back from the .ans as before
    // and which Send sends no .txt for.
    std::optional<std::vector<std::string>> ircText() const;

    // The piece name suggested for the document: pipeline::slugFrom its
    // file's base name, empty for an unsaved document.
    QString defaultSlug() const;

    // Works out publishing the document as `slug` with `tags` (as typed,
    // pipeline::parseTags) into the ascii library at `library`, writing
    // nothing to it. A floating selection is stamped first, as Save does.
    // Empty, and `error`, if it can't be published; an .ans that would lose
    // data (AnsiOutput::lossy) is refused unless `allowLossy`.
    std::optional<pipeline::PublishPlan> planPublish(const QString& library, const QString& slug,
                                                     const QString& tags, QString* error,
                                                     bool allowLossy = false);
    // Writes `plan`; false and `error` if it fails.
    bool publish(const pipeline::PublishPlan& plan, QString* error);

    struct SendResult {
        bool ok = false;        // the round is in the wip folder
        bool reloaded = false;  // and WeeChat's fifo took the reload
        QString message;        // what failed, or what to do on no fifo
    };
    // Copies the document's .ans to <wipDir>/<slug>-r<round>.ans on `host`
    // over ssh, with ircText as <slug>-r<round>.txt beside it when there is
    // one, and reloads WeeChat's ascii script; waits for ssh to finish.
    // A floating selection is stamped first. An .ans that would lose data is
    // refused unless `allowLossy`, and a .txt with a line over the ascii
    // script's EFnet budget (irc::kEfnetWeechatBudget) is refused, as
    // Publish refuses it.
    SendResult sendToWeechat(const QString& host, const QString& wipDir, const QString& slug, int round,
                             bool allowLossy = false);
    // The ssh program run ("ssh" unless set).
    void setSshProgram(const QString& program) { ssh_ = program; }
    // The round after the last one sent for `slug`, or 1.
    int nextRound(const QString& slug) const;

public slots:
    // The menu actions, with dialogs.
    void publishInteractive();
    void sendInteractive();

private:
    MainWindow* window_;
    QString ssh_ = QStringLiteral("ssh");
};

} // namespace bw
