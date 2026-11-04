#include "main_window.h"

#include "branding.h"
#include "canvas_view.h"
#include "irc_meter.h"
#include "manual_dialog.h"
#include "select_tool.h"
#include "palette_widget.h"
#include "plural.h"
#include "preferences.h"
#include "shortcut_settings.h"
#include "publish.h"
#include "recent_files.h"
#include "shortcuts_dialog.h"
#include "start_screen.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph_sets.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QScrollArea>
#include <QScreen>
#include <QStatusBar>
#include <QWhatsThis>

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bw {

namespace {

QString ansiFilter() { return QObject::tr("ANSI art (*.ans)"); }
QString utf8AnsiFilter() { return QObject::tr("ANSI art, UTF-8 (*.ans)"); }
QString cp437OpenFilter() { return QObject::tr("ANSI art, CP437 (*.ans)"); }
QString utf8OpenFilter() { return utf8AnsiFilter(); }

} // namespace

QStringList openNameFilters() {
    return {QObject::tr("Blockwright projects and ANSI art (*.bwp *.ans *.xb *.bin)"),
            QObject::tr("Blockwright projects (*.bwp)"),
            ansiFilter(),
            cp437OpenFilter(),
            utf8OpenFilter(),
            QObject::tr("XBIN (*.xb)"),
            QObject::tr("BIN (*.bin)"),
            QObject::tr("All files (*)")};
}

std::optional<TextEncoding> openFilterEncoding(const QString& filter) {
    if (filter == cp437OpenFilter())
        return TextEncoding::Cp437;
    if (filter == utf8OpenFilter())
        return TextEncoding::Utf8;
    return std::nullopt;
}

namespace {

// The distinct glyphs of `cells`, in the order they are first met, each
// with its cell (column,row from 0, as the status bar and the Lint dock give
// it), or how many cells hold it and the first: the first few, then how
// many more. A control, format or invisible character (unprintableCodePoint)
// is named by its code point (quotedGlyph), never shown raw. With
// `written`, each says what it is written as ("♥ U+FE0F at 0,1 written as ♥").
QString namedGlyphs(const std::vector<Unwritable>& cells, bool written = false) {
    constexpr std::size_t kNamed = 3;
    std::vector<std::pair<Unwritable, int>> glyphs;  // first cell, cells holding it
    for (const Unwritable& u : cells) {
        const auto it = std::find_if(glyphs.begin(), glyphs.end(), [&](const auto& g) {
            return g.first.ch == u.ch && g.first.written == u.written;
        });
        if (it == glyphs.end())
            glyphs.emplace_back(u, 1);
        else
            ++it->second;
    }
    QStringList named;
    for (std::size_t i = 0; i < std::min(glyphs.size(), kNamed); ++i) {
        const auto& [u, n] = glyphs[i];
        const QString glyph = QString::fromStdString(quotedGlyph(u.ch));
        const QString at = QStringLiteral("%1,%2").arg(u.x).arg(u.y);
        if (written) {
            const QString as = QString::fromStdString(quotedGlyph(u.written));
            named << (n == 1 ? QObject::tr("%1 at %2 written as %3").arg(glyph, at, as)
                             : QObject::tr("%1 in %2 cells, first at %3, written as %4")
                                   .arg(glyph, QString::number(n), at, as));
        } else {
            named << (n == 1 ? QObject::tr("%1 at %2").arg(glyph, at)
                             : QObject::tr("%1 in %2 cells, first at %3").arg(glyph, QString::number(n), at));
        }
    }
    QString text = named.join(QStringLiteral("; "));
    if (glyphs.size() > kNamed)
        text += plural(int(glyphs.size() - kNamed), QObject::tr("; and %n more glyph"),
                       QObject::tr("; and %n more glyphs"));
    return text;
}

// What a UTF-8 writer (`holder`: "a UTF-8 .ans", "IRC text") can't write
// of `lost`. UTF-8 holds every glyph, so that is a control character (C0,
// DEL, C1, U+2028 or U+2029) a cell holds, never a glyph CP437 lacks, the
// same before a continuation, and a lead whose glyph a terminal wouldn't
// draw over both cells; a lead's continuation is written as a space. A
// UTF-8 .ans also lists a lone wide cell and a lone mark (utf8LoneLoss).
QStringList utf8LossParts(const std::vector<Unwritable>& lost, const QString& holder) {
    QStringList parts;
    // `one` and `many` name the holder %1, where they name it, and the
    // glyphs after it
    const auto add = [&](auto which, const QString& one, const QString& many, bool named) {
        std::vector<Unwritable> cells;
        std::copy_if(lost.begin(), lost.end(), std::back_inserter(cells), which);
        if (cells.empty())
            return;
        const QString text = plural(int(cells.size()), one, many);
        parts << (named ? text.arg(holder) : text).arg(namedGlyphs(cells));
    };
    add([](const Unwritable& u) { return u.loss == Loss::Control && !u.lead; },
        QObject::tr("%n cell holds a control character %1 can't hold and will be written as '?': %2"),
        QObject::tr("%n cells hold control characters %1 can't hold and will be written as '?': %2"), true);
    add([](const Unwritable& u) { return u.loss == Loss::Control && u.lead; },
        QObject::tr("%n cell holds a control character %1 can't hold before a continuation, and will be written "
                    "as '?' and a space: %2"),
        QObject::tr("%n cells hold control characters %1 can't hold before a continuation, and will be written "
                    "as '?' and a space: %2"),
        true);
    add([](const Unwritable& u) { return u.loss == Loss::Wide; },
        QObject::tr("%n cell holds a glyph that a terminal doesn't draw two columns wide before a continuation, "
                    "and will be written as '?' and a space: %1"),
        QObject::tr("%n cells hold glyphs that a terminal doesn't draw two columns wide before a continuation, "
                    "and will be written as '?' and a space: %1"),
        false);
    add([](const Unwritable& u) { return u.loss == Loss::LoneWide; },
        QObject::tr("%n cell holds a glyph two columns wide with no continuation after it, and will be written as "
                    "'?' so the row keeps its columns: %1"),
        QObject::tr("%n cells hold glyphs two columns wide with no continuation after them, and will be written as "
                    "'?' so the row keeps its columns: %1"),
        false);
    add([](const Unwritable& u) { return u.loss == Loss::LoneMark; },
        QObject::tr("%n cell holds a zero-width glyph that a terminal would draw on the cell before it, and will be "
                    "written as '?' so the row keeps its columns: %1"),
        QObject::tr("%n cells hold zero-width glyphs that a terminal would draw on the cell before them, and will "
                    "be written as '?' so the row keeps its columns: %1"),
        false);
    return parts;
}

} // namespace

QString ircLossDescription(const std::vector<Unwritable>& lost) {
    return utf8LossParts(lost, QObject::tr("IRC text")).join(QStringLiteral(";\n"));
}

QString readLossDescription(const ReadLoss& loss) {
    QStringList parts;
    if (loss.marksDropped) {
        const QString at = QStringLiteral("%1,%2").arg(loss.markX).arg(loss.markY);
        parts << (loss.markClusters > 1
                      ? plural(loss.marksDropped,
                               QObject::tr("%n combining code point past %1 dropped in %2 clusters, the first at %3"),
                               QObject::tr("%n combining code points past %1 dropped in %2 clusters, the first at %3"))
                            .arg(kMaxClusterCodePoints)
                            .arg(loss.markClusters)
                            .arg(at)
                      : plural(loss.marksDropped, QObject::tr("%n combining code point past %1 dropped at %2"),
                               QObject::tr("%n combining code points past %1 dropped at %2"))
                            .arg(kMaxClusterCodePoints)
                            .arg(at));
    }
    if (loss.poolRefused)
        parts << plural(loss.poolRefused, QObject::tr("%n cluster read as its base: too many distinct clusters"),
                        QObject::tr("%n clusters read as their base: too many distinct clusters"));
    return parts.join(QStringLiteral("; "));
}

QString lossDescription(const AnsiOutput& out, bool countWide) {
    QStringList parts;
    const auto lost = [&](auto which) {
        std::vector<Unwritable> cells;
        std::copy_if(out.unwritable.begin(), out.unwritable.end(), std::back_inserter(cells), which);
        return cells;
    };
    const bool utf8 = out.encoding == TextEncoding::Utf8;
    const int controls = utf8 ? 0 : ansControlCount(out.unwritable);
    if (utf8) {
        parts << utf8LossParts(out.unwritable, QObject::tr("a UTF-8 .ans"));
    } else {
        if (const std::vector<Unwritable> c = lost([](const Unwritable& u) { return u.loss == Loss::NoCode; });
            !c.empty())
            parts << plural(int(c.size()),
                            QObject::tr("%n cell holds a glyph with no CP437 code and will be written as '?': %1"),
                            QObject::tr("%n cells hold a glyph with no CP437 code and will be written as '?': %1"))
                         .arg(namedGlyphs(c));
        // a lead and a cluster written as its base, each named with what
        // it is written as; Publish and Send count the leads instead
        if (const std::vector<Unwritable> c = lost([&](const Unwritable& u) {
                return u.loss == Loss::MarksDropped || (u.loss == Loss::Wide && !countWide);
            });
            !c.empty())
            parts << plural(int(c.size()), QObject::tr("%n cell holds a glyph CP437 can't hold whole: %1"),
                            QObject::tr("%n cells hold glyphs CP437 can't hold whole: %1"))
                         .arg(namedGlyphs(c, true));
        if (const int wide = int(std::count_if(out.unwritable.begin(), out.unwritable.end(),
                                               [](const Unwritable& u) { return u.loss == Loss::Wide; }));
            countWide && wide)
            parts << plural(wide, QObject::tr("%n wide glyph written as ?"), QObject::tr("%n wide glyphs written as ?"));
    }
    if (controls)
        parts << plural(controls,
                        QObject::tr("%n cell holds a glyph whose CP437 byte .ans reads as a control code (%1) and "
                                    "will be written as '?' (XBIN and BIN keep it)"),
                        QObject::tr("%n cells hold a glyph whose CP437 byte .ans reads as a control code (%1) and "
                                    "will be written as '?' (XBIN and BIN keep them)"))
                     .arg(QString::fromStdString(ansControlNames(out.unwritable)));
    // From the editor this is blink the file had and lost when it was opened
    // with iCE colours that this save still writes without (see
    // Document::encode). It claims nothing about the original file, which a
    // Save As to a new path leaves alone.
    if (out.blinkDropped)
        parts << plural(
            out.blinkDropped,
            QObject::tr(
                "%n cell lost its blink when the file was opened with iCE colours, and this save doesn't put it back"),
            QObject::tr("%n cells lost their blink when the file was opened with iCE colours, and this save doesn't "
                        "put it back"));
    if (out.paletteDropped)
        parts << QObject::tr("the file's own palette is left out (only XBIN keeps one)");
    if (out.fontDropped)
        parts << QObject::tr("the file's own font is left out (only XBIN keeps one)");
    if (out.letterSpacingDropped)
        parts << QObject::tr("the 9 px letter spacing is left out (XBIN has none and draws 8 px)");
    if (out.aspectDropped)
        parts << (out.aspectDropped == 1 ? QObject::tr("the legacy aspect ratio is left out (XBIN has none)")
                                         : QObject::tr("the square aspect ratio is left out (XBIN has none)"));
    if (!out.fontNameDropped.empty())
        parts << QObject::tr("the SAUCE font name %1 is left out (XBIN names no font)")
                     .arg(QString::fromStdString(cp437ToUtf8(out.fontNameDropped)));
    if (out.coloursQuantised)
        parts << plural(out.coloursQuantised,
                        QObject::tr("%n cell is in a colour the file can't hold, saved as the nearest of the 16"),
                        QObject::tr("%n cells are in colours the file can't hold, saved as the nearest of the 16"));
    return parts.join(QStringLiteral(";\n"));
}

QString rejectedGlyphsMessage(const QString& text) {
    QStringList parts;
    QString uncoded;
    bool lastNamed = false;      // whether `uncoded` ends in a U+XXXX
    std::vector<char32_t> seen;  // each glyph's reason once, however often it was typed
    for (const char32_t ch : text.toUcs4()) {
        if (std::find(seen.begin(), seen.end(), ch) != seen.end())
            continue;
        seen.push_back(ch);
        // A control, bidi override or invisible character is named by its
        // code point (quotedGlyph), never put in the status bar raw.
        const QString glyph = QString::fromStdString(quotedGlyph(ch));
        if (const std::optional<std::uint8_t> b = ansControlByte(ch))
            parts << QObject::tr("%1 is CP437 0x%2, which .ans reads as %3")
                         .arg(glyph, QStringLiteral("%1").arg(uint(*b), 2, 16, QLatin1Char('0')).toUpper(),
                              QString::fromLatin1(ansControlName(*b)));
        else {
            // glyphs run together (★…); a code point is set off by spaces
            const bool named = unprintableCodePoint(ch);
            if (!uncoded.isEmpty() && (named || lastNamed))
                uncoded += QLatin1Char(' ');
            uncoded += glyph;
            lastNamed = named;
        }
    }
    if (!uncoded.isEmpty())
        parts << QObject::tr("%1 has no CP437 code").arg(uncoded);
    return QObject::tr("%1; not typed").arg(parts.join(QStringLiteral("; ")));
}

namespace {

// Save's question: the loss, then, for a CP437 .ans that loses glyphs a
// UTF-8 one would hold, the hint to Save As UTF-8.
QString lossQuestion(const AnsiOutput& out, bool project, ArtFormat format) {
    const QString before = project                       ? QObject::tr("Before saving the project:")
                           : format == ArtFormat::Xbin ? QObject::tr("Before saving as XBIN:")
                           : format == ArtFormat::Bin  ? QObject::tr("Before saving as BIN:")
                                                       : QObject::tr("Before saving as .ans:");
    QString hint;
    if (!project && format == ArtFormat::Ansi && out.encoding == TextEncoding::Cp437) {
        // what a UTF-8 .ans writes as it is: every code point it can hold,
        // a lead only when its glyph takes both cells, and no lone wide cell
        // or lone mark (utf8LoneLoss; a mark in column 0 is taken as after a
        // line break, which misses only the row after a full-width one)
        const auto kept = std::count_if(out.unwritable.begin(), out.unwritable.end(), [](const Unwritable& u) {
            const GlyphCodePoints cps = codePoints(u.ch);
            return std::all_of(cps.begin(), cps.end(), utf8Writable) &&
                   (u.lead ? glyphColumns(u.ch) == 2 : !utf8LoneLoss(u.ch, u.x > 0));
        });
        // No hint at all when nothing lost would survive: a save that only
        // drops blink, a palette or a font gains nothing from UTF-8.
        if (kept > 0) {
            const QString keeps =
                kept == std::ptrdiff_t(out.unwritable.size())
                    ? plural(int(kept), QObject::tr("A UTF-8 .ans keeps that cell's glyph"),
                             QObject::tr("A UTF-8 .ans keeps all %n of those glyphs"))
                    : plural(int(kept), QObject::tr("A UTF-8 .ans keeps the glyph of %n of those cells"),
                             QObject::tr("A UTF-8 .ans keeps the glyphs of %n of those cells"));
            // Its own paragraph after the loss list, which ends in its own full stop.
            hint = QStringLiteral("\n\n") +
                   QObject::tr("%1: Save As and choose \"%2\".").arg(keeps, utf8AnsiFilter());
        }
    }
    return QObject::tr("%1\n%2.%3\n\nSave anyway?").arg(before, lossDescription(out), hint);
}

} // namespace

QString saveWarning(const AnsiOutput& out, bool project, ArtFormat format) {
    return lossQuestion(out, project, format);
}

QString pipelineWarning(const AnsiOutput& out, PipelineAction action) {
    return action == PipelineAction::Publish
               ? QObject::tr("Before publishing:\n%1.\n\nPublish anyway?").arg(lossDescription(out, true))
               : QObject::tr("Before sending:\n%1.\n\nSend anyway?").arg(lossDescription(out, true));
}

SavePath resolveSavePath(const QString& chosen, const QString& suffix) {
    if (!QFileInfo(chosen).suffix().isEmpty())
        return {chosen, false};
    const QString path = chosen + QLatin1Char('.') + suffix;
    return {path, QFileInfo::exists(path)};
}


std::optional<TextEncoding> saveFilterEncoding(const QString& filter) {
    if (filter == ansiFilter())
        return TextEncoding::Cp437;
    if (filter == utf8AnsiFilter())
        return TextEncoding::Utf8;
    return std::nullopt;
}

void configureSaveDialog(QFileDialog& dialog, const QString& path, bool project, TextEncoding encoding) {
    // Each filter and the suffix it adds; "All files" keeps the last one.
    const std::vector<std::pair<QString, QString>> filters = {
        {ansiFilter(), QStringLiteral("ans")},
        {utf8AnsiFilter(), QStringLiteral("ans")},
        {QObject::tr("Blockwright project (*.bwp)"), QStringLiteral("bwp")},
        {QObject::tr("XBIN (*.xb)"), QStringLiteral("xb")},
        {QObject::tr("BIN (*.bin)"), QStringLiteral("bin")},
    };
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    QStringList names;
    for (const auto& f : filters)
        names << f.first;
    dialog.setNameFilters(names << QObject::tr("All files (*)"));
    const ArtFormat format = artFormatFor(QFile::encodeName(path).toStdString());
    const auto& selected = filters[project                            ? 2
                                   : format == ArtFormat::Xbin      ? 3
                                   : format == ArtFormat::Bin       ? 4
                                   : encoding == TextEncoding::Utf8 ? 1
                                                                    : 0];
    dialog.selectNameFilter(selected.first);
    dialog.setDefaultSuffix(selected.second);
    QObject::connect(&dialog, &QFileDialog::filterSelected, &dialog, [&dialog, filters](const QString& name) {
        for (const auto& f : filters)
            if (f.first == name)
                dialog.setDefaultSuffix(f.second);
    });
    if (!path.isEmpty()) {
        const QFileInfo fi(path);
        dialog.setDirectory(fi.absolutePath());
        dialog.selectFile(fi.fileName());
    }
}

QSize MainWindow::initialSize(QSize available) {
    // Leave room for the title bar, borders and a desktop panel even before
    // the window manager has supplied the frame's actual dimensions.
    available.rheight() = std::max(1, available.height() - 60);
    return QSize(1400, 900).boundedTo(available);
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    renderer_.setCanvas(&doc_.canvas(), glyphWidth(doc_.ansi()));
    doc_.scanFontsFolder(fontsFolderSetting());

    view_ = new CanvasView(&doc_, &renderer_);
    view_->setObjectName(QStringLiteral("canvasView"));
    auto* scroll = new GutterScrollArea;
    scroll_ = scroll;
    scroll_->setWidget(view_);
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setBackgroundRole(QPalette::Dark);
    setCentralWidget(scroll_);
    ircMeter_ = new IrcMeter(&doc_, view_, scroll);

    preview_ = new PreviewWidget(&renderer_);
    auto* previewScroll = new QScrollArea;
    previewScroll->setWidget(preview_);
    previewScroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    previewScroll->setBackgroundRole(QPalette::Dark);
    previewDock_ = new QDockWidget(tr("Preview (1x)"), this);
    previewDock_->setObjectName(QStringLiteral("previewDock"));
    // Always visible: it can move and float, but not close.
    previewDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    previewDock_->setWidget(previewScroll);
    addDockWidget(Qt::RightDockWidgetArea, previewDock_);

    palette_ = new PaletteWidget;
    palette_->setPen(view_->pen());
    paletteDock_ = new QDockWidget(tr("Palette"), this);
    paletteDock_->setObjectName(QStringLiteral("paletteDock"));
    paletteDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    paletteDock_->setWidget(palette_);
    addDockWidget(Qt::RightDockWidgetArea, paletteDock_);

    glyphSetLabel_ = new QLabel;
    glyphSetLabel_->setObjectName(QStringLiteral("glyphSetLabel"));
    statusBar()->addPermanentWidget(glyphSetLabel_);
    targetLabel_ = new QLabel;
    targetLabel_->setObjectName(QStringLiteral("targetLabel"));
    targetLabel_->setToolTip(tr("The document's target: ANSI's 16 colours or IRC's 99 (Format > Target)"));
    statusBar()->addPermanentWidget(targetLabel_);
    encodingLabel_ = new QLabel;
    encodingLabel_->setObjectName(QStringLiteral("encodingLabel"));
    encodingLabel_->setToolTip(tr("The text encoding Save writes an .ans in"));
    statusBar()->addPermanentWidget(encodingLabel_);
    status_ = new QLabel;
    statusBar()->addPermanentWidget(status_);

    connect(&doc_, &Document::cellsChanged, this, [this](const QRegion& cells) {
        renderer_.markDirty(cells);
        view_->cellsChanged(cells);
        preview_->cellsChanged(cells);
    });
    connect(&doc_, &Document::reset, this, &MainWindow::canvasReset);
    connect(&doc_, &Document::formatChanged, this, &MainWindow::formatChanged);
    connect(&doc_, &Document::modifiedChanged, this, &MainWindow::updateTitle);
    connect(&doc_, &Document::pathChanged, this, &MainWindow::updateTitle);
    connect(&doc_, &Document::pathChanged, this, &MainWindow::updateEncoding);
    connect(&doc_, &Document::encodingChanged, this, &MainWindow::updateEncoding);
    connect(&doc_, &Document::reset, this, &MainWindow::updateTarget);
    connect(&doc_, &Document::targetChanged, this, &MainWindow::updateTarget);
    connect(&doc_, &Document::fileWritten, this, [](const QString& path) { addRecentFile(path); });
    connect(view_, &CanvasView::cursorMoved, this, [this] {
        const QRect r = view_->toWidget(view_->cellRect(view_->cellCursor()));
        scroll_->ensureVisible(r.center().x(), r.center().y(), r.width(), r.height());
        updateStatus();
    });
    // the text cursor's cells and the bar drawn over their left edge, which
    // a big face makes taller than the view: its top shows then, and stays
    // put while it stays on those rows. ensureVisible keeps x ± xm in view
    // and tests the near side first, so a margin over half the viewport
    // would line up the top on one move and the bottom on the next.
    connect(view_, &CanvasView::caretMoved, this, [this](const QRect& cells) {
        const QRect px = view_->cellRect(cells.topLeft()).united(view_->cellRect(cells.bottomRight()));
        const QRect r = view_->toWidget(px.united(Tool::caretBar(px)));
        const QWidget* vp = scroll_->viewport();
        const int xm = std::min((r.width() + 1) / 2, vp->width() / 2);
        const int ym = std::min((r.height() + 1) / 2, vp->height() / 2);
        scroll_->ensureVisible(r.left() + xm, r.top() + ym, xm, ym);
    });
    connect(view_, &CanvasView::zoomChanged, this, &MainWindow::updateStatus);
    connect(palette_, &PaletteWidget::foregroundPicked, view_, &CanvasView::setForeground);
    connect(palette_, &PaletteWidget::backgroundPicked, view_, &CanvasView::setBackground);
    connect(view_, &CanvasView::penChanged, palette_, &PaletteWidget::setPen);
    connect(view_, &CanvasView::glyphSetChanged, this, &MainWindow::updateGlyphSet);
    connect(view_, &CanvasView::glyphRejected, this, [this](const QString& text) {
        statusBar()->showMessage(rejectedGlyphsMessage(text), 3000);
    });
    connect(view_, &CanvasView::layerMoveStatus, this, [this](const QString& text) {
        statusBar()->showMessage(text);
    });
    connect(view_, &CanvasView::statusNotice, this, [this](const QString& text) {
        statusBar()->showMessage(text, 5000);
    });

    createActions();
    createLibrary();
    createDocks();
    createGenerators();
    createReferenceActions();
    applyShortcutSettings(this);
    // The glyph-set line's tooltip names the sets' keys.
    for (const char* name : {"previousGlyphSet", "nextGlyphSet", "defaultGlyphSet"})
        connect(findChild<QAction*>(QString::fromLatin1(name)), &QAction::changed, this, &MainWindow::updateGlyphSet);
    for (QAction* set : std::as_const(glyphSetActions_))
        connect(set, &QAction::changed, this, &MainWindow::updateGlyphSet);
    // the first document is a new one, so it takes the default target
    if (defaultTargetSetting() != doc_.target())
        doc_.clear(Canvas::kDefaultWidth, Canvas::kDefaultHeight, true, defaultTargetSetting());
    updateTitle();
    updateEncoding();
    updateTarget();
    updateStatus();
    updateGlyphSet();
    resize(initialSize(screen()->availableGeometry().size()));
    restoreLayout();
    view_->setFocus();
}

MainWindow::~MainWindow() {
    // Members go before the child widgets (the base class deletes those),
    // renderer_ before doc_. Destroying doc_ clears its undo stack, whose
    // signals would end a drag the view still shows and repaint through the
    // dead renderer (#237); the view, the docks and this window's own slots
    // all follow the document. So the drag ends now, while everything it
    // touches is here, and nothing the document or the stack says once
    // teardown starts reaches anyone. A focus-out the view gets later ends
    // no drag, so it doesn't touch the document either. A focused dock
    // editor would write its edit into the document when the base class
    // hides the window, after doc_ is gone (#241), so it writes it first.
    commitFocusedEdit();
    view_->cancelDrag();
    doc_.blockSignals(true);
    doc_.undoStack()->blockSignals(true);
}

void MainWindow::createActions() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* a = file->addAction(tr("&New"), this, &MainWindow::newFile);
    a->setObjectName(QStringLiteral("newFile"));
    a->setShortcut(standardKeys(a, QKeySequence::New).value(0));
    a = file->addAction(tr("&Open..."), this, &MainWindow::open);
    a->setObjectName(QStringLiteral("open"));
    a->setShortcut(standardKeys(a, QKeySequence::Open).value(0));
    a = file->addAction(tr("Open Project Di&rectory..."), this, &MainWindow::openDirectory);
    a->setObjectName(QStringLiteral("openDirectory"));
    a = file->addAction(tr("Open S&cene..."), this, &MainWindow::openScene);
    a->setObjectName(QStringLiteral("openScene"));
    openRecentMenu_ = file->addMenu(tr("Open Rec&ent"));
    openRecentMenu_->setObjectName(QStringLiteral("openRecent"));
    connect(openRecentMenu_, &QMenu::aboutToShow, this, &MainWindow::fillRecentMenu);
    fillRecentMenu();
    reopenMenu_ = file->addMenu(tr("Reopen with Encodin&g"));
    reopenMenu_->setObjectName(QStringLiteral("reopenWithEncoding"));
    const std::pair<const char*, std::optional<TextEncoding>> reopen[] = {
        {"reopenAuto", std::nullopt}, {"reopenCp437", TextEncoding::Cp437}, {"reopenUtf8", TextEncoding::Utf8}};
    for (const auto& [name, enc] : reopen) {
        const QString text = !enc                          ? tr("&Auto (as its bytes say)")
                             : *enc == TextEncoding::Cp437 ? tr("&CP437")
                                                           : tr("&UTF-8");
        a = reopenMenu_->addAction(text, this, [this, enc] { reopenWithEncoding(enc); });
        a->setObjectName(QLatin1String(name));
    }
    a = file->addAction(tr("Star&t Screen..."), this, &MainWindow::showStartScreen);
    a->setObjectName(QStringLiteral("startScreen"));
    a = file->addAction(tr("&Save"), this, &MainWindow::save);
    a->setObjectName(QStringLiteral("save"));
    a->setShortcut(standardKeys(a, QKeySequence::Save).value(0));
    a = file->addAction(tr("Save &As..."), this, &MainWindow::saveAs);
    a->setObjectName(QStringLiteral("saveAs"));
    a->setShortcut(standardKeys(a, QKeySequence::SaveAs).value(0));
    createProjectActions(file);
    file->addSeparator();
    a = file->addAction(tr("SA&UCE..."), this, &MainWindow::editSauce);
    a->setObjectName(QStringLiteral("sauce"));
    file->addSeparator();
    (new Publisher(this))->addActions(file);
    file->addSeparator();
    a = file->addAction(tr("&Quit"), this, &QWidget::close);
    a->setObjectName(QStringLiteral("quit"));
    // Qt 6.10's generic theme gives Quit only the bare Exit key, which keyboard
    // mode would take; Ctrl+Q there, as the other desktops have it.
    const QKeySequence quit = standardKeys(a, QKeySequence::Quit).value(0);
    a->setShortcut(shortcutRefusal(quit).isEmpty() ? quit : QKeySequence(Qt::CTRL | Qt::Key_Q));

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    a = doc_.undoStack()->createUndoAction(this, tr("&Undo"));
    a->setObjectName(QStringLiteral("undo"));
    a->setShortcuts(standardKeys(a, QKeySequence::Undo));
    edit->addAction(a);
    a = doc_.undoStack()->createRedoAction(this, tr("&Redo"));
    a->setObjectName(QStringLiteral("redo"));
    a->setShortcuts(standardKeys(a, QKeySequence::Redo));
    // A snapshot redone into an .ans drops itself from the stack (#115), and
    // Qt before 6.9 drops it without updating this action. Connected after
    // the stack's own redo, this runs once the redo is done.
    connect(a, &QAction::triggered, doc_.undoStack(), [s = doc_.undoStack()] {
        emit s->canRedoChanged(s->canRedo());
        emit s->redoTextChanged(s->redoText());
    });
    edit->addAction(a);
    edit->addSeparator();
    // no shortcut: Ctrl+, (QKeySequence::Preferences) steps the glyph set
    a = edit->addAction(tr("&Preferences..."), this, &MainWindow::editPreferences);
    a->setObjectName(QStringLiteral("preferences"));
    a->setMenuRole(QAction::PreferencesRole);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    a = view->addAction(tr("Zoom &In"), this, &MainWindow::zoomIn);
    a->setObjectName(QStringLiteral("zoomIn"));
    a->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)});
    a = view->addAction(tr("Zoom &Out"), this, &MainWindow::zoomOut);
    a->setObjectName(QStringLiteral("zoomOut"));
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus));
    a = view->addAction(tr("&Actual Size"), this, &MainWindow::zoomReset);
    a->setObjectName(QStringLiteral("zoomReset"));
    a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    createFormatActions(view);
    createTargetActions(menuBar()->addMenu(tr("F&ormat")));
    createIrcActions(file, view);

    // Colours and glyph sets: Moebius's keys.
    const auto add = [this](QMenu* m, const QString& text, const char* name, QKeySequence key, auto slot) {
        QAction* act = m->addAction(text, this, slot);
        act->setObjectName(QString::fromLatin1(name));
        act->setShortcut(key);
        return act;
    };
    QMenu* colours = menuBar()->addMenu(tr("&Colours"));
    add(colours, tr("&Previous Foreground"), "previousForeground", QKeySequence(Qt::CTRL | Qt::Key_Up),
        [this] { stepColour(true, -1); });
    add(colours, tr("&Next Foreground"), "nextForeground", QKeySequence(Qt::CTRL | Qt::Key_Down),
        [this] { stepColour(true, 1); });
    add(colours, tr("P&revious Background"), "previousBackground", QKeySequence(Qt::CTRL | Qt::Key_Left),
        [this] { stepColour(false, -1); });
    add(colours, tr("N&ext Background"), "nextBackground", QKeySequence(Qt::CTRL | Qt::Key_Right),
        [this] { stepColour(false, 1); });
    colours->addSeparator();
    add(colours, tr("&Default Colours"), "defaultColours", QKeySequence(Qt::CTRL | Qt::Key_D),
        [this] { view_->setPen(Pen{}); });
    add(colours, tr("&Swap Colours"), "swapColours", QKeySequence(),
        [this] { view_->setPen(Pen{view_->pen().bg, view_->pen().fg}); });

    QMenu* glyphs = menuBar()->addMenu(tr("&Glyphs"));
    add(glyphs, tr("&Previous Set"), "previousGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Comma),
        [this] { view_->setGlyphSet(view_->glyphSet() - 1); });
    add(glyphs, tr("&Next Set"), "nextGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Period),
        [this] { view_->setGlyphSet(view_->glyphSet() + 1); });
    add(glyphs, tr("&Default Set"), "defaultGlyphSet", QKeySequence(Qt::CTRL | Qt::Key_Slash),
        [this] { view_->setGlyphSet(kDefaultGlyphSet); });
    glyphs->addSeparator();
    // Ctrl+Shift+F1-F10 pick sets 1-10; 11-16 are reached from this menu or by
    // stepping. Not Moebius's Alt+F-keys: desktops take those (Alt+F4 closes
    // the window), as they do Ctrl+F-keys (KDE and Xfce desktop switching) and
    // Ctrl+Alt+F-keys (Linux VT switching).
    auto* group = new QActionGroup(this);
    for (int i = 0; i < kGlyphSetCount; ++i) {
        QString shown;
        for (const char32_t ch : glyphSet(i))
            shown += QString::fromUcs4(&ch, 1);
        const QKeySequence key =
            i < kGlyphSetKeys ? QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key(Qt::Key_F1 + i)) : QKeySequence();
        QAction* set = add(glyphs, tr("Set %1: %2").arg(i + 1).arg(shown), "", key,
                           [this, i] { view_->setGlyphSet(i); });
        set->setObjectName(QStringLiteral("glyphSet%1").arg(i + 1));
        set->setCheckable(true);
        group->addAction(set);
        glyphSetActions_ << set;
    }

    createEditingActions(file, edit, view);
    // Filled in by createDocks.
    layerMenu_ = menuBar()->addMenu(tr("&Layer"));
    windowMenu_ = menuBar()->addMenu(tr("&Window"));

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    // Shift+F1, the standard What's This key: a click then says what the
    // part clicked does (whats_this.h). It takes Shift+F1 from keyboard
    // mode, which typed F1's glyph with it; Shift+F2 to F10 still type.
    a = help->addAction(tr("&What's This?"), this, [] { QWhatsThis::enterWhatsThisMode(); });
    a->setObjectName(QStringLiteral("whatsThis"));
    a->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F1));
    a->setStatusTip(tr("Click a toolbox button, a menu item or a tool option to see what it does"));
    // No shortcut: F1, the standard help key, types a glyph in keyboard mode.
    a = help->addAction(tr("&Keyboard Shortcuts..."), this, [this] { showShortcuts(this); });
    a->setObjectName(QStringLiteral("keyboardShortcuts"));
    a = help->addAction(tr("&Manual..."), this, [this] { showManual(*tools_, this); });
    a->setObjectName(QStringLiteral("manual"));
    a->setStatusTip(tr("What each tool, its options and each kind of layer do"));
    createSamplesMenu(help);
    help->addSeparator();
    a = help->addAction(tr("&About Blockwright"), this, [this] { showAbout(this); });
    a->setObjectName(QStringLiteral("about"));
    a = help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
    a->setObjectName(QStringLiteral("aboutQt"));
}

