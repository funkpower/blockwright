#include "glyph_bar.h"

#include "canvas_view.h"

#include "blockwright/canvas.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/render.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPixmap>
#include <QToolButton>

namespace bw {

namespace {

constexpr int kScale = 2;

// The glyph as the canvas draws it, light grey on black.
QIcon glyphIcon(char32_t ch) {
    Canvas c(1, 1);
    c.at(0, 0).ch = ch;
    return QIcon(QPixmap::fromImage(renderCanvas(c, 8, kScale)));
}

QString glyphText(char32_t ch) { return QString::fromUcs4(&ch, 1); }

} // namespace

GlyphBar::GlyphBar(CanvasView* view, QWidget* parent) : QWidget(parent), view_(view) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(2);
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("glyphBarCell%1").arg(k + 1));
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        b->setIconSize(QSize(8 * kScale, 16 * kScale));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setText(QStringLiteral("F%1").arg(k + 1));
        // The F-key's own event, so the bar can't drift from the keyboard.
        connect(b, &QToolButton::clicked, this, [this, k] {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_F1 + k, Qt::NoModifier);
            QApplication::sendEvent(view_, &press);
            view_->setFocus();
        });
        row->addWidget(b);
        cells_ << b;
    }
    row->addSpacing(8);
    set_ = new QComboBox;
    set_->setObjectName(QStringLiteral("glyphBarSet"));
    set_->setFocusPolicy(Qt::NoFocus);
    set_->setToolTip(tr("The glyph set F1-F10 type; the Glyphs menu has the keys that step through them"));
    row->addWidget(set_);
    row->addStretch(1);

    sync();
    connect(view_, &CanvasView::glyphSetChanged, this, &GlyphBar::sync);
    connect(set_, &QComboBox::activated, this, [this](int i) {
        view_->setGlyphSet(set_->itemData(i).toInt());
        view_->setFocus();
    });
}

void GlyphBar::sync() {
    // the sets the typing policy offers, the Unicode ones only under Unicode
    const int offered = glyphSetCount(view_->typingPolicy());
    while (set_->count() > offered)
        set_->removeItem(set_->count() - 1);
    for (int i = set_->count(); i < offered; ++i) {
        QString shown;
        for (const char32_t ch : glyphSet(i))
            shown += glyphText(ch);
        set_->addItem(tr("Set %1: %2").arg(i + 1).arg(shown), i);
    }
    const int set = view_->glyphSet();
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        const char32_t ch = glyphSetGlyph(set, k);
        cells_[k]->setIcon(glyphIcon(ch));
        cells_[k]->setToolTip(tr("F%1: %2 (types it, or picks it for the glyph brush)").arg(k + 1).arg(glyphText(ch)));
    }
    set_->setCurrentIndex(set_->findData(set));
}

} // namespace bw
