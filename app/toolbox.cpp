#include "toolbox.h"

#include "tool.h"

#include <QAction>
#include <QGridLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSet>
#include <QToolButton>

namespace bw {

namespace {

constexpr int kColumns = 2;
constexpr int kIcon = 24;

// The hint a group's tools carry in their tooltips.
QString groupHint() { return Toolbox::tr("Hold or right-click for the rest of the group"); }

} // namespace

const QList<QStringList>& Toolbox::groups() {
    static const QList<QStringList> g{
        {QStringLiteral("keyboard")},
        {QStringLiteral("select")},
        // The brushes that lay a colour or a glyph down.
        {QStringLiteral("halfBlock"), QStringLiteral("shade"), QStringLiteral("glyph"), QStringLiteral("colorize")},
        // The brushes that walk a ramp.
        {QStringLiteral("rampShade"), QStringLiteral("form"), QStringLiteral("soft"), QStringLiteral("despeckle")},
        // The detail brushes, which find an edge.
        {QStringLiteral("outline"), QStringLiteral("rimLight")},
        {QStringLiteral("line"), QStringLiteral("rectangle"), QStringLiteral("ellipse")},
        {QStringLiteral("fill")},
        {QStringLiteral("eyedropper")},
    };
    return g;
}

QIcon toolIcon(const QString& id) {
    QPixmap pm(kIcon, kIcon);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(40, 40, 40), mid(130, 130, 130), light(220, 220, 220);
    p.setPen(QPen(ink, 1.5));
    const QRectF box(3.5, 3.5, kIcon - 7, kIcon - 7);
    const auto cells = [&](std::initializer_list<QColor> shades) {
        // A row of cells, dark to light, like a ramp.
        const qreal w = box.width() / qreal(shades.size());
        qreal x = box.left();
        for (const QColor& c : shades) {
            p.fillRect(QRectF(x, box.top() + 4, w, box.height() - 8), c);
            x += w;
        }
        p.setBrush(Qt::NoBrush);
        p.drawRect(box.adjusted(0, 4, 0, -4));
    };
    if (id == QLatin1String("keyboard")) {
        p.drawRoundedRect(box.adjusted(0, 4, 0, -4), 2, 2);
        for (int i = 0; i < 4; ++i)
            p.drawPoint(QPointF(box.left() + 3 + i * 4, box.center().y() - 1));
        p.drawLine(QPointF(box.left() + 4, box.center().y() + 3), QPointF(box.right() - 4, box.center().y() + 3));
    } else if (id == QLatin1String("select")) {
        p.setPen(QPen(ink, 1.5, Qt::DashLine));
        p.drawRect(box);
    } else if (id == QLatin1String("halfBlock")) {
        p.fillRect(QRectF(box.left(), box.top(), box.width(), box.height() / 2), ink);
        p.drawRect(box);
    } else if (id == QLatin1String("shade")) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if ((x + y) % 2 == 0)
                    p.fillRect(QRectF(box.left() + x * box.width() / 8, box.top() + y * box.height() / 8,
                                      box.width() / 8, box.height() / 8),
                               ink);
    } else if (id == QLatin1String("rampShade")) {
        cells({ink, mid, light});
    } else if (id == QLatin1String("form")) {
        QRadialGradient g(box.center() - QPointF(4, 4), box.width() / 1.6);
        g.setColorAt(0, light);
        g.setColorAt(1, ink);
        p.setBrush(g);
        p.drawEllipse(box);
    } else if (id == QLatin1String("soft")) {
        QRadialGradient g(box.center(), box.width() / 2);
        g.setColorAt(0, ink);
        g.setColorAt(1, Qt::transparent);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(box);
    } else if (id == QLatin1String("despeckle")) {
        p.fillRect(box, mid);
        p.fillRect(QRectF(box.center().x() - 2, box.center().y() - 2, 4, 4), light);
        p.setPen(QPen(ink, 1.5));
        p.drawLine(box.topRight(), box.bottomLeft());
    } else if (id == QLatin1String("glyph")) {
        QFont f = p.font();
        f.setPixelSize(kIcon - 6);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, kIcon, kIcon), Qt::AlignCenter, QStringLiteral("A"));
    } else if (id == QLatin1String("colorize")) {
        p.setPen(Qt::NoPen);
        const QColor hues[] = {QColor(200, 40, 40), QColor(40, 160, 40), QColor(40, 80, 200)};
        for (int i = 0; i < 3; ++i) {
            p.setBrush(hues[i]);
            p.drawEllipse(QRectF(box.left() + i * 5, box.top() + (i % 2) * 6, 9, 9));
        }
        p.setPen(QPen(ink, 1.5));
        p.drawLine(QPointF(box.left() + 2, box.bottom()), QPointF(box.right(), box.top() + 8));
    } else if (id == QLatin1String("line")) {
        p.drawLine(box.bottomLeft(), box.topRight());
    } else if (id == QLatin1String("rectangle")) {
        p.drawRect(box);
    } else if (id == QLatin1String("ellipse")) {
        p.drawEllipse(box);
    } else if (id == QLatin1String("fill")) {
        QPainterPath bucket;
        bucket.moveTo(box.left() + 2, box.top() + 6);
        bucket.lineTo(box.right() - 6, box.top() + 6);
        bucket.lineTo(box.right() - 8, box.bottom());
        bucket.lineTo(box.left() + 4, box.bottom());
        bucket.closeSubpath();
        p.setBrush(mid);
        p.drawPath(bucket);
        p.setBrush(ink);
        p.drawEllipse(QRectF(box.right() - 4, box.top() + 8, 4, 6));
    } else if (id == QLatin1String("eyedropper")) {
        p.drawLine(QPointF(box.left() + 1, box.bottom() - 1), QPointF(box.right() - 4, box.top() + 4));
        p.setBrush(ink);
        p.drawEllipse(QRectF(box.right() - 7, box.top(), 7, 7));
    } else if (id == QLatin1String("outline")) {
        p.fillRect(box.adjusted(3, 3, -3, -3), mid);
        p.setPen(QPen(ink, 2.5));
        p.drawRect(box.adjusted(2, 2, -2, -2));
    } else if (id == QLatin1String("rimLight")) {
        p.setBrush(mid);
        p.drawEllipse(box);
        p.setPen(QPen(light, 3));
        p.drawArc(box.adjusted(1.5, 1.5, -1.5, -1.5), 30 * 16, 120 * 16);
    } else {
        // A tool with no picture of its own: its name's first letter.
        p.drawText(QRectF(0, 0, kIcon, kIcon), Qt::AlignCenter, id.left(1).toUpper());
    }
    return QIcon(pm);
}