void MainWindow::updateGlyphSet() {
    const int set = view_->glyphSet();
    QStringList glyphs, tips;
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        const char32_t ch = glyphSetGlyph(set, k);
        glyphs << QString::fromUcs4(&ch, 1);
        tips << tr("F%1 %2").arg(k + 1).arg(glyphs.back());
    }
    glyphSetLabel_->setText(tr("F-keys set %1: %2").arg(set + 1).arg(glyphs.join(QLatin1Char(' '))));
    const auto key = [](const QAction* a) { return a->shortcut().toString(QKeySequence::NativeText); };
    const auto named = [this, &key](const char* name) { return key(findChild<QAction*>(QString::fromLatin1(name))); };
    // Only the keys the actions have: each line reads right with any of them gone.
    const QString previous = named("previousGlyphSet"), next = named("nextGlyphSet"), back = named("defaultGlyphSet");
    QStringList steps;
    if (!previous.isEmpty() && !next.isEmpty())
        steps << tr("%1 and %2 step through the sets").arg(previous, next);
    else if (!next.isEmpty())
        steps << tr("%1 steps through the sets").arg(next);
    else if (!previous.isEmpty())
        steps << tr("%1 steps back through the sets").arg(previous);
    if (!back.isEmpty())
        steps << tr("%1 goes back to the default").arg(back);
    // The sets' own keys, a run of F-keys under the same modifiers (the
    // default Ctrl+Shift+F1 to F10) as one range.
    const auto fkey = [](const QAction* a) -> std::optional<QKeyCombination> {
        const QKeySequence k = a->shortcut();
        if (k.count() != 1 || k[0].key() < Qt::Key_F1 || k[0].key() > Qt::Key_F35)
            return std::nullopt;
        return k[0];
    };
    QStringList picks;
    for (qsizetype i = 0; i < glyphSetActions_.size(); ++i) {
        const QAction* first = glyphSetActions_[i];
        if (first->shortcut().isEmpty())
            continue;
        qsizetype last = i;
        if (const auto from = fkey(first))
            while (last + 1 < glyphSetActions_.size()) {
                const auto to = fkey(glyphSetActions_[last + 1]);
                if (!to || to->keyboardModifiers() != from->keyboardModifiers() ||
                    to->key() != from->key() + int(last + 1 - i))
                    break;
                ++last;
            }
        if (last == i)
            picks << tr("%1 picks set %2").arg(key(first)).arg(i + 1);
        else
            picks << tr("%1 to %2 pick sets %3-%4")
                         .arg(key(first), key(glyphSetActions_[last]))
                         .arg(i + 1)
                         .arg(last + 1);
        i = last;
    }
    tips << QString();
    if (!steps.isEmpty())
        tips << steps.join(QStringLiteral(", "));
    tips << (picks.isEmpty() ? tr("Every set is in the Glyphs menu")
                             : tr("%1; every set is in the Glyphs menu").arg(picks.join(QStringLiteral(", "))));
    glyphSetLabel_->setToolTip(tips.join(QLatin1Char('\n')));
    if (set < glyphSetActions_.size())
        glyphSetActions_[set]->setChecked(true);
}

