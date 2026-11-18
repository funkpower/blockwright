#pragma once
// The Lint, IRC View and NomadNet View docks (PLAN §3, §11, §17). The Lint
// dock lists the whole lint (lint::lintCanvas, as `blockwright-cli lint`
// runs it) of the canvas under every named ramp and the craft pairs
// (lint::craftPair), as the CLI lints by default, or under one ramp, a finding to a row; activating a row asks
// for the cell cursor at its cell. The IRC View dock shows the canvas as an
// IRC client shows the lines File > Export IRC Text sends (irc::ircView,
// renderIrcView): the cells as saved with the document's iCE setting
// (irc::sentCanvas), the row meter's encoder options (View > IRC > Fit, Pad,
// Swap Shades), the mIRC palette, the client's background where IRC drops
// cells, and a red bar where a line over the budget is cut. It follows the
// canvas's scroll (followCanvas): its left column and top row are the canvas
// viewport's. The NomadNet View dock shows, the same way, the page Export
// Micron writes as NomadNet draws it at 256 colours (nomadnet::view256).
//
// Each follows the document a moment after each change (kRefreshDelayMs), and
// only while it is shown: a hidden dock, or one in a tab behind another,
// catches up when it is shown. The work runs off the GUI thread on a copy of
// the canvas; the dock shows its last result until the next is in, and drops
// a result the document has moved past. A dock behind the document for longer
// than kStaleShowMs says so under its result ("Linting…", "Updating…"):
// on a canvas whose work takes longer than the gap between edits no result
// is ever current, and the last one stays up with that note on it.

#include "blockwright/irc_view.h"
#include "blockwright/lint_rules.h"

#include <QImage>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

template <typename T>
class QFutureWatcher;
class QAbstractScrollArea;
class QComboBox;
class QLabel;
class QScrollArea;
class QTimer;
class QTreeWidget;

namespace bw {

class CanvasView;
class Document;
class IrcMeter;

inline constexpr int kRefreshDelayMs = 250;
// How long a dock is behind before it says so: an edit that is caught up
// within it (a refresh a moment after it, and quick work) shows no note.
inline constexpr int kStaleShowMs = 2 * kRefreshDelayMs;

// Refreshes a moment after the document changes, or when the widget is next
// shown if it wasn't shown then. Shown is the widget's show and hide events,
// and setShown: a dock in a tab behind another stays visible to Qt
// (off-screen), so the window passes on its dock's visibilityChanged.
//
// A refresh takes a snapshot (on the GUI thread), runs its work on the
// thread pool and shows the result back on the GUI thread. A refresh while
// one runs waits for it, drops its result and starts again, and a change
// while one runs drops its result (schedule), so only a result of the
// document as it is is shown. While what it shows has been stale for
// kStaleShowMs, staleLabel() is shown; the subclass puts it in its layout.
class FollowsDocument : public QWidget {
    Q_OBJECT
public:
    // `staleText` is what staleLabel() says.
    FollowsDocument(Document* doc, const QString& staleText, QWidget* parent = nullptr);
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
    // Says the result shown is behind the document; hidden while it isn't,
    // or hasn't been for kStaleShowMs.
    QLabel* staleLabel() const { return staleLabel_; }
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
    // Shows staleLabel() once stale() has held for kStaleShowMs; hides it
    // as soon as it doesn't.
    void updateStale();

    QTimer* timer_;
    QTimer* staleTimer_;
    QLabel* staleLabel_;
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

// The lint the Lint dock runs: lint::lintCanvas of `canvas`, the document's
// composite as stored (its cells raw, iCE read through `ice`), under every
// named ramp with the craft pairs (rampPick 0) or the one named ramp
// rampPick - 1, for the document's `target` (LintOptions::target). The
// Layer Style dialog lints its preview through it too, so the two never
// disagree.
lint::LintReport dockLint(const Canvas& canvas, bool ice, int rampPick = 0, Target target = Target::Ansi,
                          IrcColours colours = IrcColours::Mirc99,
                          const std::optional<std::array<Rgb, 16>>& palette = {});

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

// A dock showing a picture of the canvas at 1x, a canvas cell to every cell
// of the picture (the IRC View, the NomadNet View), that follows the
// canvas's scroll (followCanvas).
class CanvasFollowingDock : public FollowsDocument {
    Q_OBJECT
public:
    CanvasFollowingDock(Document* doc, const QString& staleText, QWidget* parent = nullptr);

    // The picture as drawn at 1x, at the document's letter spacing.
    const QImage& image() const { return image_; }
    QLabel* picture() const { return picture_; }
    // What the dock says under the picture.
    QLabel* statusLabel() const { return status_; }
    // The scroll area the picture is in.
    QScrollArea* scrollArea() const { return scroll_; }