Toolbox::Toolbox(ToolRegistry* tools, const QHash<QString, QAction*>& actions, QWidget* parent)
    : QWidget(parent), actions_(actions) {
    QList<QStringList> layout;
    QSet<QString> placed;
    for (const QStringList& group : groups()) {
        QStringList present;
        for (const QString& id : group)
            if (tools->find(id) && actions.contains(id))
                present << id;
        if (!present.isEmpty())
            layout << present;
        for (const QString& id : present)
            placed.insert(id);
    }
    for (Tool* t : tools->tools())
        if (!placed.contains(t->id()) && actions.contains(t->id()))
            layout << QStringList{t->id()};

    auto* grid = new QGridLayout(this);
    grid->setSpacing(2);
    grid->setContentsMargins(4, 4, 4, 4);
    for (const QStringList& group : layout) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("toolbox_") + group.front());
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIconSize(QSize(kIcon, kIcon));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setDefaultAction(actions.value(group.front()));
        if (group.size() > 1) {
            auto* menu = new QMenu(b);
            menu->setObjectName(b->objectName() + QStringLiteral("Menu"));
            for (const QString& id : group) {
                QAction* a = actions.value(id);
                menu->addAction(a);
                if (!a->toolTip().contains(groupHint()))
                    a->setToolTip(a->toolTip() + QLatin1Char('\n') + groupHint());
            }
            b->setMenu(menu);
            b->setPopupMode(QToolButton::DelayedPopup);
            b->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(b, &QWidget::customContextMenuRequested, b, &QToolButton::showMenu);
        }
        const int i = int(buttons_.size());
        grid->addWidget(b, i / kColumns, i % kColumns);
        buttons_ << b;
        for (const QString& id : group)
            byTool_.insert(id, b);
    }
    grid->setRowStretch(int((buttons_.size() + kColumns - 1) / kColumns), 1);
    grid->setColumnStretch(kColumns, 1);

    if (tools->active())
        follow(tools->active());
    connect(tools, &ToolRegistry::activeChanged, this, &Toolbox::follow);
}

void Toolbox::follow(Tool* tool) {
    // The group's button takes the tool picked, from its menu or anywhere.
    if (!tool)
        return;
    QToolButton* b = byTool_.value(tool->id());
    QAction* a = actions_.value(tool->id());
    if (b && a && b->defaultAction() != a) {
        QMenu* menu = b->menu();
        b->setDefaultAction(a);
        b->setMenu(menu);
    }
}

} // namespace bw