void MainWindow::canvasReset() {
    // openFile has already shown the renderer this canvas, and a blank one
    // always fits, so this does not throw.
    renderer_.setCanvas(&doc_.canvas(), glyphWidth(doc_.ansi()));
    renderer_.setIceColours(doc_.ansi().ice);
    syncFormatActions();
    view_->canvasReset();
    preview_->canvasReset();
    view_->setCellCursor({0, 0});
    updateStatus();
}

bool MainWindow::openFile(const QString& path, QString* error, std::optional<TextEncoding> encoding) {
    if (project::isProject(QFile::encodeName(path).toStdString()) || fileFormFor(path) != FileForm::Ansi) {
        if (!openProject(path, error))
            return false;
        addRecentFile(path);
        return true;
    }
    AnsiDocument candidate;
    if (!Document::read(path, &candidate, error, encoding))
        return false;
    // Let the renderer take the new canvas before the document does, so a
    // canvas too large to display leaves the open one as it was. setCanvas
    // changes nothing when it throws, and canvasReset (from replace) points
    // it at the document's copy before anything draws.
    try {
        renderer_.setCanvas(&candidate.canvas, glyphWidth(candidate));
    } catch (const std::exception&) {
        *error = tr("the canvas is too large to display");
        return false;
    }
    const QString dropped = readLossDescription(candidate.readLoss);
    doc_.replace(std::move(candidate), path);
    addRecentFile(path);
    if (!dropped.isEmpty())
        statusBar()->showMessage(tr("Opened %1: %2").arg(QDir::toNativeSeparators(path), dropped));
    return true;
}

