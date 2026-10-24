#pragma once
// The Lint and IRC View docks (PLAN §3, §11). The Lint dock lists the whole
// lint (lint::lintCanvas, as `blockwright-cli lint` runs it) of the canvas
// under every named ramp and the craft pairs (lint::craftPair), as the CLI
// lints by default, or under one ramp, a finding to a row; activating a row asks
// for the cell cursor at its cell. The IRC View dock shows the canvas as an
// IRC client shows the exported lines (irc::ircView, renderIrcView): the
// mIRC palette, the client's background where IRC drops cells, and a red
// bar where a line over the budget is cut.
//
// Both follow the document a moment after each change (kRefreshDelayMs), and
// only while they are shown: a hidden dock, or one in a tab behind another,
// catches up when it is shown. The work runs off the GUI thread on a copy of
// the canvas; the dock shows its last result until the next is in, and drops
// a result the document has moved past.

#include "blockwright/irc_view.h"
#include "blockwright/lint_rules.h"

#include <QImage>
#include <QPoint>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <utility>

template <typename T>
class QFutureWatcher;
class QComboBox;
class QLabel;
class QTimer;
class QTreeWidget;

namespace bw {

class Document;

inline constexpr int kRefreshDelayMs = 250;

// Refreshes a moment after the document changes, or when the widget is next
// shown if it wasn't shown then. Shown is the widget's show and hide events,
// and setShown: a dock in a tab behind another stays visible to Qt
// (off-screen), so the window passes on its dock's visibilityChanged.
//
// A refresh takes a snapshot (on the GUI thread), runs its work on the
// thread pool and shows the result back on the GUI thread. A refresh while
// one runs waits for it, drops its result and starts again, and a change
// while one runs drops its result (schedule), so only a result of the
// document as it is is shown.
class FollowsDocument : public QWidget {
    Q_OBJECT
public:
    explicit FollowsDocument(Document* doc, QWidget* parent = nullptr);
    ~FollowsDocument() override;
    // Starts bringing the dock up to date and returns; refreshed() says when
    // it is.
    void refresh();
    // Whether what it shows is behind the document: a change waits for
    // refresh, or a refresh is still running.
    bool stale() const { return stale_ || running_; }
    // Whether a refresh is running.
    bool busy() const { return running_; }
    bool shown() const { return shown_; }
    // Refreshes now if a change is waiting.
    void setShown(bool on);
    // Tests only: runs on the worker before each refresh's work, so a test
    // can hold a refresh while it changes the document.
    void setBeforeWork(std::function<void()> hook) { beforeWork_ = std::move(hook); }

signals:
    // A refresh's result is shown.
    void refreshed();

protected:
    // Shows a result; runs on the GUI thread.
    using Show = std::function<void()>;
    // Makes a result off the GUI thread: it reads only what it captured.
    using Work = std::function<Show()>;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    // What the work needs, copied now (the canvas, the options).
    virtual Work snapshot() = 0;
    // What it shows is behind: refresh a moment from now, or when shown. A
    // refresh running now read the document before the change: its result
    // is dropped.
    void schedule();

    Document* doc_;

private:
    void start();
    void finished();

    QTimer* timer_;
    QFutureWatcher<Show>* watcher_;
    std::function<void()> beforeWork_;
    bool stale_ = true;
    bool shown_ = false;
    bool running_ = false;
    bool again_ = false;
    bool dropped_ = false;
};

// The Lint dock lists at most this many findings, the first in reading
// order; its summary counts them all.
inline constexpr int kMaxLintRows = 1000;

class LintDock : public FollowsDocument {
    Q_OBJECT
public:
    explicit LintDock(Document* doc, QWidget* parent = nullptr);

    const lint::LintReport& report() const { return report_; }
    // "All named ramps" (with the craft pairs), then each named ramp.
    QComboBox* rampBox() const { return ramps_; }
    // A row per finding, in the report's order, up to kMaxLintRows: cell,
    // kind, ramp, message.
    QTreeWidget* list() const { return list_; }
    // lint::summary of the report, and how many are listed when not all.
    QLabel* summaryLabel() const { return summary_; }

signals:
    // A finding's row was activated: its cell (Finding::x, y).
    void findingActivated(QPoint cell);

protected:
    Work snapshot() override;

private:
    void showReport(lint::LintReport report);

    QComboBox* ramps_;
    QTreeWidget* list_;
    QLabel* summary_;
    lint::LintReport report_;
};

class IrcViewDock : public FollowsDocument {
    Q_OBJECT
public:
    explicit IrcViewDock(Document* doc, QWidget* parent = nullptr);

    // The bytes a line may take before the view cuts it: the document's
    // budget (extras().ircBudget, View > IRC > Budget...), the one the row
    // meter, fit and Export use. A change to it (Document::setIrcBudget, its
    // undo and redo, another document) refreshes the view as an edit does.
    std::size_t budget() const;
    const irc::View& view() const { return view_; }
    // The view as drawn at 1x (renderIrcView), at the document's letter spacing.
    const QImage& image() const { return image_; }
    QLabel* picture() const { return picture_; }
    // "Every line fits B bytes" or "N rows cut at B bytes".
    QLabel* statusLabel() const { return status_; }

protected:
    Work snapshot() override;

private:
    irc::View view_;
    QImage image_;
    QLabel* picture_;
    QLabel* status_;
};

} // namespace bw