    // Follows `view`, the canvas in `scroll`, one way (PLAN §11, #394): on
    // every scroll, zoom or size change of the canvas the picture scrolls so
    // that its left column and top row are the canvas viewport's
    // (canvasOrigin), by cell coordinates, as far as its scroll bars go: the
    // picture's column x and row y are at canvas column x and row y. That is
    // a rule about numbers, not content: irc::ircView keeps the canvas's
    // size, but a lone wide cell gets a continuation in the view and pushes
    // the rest of its row a column right, so what shows at a column can be
    // the canvas's column to its left. The canvas's cell is the one under
    // the viewport's corner on the canvas's own device grid (at a fractional
    // device pixel ratio the row meter's rowAt can be a row off it, #404).
    // Scrolled by hand (the bars, their menu, the wheel, the keys, a pan, an
    // assistive technology's new value; not a value set by code, such as
    // ensureVisible's; QScroller's kinetic scrolling, which Blockwright does
    // not enable, would need its own detection), the picture stays
    // where it is put until the canvas next scrolls, zooms or changes size;
    // the canvas centred again in a resized viewport is none of those.
    // Until then it keeps its place when it is redrawn, resized, hidden and
    // shown. Otherwise it goes back to the canvas's cell whenever its scroll
    // range changes: a new picture size, the dock resized or first laid out
    // when it is shown.
    void followCanvas(CanvasView* view, QAbstractScrollArea* scroll);
    // The cell at the top left of the canvas's viewport, as last followed.
    QPoint canvasOrigin() const { return origin_; }
    // Whether the picture is lined up with the canvas (not scrolled by hand
    // since the canvas last moved).
    bool followingCanvas() const { return following_; }

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;
    // Shows `image`, a picture of `cells` cells (columns, rows), lined up
    // with the canvas if following it, and `status` under it.
    void setPicture(const QImage& image, QSize cells, const QString& status);
    // Refreshes now if shown, else when next shown.
    void follow();

private:
    // The canvas scrolled, zoomed or changed size: takes its origin again
    // and lines the picture up with it.
    void canvasMoved();
    // Scrolls the picture to canvasOrigin, if following it.
    void lineUp();
    // The user scrolled the picture: it stays where they put it.
    void scrolledByHand();

    QImage image_;
    QSize cells_;
    QLabel* picture_;
    QLabel* status_;
    QScrollArea* scroll_;
    QPointer<CanvasView> canvas_;
    QPointer<QAbstractScrollArea> canvasScroll_;
    // What moves the canvas's top-left cell: the bars' values, the zoom and
    // the view's size (its cells, zoom and letter spacing).
    struct CanvasKey {
        int h = 0, v = 0, zoom = 0;
        QSize size;
        bool operator==(const CanvasKey&) const = default;
    };
    std::optional<CanvasKey> canvasKey_;
    QPoint origin_;
    bool following_ = false;
};

class IrcViewDock : public CanvasFollowingDock {
    Q_OBJECT
public:
    // The view of `doc` sent with `meter`'s encoder options (IrcMeter::options),
    // the ones Export writes with; a change to them (Fit, Pad, Swap Shades)
    // refreshes the view as a new budget does.
    IrcViewDock(Document* doc, IrcMeter* meter, QWidget* parent = nullptr);

    // The bytes a line may take before the view cuts it: the document's
    // budget (extras().ircBudget, View > IRC > Budget...), the one the row
    // meter, fit and Export use. A change to it (Document::setIrcBudget, its
    // undo and redo, another document) refreshes the view as an edit does.
    std::size_t budget() const;
    const irc::View& view() const { return view_; }
    // statusLabel() says "Every line fits B bytes" or "N rows cut at B bytes".

protected:
    Work snapshot() override;

private:
    IrcMeter* meter_;
    irc::View view_;
};

// The NomadNet View (PLAN §17): the page File > Export Micron writes as
// NomadNet draws it in its 256-colour mode (nomadnet::view256): the cells
// as written with the document's palette and iCE setting, each colour as
// urwid maps it to xterm's 256, a glyph NomadNet strips as '?'. It means
// something only under Micron; the window shows it only then.
class NomadNetViewDock : public CanvasFollowingDock {
    Q_OBJECT
public:
    explicit NomadNetViewDock(Document* doc, QWidget* parent = nullptr);
    // The view's cells, in the colours drawn.
    const Canvas& view() const { return view_; }
    // statusLabel() says the colour mode, and how many cells show '?'.

protected:
    Work snapshot() override;

private:
    Canvas view_{1, 1};
};

} // namespace bw