bool MainWindow::newCanvas(QSize size, bool ice, QString* error) {
    if (size.width() < 1 || size.height() < 1 || size.width() > kMaxColumns || size.height() > kMaxRows) {
        *error = tr("a canvas is 1 to %1 columns wide and 1 to %2 rows high").arg(kMaxColumns).arg(kMaxRows);
        return false;
    }
    // As openFile: the renderer takes the canvas first, so one too large to
    // display leaves the document as it was.
    const Canvas candidate(size.width(), size.height());
    try {
        renderer_.setCanvas(&candidate, 8);
    } catch (const std::exception&) {
        *error = tr("the canvas is too large to display");
        return false;
    }
    doc_.clear(size.width(), size.height(), ice, defaultTargetSetting());
    return true;
}

void MainWindow::fillRecentMenu() {
    openRecentMenu_->clear();
    const QStringList list = recentFiles();
    for (const QString& path : list) {
        QString text = QDir::toNativeSeparators(path);
        QAction* a = openRecentMenu_->addAction(text.replace(QLatin1Char('&'), QStringLiteral("&&")), this,
                                                [this, path] { openRequested(path); });
        a->setEnabled(QFileInfo::exists(path));
    }
    if (list.isEmpty())
        openRecentMenu_->addAction(tr("(none)"))->setEnabled(false);
}

