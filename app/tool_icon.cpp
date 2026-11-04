#include "tool_icon.h"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QSvgRenderer>
#include <QWidget>

#include <cmath>
#include <utility>

namespace bw {

namespace {

// The grid every icon is drawn on, as Tabler's are: 24 units, a 2-unit stroke
// with round caps and joins, the drawing kept within 2 units of the edge.
constexpr qreal kGrid = 24;

QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

qreal luminance(const QColor& c) {
    const auto lin = [](float v) { return v <= 0.04045f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

// The colours one icon is drawn in. Outlines are always `ink`; the tones of
// a fill run from the darker of ink and paper to the lighter, so a lit side
// is the light one on either scheme.
struct Tones {
    QColor ink, paper;
    QColor dark, light;

    explicit Tones(const ToolIconColours& c) : ink(c.ink), paper(c.paper) {
        const bool inkDark = luminance(ink) <= luminance(paper);
        dark = inkDark ? ink : paper;
        light = inkDark ? paper : ink;
    }
    // From the dark end (0) to the light (1).
    QColor ramp(qreal t) const { return mix(dark, light, t); }
    // Between the ink (0) and the paper (1).
    QColor towardPaper(qreal t) const { return mix(ink, paper, t); }
    QPen pen(qreal width = 2) const { return QPen(ink, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); }
};

// A character cell, twice as high as it is wide, as the half-block tools see it.
const QRectF kCell(6, 3, 12, 18);

QPainterPath rounded(const QRectF& r, qreal radius = 2) {
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    return path;
}

void outlineCell(QPainter& p, const Tones& t) {
    p.setPen(t.pen());
    p.setBrush(Qt::NoBrush);
    p.drawPath(rounded(kCell));
}

void drawHalfBlock(QPainter& p, const Tones& t) {
    // ▀: the cell's upper pixel set
    p.save();
    p.setClipRect(QRectF(kCell.left(), kCell.top(), kCell.width(), kCell.height() / 2));
    p.fillPath(rounded(kCell), t.ink);
    p.restore();
    outlineCell(p, t);
}

void drawShade(QPainter& p, const Tones& t) {
    // ▒ in a cell
    p.save();
    p.setClipPath(rounded(kCell));
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 6; ++x)
            if ((x + y) % 2 == 0)
                p.fillRect(QRectF(kCell.left() + x * 2, kCell.top() + y * 2, 2, 2), t.ink);
    p.restore();
    outlineCell(p, t);
}

void drawGlyph(QPainter& p, const Tones& t) {
    // ♥, a CP437 glyph, in a cell
    QPainterPath heart;
    heart.moveTo(12, 16.5);
    heart.cubicTo(6.5, 13, 7.5, 8, 10, 8);
    heart.cubicTo(11.2, 8, 12, 9, 12, 10);
    heart.cubicTo(12, 9, 12.8, 8, 14, 8);
    heart.cubicTo(16.5, 8, 17.5, 13, 12, 16.5);
    p.fillPath(heart, t.ink);
    outlineCell(p, t);
}

void drawColorize(QPainter& p, const Tones& t) {
    // three hues, each outlined in the ink so none is lost on either scheme
    const QColor hues[] = {QColor(200, 40, 40), QColor(40, 160, 40), QColor(40, 80, 200)};
    const QPointF at[] = {QPointF(12, 8), QPointF(8, 15), QPointF(16, 15)};
    p.setPen(t.pen(1.5));
    for (int i = 0; i < 3; ++i) {
        p.setBrush(hues[i]);
        p.drawEllipse(at[i], 4.75, 4.75);
    }
}

void drawRampShade(QPainter& p, const Tones& t) {
    // three rungs of a ramp, and the arrow both ways: it lightens or darkens
    const QRectF bar(3, 4, 18, 11);
    const qreal w = bar.width() / 3;
    p.save();
    p.setClipPath(rounded(bar));
    for (int i = 0; i < 3; ++i)
        p.fillRect(QRectF(bar.left() + i * w, bar.top(), w, bar.height()), t.ramp(0.1 + 0.4 * i));
    p.restore();
    p.setPen(t.pen());
    p.setBrush(Qt::NoBrush);
    p.drawPath(rounded(bar));
    p.drawLine(QPointF(bar.left() + w, bar.top()), QPointF(bar.left() + w, bar.bottom()));
    p.drawLine(QPointF(bar.left() + 2 * w, bar.top()), QPointF(bar.left() + 2 * w, bar.bottom()));
    p.drawLine(QPointF(5, 19.5), QPointF(19, 19.5));
    p.drawPolyline(QPolygonF({QPointF(7.5, 17), QPointF(5, 19.5), QPointF(7.5, 22)}));
    p.drawPolyline(QPolygonF({QPointF(16.5, 17), QPointF(19, 19.5), QPointF(16.5, 22)}));
}

void drawGradient(QPainter& p, const Tones& t) {
    // a box filled in bands along the ramp
    const QRectF box(3, 3, 18, 18);
    const qreal w = box.width() / 4;
    p.save();
    p.setClipPath(rounded(box));
    for (int i = 0; i < 4; ++i)
        p.fillRect(QRectF(box.left() + i * w, box.top(), w, box.height()), t.ramp(i / 3.0));
    p.restore();
    p.setPen(t.pen());
    p.setBrush(Qt::NoBrush);
    p.drawPath(rounded(box));
}

void drawForm(QPainter& p, const Tones& t) {
    // a dome lit from the top left
    const QPointF c(12, 12);
    const qreal r = 8.5;
    QPainterPath disc;
    disc.addEllipse(c, r, r);
    p.save();
    p.setClipPath(disc);
    p.fillPath(disc, t.ramp(0.1));
    p.setPen(Qt::NoPen);
    p.setBrush(t.ramp(0.5));
    p.drawEllipse(QPointF(10.5, 10.5), 7, 7);
    p.setBrush(t.ramp(0.9));
    p.drawEllipse(QPointF(9, 9), 3.75, 3.75);
    p.restore();
    p.setPen(t.pen());
    p.setBrush(Qt::NoBrush);
    p.drawPath(disc);
}

void drawSoft(QPainter& p, const Tones& t) {
    // a tip that is strongest in the middle and fades in bands
    const QPointF c(12, 12);
    p.setPen(Qt::NoPen);
    p.setBrush(t.ink);
    p.drawEllipse(c, 3.25, 3.25);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(t.towardPaper(0.4), 2));
    p.drawEllipse(c, 6, 6);
    QPen dots = t.pen(2);
    dots.setDashPattern({0.01, 2});
    p.setPen(dots);
    p.drawEllipse(c, 9, 9);
}

void drawDespeckle(QPainter& p, const Tones& t) {
    // an even patch of cells, and a sparkle: the odd one out is gone
    p.setPen(t.pen(1.25));
    p.setBrush(t.towardPaper(0.5));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            p.drawRoundedRect(QRectF(3 + x * 5, 8 + y * 5, 4, 4), 0.75, 0.75);
    QPainterPath star;
    const QPointF c(19, 5);
    star.moveTo(c + QPointF(0, -3.5));
    star.quadTo(c, c + QPointF(3.5, 0));
    star.quadTo(c, c + QPointF(0, 3.5));
    star.quadTo(c, c + QPointF(-3.5, 0));
    star.quadTo(c, c + QPointF(0, -3.5));
    p.setPen(t.pen(1));
    p.setBrush(t.ink);
    p.drawPath(star);
}

void drawSmudge(QPainter& p, const Tones& t) {
    // paint picked up and dragged into a tapering streak
    QPainterPath tail;
    tail.moveTo(8, 7.5);
    tail.cubicTo(13, 7.5, 17, 10, 21, 12);
    tail.cubicTo(17, 14, 13, 16.5, 8, 16.5);
    tail.closeSubpath();
    p.setPen(t.pen(1.5));
    p.setBrush(t.towardPaper(0.5));
    p.drawPath(tail);
    p.setPen(Qt::NoPen);
    p.setBrush(t.ink);
    p.drawEllipse(QPointF(7.5, 12), 4.5, 4.5);
    p.setPen(t.pen(1.5));
    p.drawLine(QPointF(12, 4.5), QPointF(18, 6.5));
    p.drawLine(QPointF(12, 19.5), QPointF(18, 17.5));
}

void drawOutline(QPainter& p, const Tones& t) {
    // a shape with a solid line drawn round it
    QPainterPath shape;
    shape.addEllipse(QPointF(10, 13.5), 5, 5);
    QPainterPath lobe;
    lobe.addEllipse(QPointF(14.5, 10), 4, 4);
    shape = shape.united(lobe);
    p.setPen(QPen(t.ink, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    p.fillPath(shape, t.towardPaper(0.5));
}

void drawFrameTool(QPainter& p, const Tones& t) {
    // a double-line border with solid corner blocks
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(t.ink, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.drawRect(QRectF(3, 4, 18, 16));
    p.setPen(QPen(t.towardPaper(0.35), 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    p.drawRect(QRectF(6.5, 7.5, 11, 9));
    for (const QPointF& c : {QPointF(3, 4), QPointF(21, 4), QPointF(3, 20), QPointF(21, 20)})
        p.fillRect(QRectF(c.x() - 2, c.y() - 2, 4, 4), t.ink);
}

void drawRimLight(QPainter& p, const Tones& t) {
    // a form whose edge towards the light is lit
    const QPointF c(13.5, 13.5);
    p.setPen(t.pen(1.5));
    p.setBrush(t.ramp(0.35));
    p.drawEllipse(c, 7, 7);
    p.setPen(QPen(t.light, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - 5, c.y() - 5, 10, 10), 100 * 16, 70 * 16);
    p.setPen(t.pen());
    for (const qreal deg : {105.0, 135.0, 165.0}) {
        const qreal a = qDegreesToRadians(deg);
        const QPointF d(std::cos(a), -std::sin(a));
        p.drawLine(c + d * 9.5, c + d * 11.5);
    }
}

void drawTexture(QPainter& p, const Tones& t) {
    // overlapping scales in a box
    const QRectF box(3, 3, 18, 18);
    p.save();
    p.setClipPath(rounded(box));
    p.setPen(t.pen(1.5));
    p.setBrush(Qt::NoBrush);
    for (int row = 0; row < 4; ++row)
        for (int col = -1; col < 4; ++col) {
            const QPointF c(3 + col * 6 + (row % 2) * 3, 4 + row * 5);
            p.drawArc(QRectF(c.x() - 3, c.y() - 3, 6, 6), 180 * 16, 180 * 16);
        }
    p.restore();
    p.setPen(t.pen());
    p.drawPath(rounded(box));
}

void drawLetter(QPainter& p, const Tones& t, const QString& id) {
    // a tool with no icon of its own: its name's first letter
    QFont f = p.font();
    f.setPixelSize(16);
    f.setBold(true);
    p.setFont(f);
    p.setPen(t.ink);
    p.drawText(QRectF(0, 0, kGrid, kGrid), Qt::AlignCenter, id.left(1).toUpper());
}

using Draw = void (*)(QPainter&, const Tones&);

const QHash<QString, Draw>& drawn() {
    static const QHash<QString, Draw> d{
        {QStringLiteral("halfBlock"), drawHalfBlock}, {QStringLiteral("shade"), drawShade},
        {QStringLiteral("glyph"), drawGlyph},         {QStringLiteral("colorize"), drawColorize},
        {QStringLiteral("rampShade"), drawRampShade}, {QStringLiteral("form"), drawForm},
        {QStringLiteral("soft"), drawSoft},           {QStringLiteral("despeckle"), drawDespeckle},
        {QStringLiteral("smudge"), drawSmudge},       {QStringLiteral("outline"), drawOutline},
        {QStringLiteral("rimLight"), drawRimLight},   {QStringLiteral("texture"), drawTexture},
        {QStringLiteral("gradient"), drawGradient},  {QStringLiteral("frame"), drawFrameTool},
    };
    return d;
}

const QHash<QString, QString>& tabler() {
    static const QHash<QString, QString> s{
        {QStringLiteral("keyboard"), QStringLiteral("keyboard")},
        {QStringLiteral("select"), QStringLiteral("marquee-2")},
        {QStringLiteral("move"), QStringLiteral("arrows-move")},
        {QStringLiteral("line"), QStringLiteral("line")},
        {QStringLiteral("rectangle"), QStringLiteral("square")},
        {QStringLiteral("ellipse"), QStringLiteral("oval")},
        {QStringLiteral("fill"), QStringLiteral("bucket-droplet")},
        {QStringLiteral("eyedropper"), QStringLiteral("color-picker")},
        {QStringLiteral("text"), QStringLiteral("typography")},
        {QStringLiteral("clone"), QStringLiteral("rubber-stamp")},
        {QStringLiteral("spray"), QStringLiteral("spray")},
    };
    return s;
}

// A Tabler icon in the ink: its strokes are currentColor.
void drawTabler(QPainter& p, const Tones& t, const QString& name) {
    static QHash<QString, QByteArray> files;
    auto it = files.find(name);
    if (it == files.end()) {
        QFile f(QStringLiteral(":/tool-icons/%1.svg").arg(name));
        it = files.insert(name, f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
    }
    QByteArray svg = *it;
    svg.replace("currentColor", t.ink.name(QColor::HexRgb).toLatin1());
    QSvgRenderer r(svg);
    p.save();
    p.setOpacity(p.opacity() * t.ink.alphaF());
    r.render(&p, QRectF(0, 0, kGrid, kGrid));
    p.restore();
}

class ToolIconEngine : public QIconEngine {
public:
    ToolIconEngine(QString id, const QWidget* paletteFrom) : id_(std::move(id)), paletteFrom_(paletteFrom) {}

    QIconEngine* clone() const override { return new ToolIconEngine(id_, paletteFrom_); }
    QString key() const override { return QStringLiteral("bwToolIcon"); }
    bool isNull() override { return false; }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }

    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal dpr = p->device() ? p->device()->devicePixelRatio() : 1;
        p->drawPixmap(rect, draw(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return draw(size, mode, state, 1);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        // QIcon::pixmap() passes the size in device pixels before 6.8.
        return draw(QSize(qRound(size.width() / scale), qRound(size.height() / scale)), mode, state, scale);
#else
        return draw(size, mode, state, scale);
#endif
    }

private:
    // The icon at `size` logical pixels and device pixel ratio `scale`.
    QPixmap draw(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) {
        const QPalette palette = paletteFrom_ ? paletteFrom_->palette() : QGuiApplication::palette();
        if (palette.cacheKey() != paletteKey_) {
            cache_.clear();
            paletteKey_ = palette.cacheKey();
        }
        const QString key = QStringLiteral("%1x%2@%3/%4/%5")
                                .arg(size.width())
                                .arg(size.height())
                                .arg(scale)
                                .arg(int(mode))
                                .arg(int(state));
        if (auto it = cache_.constFind(key); it != cache_.cend())
            return *it;
        QPixmap pm(QSize(qRound(size.width() * scale), qRound(size.height() * scale)));
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        if (!size.isEmpty()) {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const qreal side = qMin(size.width(), size.height());
            p.translate((size.width() - side) / 2, (size.height() - side) / 2);
            p.scale(side / kGrid, side / kGrid);
            const Tones t(toolIconColours(palette, mode));
            if (const QString svg = tabler().value(id_); !svg.isEmpty())
                drawTabler(p, t, svg);
            else if (const Draw d = drawn().value(id_))
                d(p, t);
            else
                drawLetter(p, t, id_);
        }
        cache_.insert(key, pm);
        return pm;
    }

    QString id_;
    QPointer<const QWidget> paletteFrom_;
    qint64 paletteKey_ = -1;
    QHash<QString, QPixmap> cache_;
};

} // namespace

ToolIconColours toolIconColours(const QPalette& palette, QIcon::Mode mode) {
    const QPalette::ColorGroup g = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
    if (mode == QIcon::Selected)
        return {palette.color(g, QPalette::HighlightedText), palette.color(g, QPalette::Highlight)};
    return {palette.color(g, QPalette::ButtonText), palette.color(g, QPalette::Button)};
}

QString toolIconSource(const QString& toolId) { return tabler().value(toolId); }

QIcon toolIcon(const QString& toolId, const QWidget* paletteFrom) {
    return QIcon(new ToolIconEngine(toolId, paletteFrom));
}

} // namespace bw