void MainWindow::showStartScreen() {
    if (!maybeSave())
        return;
    doc_.clear(Canvas::kDefaultWidth, Canvas::kDefaultHeight, true, defaultTargetSetting());
    auto* screen = new StartScreen(this);
    screen->setAttribute(Qt::WA_DeleteOnClose);
    screen->open();
}

bool MainWindow::saveFile(const QString& path, QString* error, bool allowLossy,
                          std::optional<TextEncoding> encoding) {
    const FileForm form = path == doc_.path() ? doc_.form() : fileFormFor(path);
    return writeFile(path, form, error, allowLossy, encoding);
}

void MainWindow::newFile() {
    if (maybeSave())
        doc_.clear(Canvas::kDefaultWidth, Canvas::kDefaultHeight, true, defaultTargetSetting());
}

void MainWindow::open() {
    if (!maybeSave())
        return;
    const QString dir = doc_.path().isEmpty() ? QString() : QFileInfo(doc_.path()).absolutePath();
    QString filter;
    const QString path = QFileDialog::getOpenFileName(promptParent(), tr("Open"), dir,
                                                      openNameFilters().join(QStringLiteral(";;")), &filter);
    if (!path.isEmpty())
        openInteractive(path, openFilterEncoding(filter));
}

void MainWindow::reopenWithEncoding(std::optional<TextEncoding> encoding) {
    if (!reopenable() || !maybeSave())
        return;
    // maybeSave may have saved it elsewhere: reopen the file it is now
    if (reopenable())
        openInteractive(doc_.path(), encoding);
}

bool MainWindow::reopenable() const {
    return doc_.form() == FileForm::Ansi && !doc_.path().isEmpty() &&
           artFormatFor(QFile::encodeName(doc_.path()).toStdString()) == ArtFormat::Ansi;
}

void MainWindow::updateEncoding() {
    encodingLabel_->setText(doc_.textEncoding() == TextEncoding::Utf8 ? tr("UTF-8") : tr("CP437"));
    if (reopenMenu_)
        reopenMenu_->setEnabled(reopenable());
}

void MainWindow::openRequested(const QString& path) {
    if (maybeSave())
        openInteractive(path);
}

void MainWindow::openInteractive(const QString& path, std::optional<TextEncoding> encoding) {
    QString error;
    if (openFile(path, &error, encoding))
        return;
    // On the heap: at launch its parent is the start screen, which a file
    // the system hands the app while the box is up closes and deletes.
    QPointer<QMessageBox> box = new QMessageBox(QMessageBox::Warning, tr("Open"),
                                                tr("Could not open %1:\n%2").arg(path, error), QMessageBox::Ok,
                                                promptParent());
    box->exec();
    delete box;
}

QWidget* MainWindow::promptParent() {
    if (isVisible())
        return this;
    QWidget* modal = QApplication::activeModalWidget();
    return modal ? modal : this;
}

bool MainWindow::save() { return doc_.path().isEmpty() ? saveAs() : saveInteractive(doc_.path(), doc_.form()); }

bool MainWindow::saveAs() {
    return doc_.form() == FileForm::ProjectDirectory ? saveAsDirectory() : saveAsFile();
}

bool MainWindow::saveAsFile() {
    QFileDialog dialog(this, tr("Save As"));
    configureSaveDialog(dialog, doc_.path(), doc_.form() != FileForm::Ansi, doc_.textEncoding());
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return false;
    // A platform dialog may not apply the default suffix; then the name
    // written is one the dialog never checked.
    const SavePath p = resolveSavePath(dialog.selectedFiles().constFirst(), dialog.defaultSuffix());
    if (p.confirmOverwrite &&
        QMessageBox::warning(this, tr("Save As"), tr("%1 already exists.\nDo you want to replace it?").arg(p.path),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    return saveInteractive(p.path, fileFormFor(p.path), saveFilterEncoding(dialog.selectedNameFilter()));
}

bool MainWindow::saveInteractive(const QString& path, FileForm form, std::optional<TextEncoding> encoding) {
    view_->finishTool();
    const AnsiOutput out = saveLoss(path, form, encoding);
    if (out.lossy()) {
        const auto answer = QMessageBox::warning(
            this, tr("Save"),
            saveWarning(out, form != FileForm::Ansi, artFormatFor(QFile::encodeName(path).toStdString())),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return false;
    }
    QString error;
    if (!(form == FileForm::Ansi ? doc_.save(path, out, &error) : doc_.saveProject(path, form, &error))) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2").arg(path, error));
        return false;
    }
    if (form == FileForm::Ansi && doc_.path() != path)
        statusBar()->showMessage(tr("Exported %1 flattened; the document is still %2").arg(path, doc_.path()), 5000);
    else if (doc_.isModified())
        statusBar()->showMessage(tr("Saved %1 flattened; only a project keeps the layers").arg(path), 5000);
    else
        statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
    return true;
}

void MainWindow::zoomIn() { view_->setZoom(view_->zoom() + 1); }
void MainWindow::zoomOut() { view_->setZoom(view_->zoom() - 1); }
void MainWindow::zoomReset() { view_->setZoom(1); }

void MainWindow::commitFocusedEdit() {
    // The view's own focus-out ends a drag; finishTool and ~MainWindow see
    // to that. A floating dock is a window of its own, so this walks the
    // parents rather than asking isAncestorOf, which stops at a window.
    QWidget* f = QApplication::focusWidget();
    if (!f || f == view_ || view_->isAncestorOf(f))
        return;
    for (const QWidget* w = f; w; w = w->parentWidget())
        if (w == this) {
            f->clearFocus();
            return;
        }
}

bool MainWindow::maybeSave() {
    // An edit still open in a dock is an unsaved change too: closing would
    // otherwise commit it after this check, with nobody asked (#241).
    commitFocusedEdit();
    // A floating selection is an unsaved change: stamped, it marks the
    // document modified and is saved with it.
    view_->finishTool();
    if (!doc_.isModified())
        return true;
    QString text = tr("The canvas has unsaved changes.");
    // Save writes the .ans, which can't hold the layers or an IRC budget:
    // they stay open. An untitled document asks for a name first, and
    // only an .ans name loses them.
    if (doc_.form() == FileForm::Ansi && !doc_.flatHoldsAll()) {
        if (doc_.path().isEmpty())
            text += tr("\nSaved as an .ans it is written flattened and the document stays open; only a project "
                       "saves its layers and IRC budget.");
        else
            text += tr("\nSave writes %1 flattened and keeps the document open; only a project saves its layers "
                       "and IRC budget.")
                        .arg(QFileInfo(doc_.path()).fileName());
    }
    const auto answer = QMessageBox::warning(this, tr("Blockwright"), text,
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Save);
    // A save that leaves the document modified didn't keep all of it.
    if (answer == QMessageBox::Save)
        return save() && !doc_.isModified();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (maybeSave()) {
        saveLayout();
        e->accept();
    } else {
        e->ignore();
    }
}

void MainWindow::updateTitle() {
    const QString name = doc_.path().isEmpty() ? tr("untitled.ans") : QFileInfo(doc_.path()).fileName();
    setWindowTitle(name + QStringLiteral("[*] - Blockwright"));
    setWindowFilePath(doc_.path());
    setWindowModified(doc_.isModified());
}

void MainWindow::updateStatus() {
    const Canvas& cv = doc_.canvas();
    const Cursor c = view_->cellCursor();
    status_->setText(tr("%1, %2  |  %3x%4  |  %5 px  |  %6x")
                         .arg(c.x)
                         .arg(c.y)
                         .arg(cv.width())
                         .arg(cv.height())
                         .arg(renderer_.cellSize().width())
                         .arg(view_->zoom()));
}

} // namespace bw
