// The check boxes in the Layers dock and the Layer Style dialog: every row
// has its own, drawn where the row is and toggling that row only. QMacStyle
// before Qt's fix for QTBUG-150017 drew a box shifted by its place in the
// view, so on macOS 27 only the first row's showed (item_check_style.h).

#include "item_check_style.h"
#include "layer_style_dialog.h"
#include "layers_dock.h"
#include "main_window.h"

#include "document.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QCommonStyle>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStyleOptionViewItem>
#include <QTreeWidget>

#include <filesystem>
#include <memory>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

// What the style under an ItemCheckStyle was asked to draw (it draws
// nothing): the element, its
// rectangle, and where the rectangle's corner fell on the device.
struct Drawn {
    QStyle::PrimitiveElement pe;
    QRect rect;
    QPoint device;
};

class RecordingStyle : public QCommonStyle {
public:
    explicit RecordingStyle(std::vector<Drawn>* drawn) : drawn_(drawn) {}
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p, const QWidget*) const override {
        drawn_->push_back({pe, opt->rect, p->worldTransform().map(opt->rect.topLeft())});
    }

private:
    std::vector<Drawn>* drawn_;
};

// Counts the palettes it is polished with, as QMacStyle drops its cached
// native controls on each.
class PaletteCountingStyle : public QCommonStyle {
public:
    explicit PaletteCountingStyle(int* polished) : polished_(polished) {}
    using QCommonStyle::polish;
    void polish(QPalette& pal) override {
        ++*polished_;
        QCommonStyle::polish(pal);
    }

private:
    int* polished_;
};

// Makes fixItemChecks act under any style while it lives.
struct ForceItemChecks {
    ForceItemChecks() { forceItemChecks(true); }
    ~ForceItemChecks() { forceItemChecks(false); }
};

// Presses and releases the left button on the check box of `index`, as the
// user clicks it.
void clickBox(QAbstractItemView* view, const QModelIndex& index) {
    view->scrollTo(index);
    QStyleOptionViewItem opt;
    opt.initFrom(view);
    opt.rect = view->visualRect(index);
    opt.features = QStyleOptionViewItem::HasCheckIndicator;
    opt.text = index.data().toString();
    const QPointF at =
        QRectF(view->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, view)).center();
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, view->viewport()->mapToGlobal(at), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, {});
        QApplication::sendEvent(view->viewport(), &e);
    }
}

} // namespace

TEST_CASE("ItemCheckStyle draws an item's check box at the painter's origin, moved to its place", "[layers][item-checks]") {
    std::vector<Drawn> drawn;
    ItemCheckStyle style(new RecordingStyle(&drawn));
    QImage image(200, 200, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&image);
    QStyleOptionViewItem opt;
    opt.state = QStyle::State_Enabled | QStyle::State_On;

    SECTION("a box away from the view's corner") {
        opt.rect = QRect(40, 60, 16, 16);
        style.drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &opt, &p);
        REQUIRE(drawn.size() == 1);
        CHECK(drawn[0].pe == QStyle::PE_IndicatorItemViewItemCheck);
        CHECK(drawn[0].rect == QRect(0, 0, 16, 16));
        CHECK(drawn[0].device == QPoint(40, 60));
        // the painter is given back as it was
        CHECK(p.worldTransform().isIdentity());
    }
    SECTION("a box at the corner, and any other element, as they come") {
        opt.rect = QRect(0, 0, 16, 16);
        style.drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &opt, &p);
        opt.rect = QRect(40, 60, 16, 16);
        style.drawPrimitive(QStyle::PE_IndicatorCheckBox, &opt, &p);
        REQUIRE(drawn.size() == 2);
        CHECK(drawn[0].rect == QRect(0, 0, 16, 16));
        CHECK(drawn[1].pe == QStyle::PE_IndicatorCheckBox);
        CHECK(drawn[1].rect == QRect(40, 60, 16, 16));
        CHECK(drawn[1].device == QPoint(40, 60));
    }
}

TEST_CASE("ItemCheckStyle passes an application palette change to its base style once", "[layers][item-checks]") {
    int polished = 0;
    QTreeWidget tree;
    auto* style = new ItemCheckStyle(new PaletteCountingStyle(&polished));
    style->setParent(&tree);
    tree.setStyle(style);
    tree.show();
    QCoreApplication::processEvents();

    const QPalette saved = QApplication::palette();
    QPalette changed = saved;
    changed.setColor(QPalette::Base, saved.color(QPalette::Base) == Qt::black ? Qt::white : Qt::black);
    const int before = polished;
    QApplication::setPalette(changed);
    QCoreApplication::processEvents();
    CHECK(polished - before == 1);

    // and again on the way back, as after a light to dark switch and back
    QApplication::setPalette(saved);
    QCoreApplication::processEvents();
    CHECK(polished - before == 2);
}

TEST_CASE("ItemCheckStyle passes an application palette change to its base style for a view inside a window", "[layers][item-checks]") {
    int polished = 0;
    QWidget window;
    auto* tree = new QTreeWidget(&window);
    auto* style = new ItemCheckStyle(new PaletteCountingStyle(&polished));
    style->setParent(tree);
    tree->setStyle(style);
    window.show();
    QCoreApplication::processEvents();

    const QPalette saved = QApplication::palette();
    QPalette changed = saved;
    changed.setColor(QPalette::Base, saved.color(QPalette::Base) == Qt::black ? Qt::white : Qt::black);
    const int before = polished;
    QApplication::setPalette(changed);
    QCoreApplication::processEvents();
    CHECK(polished - before == 1);
    QApplication::setPalette(saved);
    QCoreApplication::processEvents();
    CHECK(polished - before == 2);

    SECTION("a palette set on the view alone is not passed on") {
        QPalette own = tree->palette();
        own.setColor(QPalette::Base, Qt::red);
        tree->setPalette(own);
        QCoreApplication::processEvents();
        CHECK(polished - before == 2);
    }
}

TEST_CASE("the real Layers dock passes an application palette change to its base style", "[layers][item-checks]") {
    const ForceItemChecks force;
    MainWindow w;
    w.show();
    QCoreApplication::processEvents();
    auto* style = dynamic_cast<ItemCheckStyle*>(w.layersWidget()->tree()->style());
    REQUIRE(style);
    // the tree is a child of the dock, so it gets PaletteChange, not
    // ApplicationPaletteChange; a counting base stands in for the second QMacStyle
    int polished = 0;
    style->setBaseStyle(new PaletteCountingStyle(&polished));

    const QPalette saved = QApplication::palette();
    QPalette changed = saved;
    changed.setColor(QPalette::Base, saved.color(QPalette::Base) == Qt::black ? Qt::white : Qt::black);
    QApplication::setPalette(changed);
    QCoreApplication::processEvents();
    CHECK(polished == 1);
    QApplication::setPalette(saved);
    QCoreApplication::processEvents();
    CHECK(polished == 2);
}

TEST_CASE("the Layers dock and the Layer Style dialog's effect list are given ItemCheckStyle", "[layers][item-checks]") {
    MainWindow w;
    QTreeWidget* tree = w.layersWidget()->tree();
    LayerStyleDialog d(w.document(), w.document()->activeLayer());
    if (QApplication::style()->name() == QLatin1String("macos")) {
        CHECK(dynamic_cast<ItemCheckStyle*>(tree->style()));
        CHECK(dynamic_cast<ItemCheckStyle*>(d.effectList()->style()));
    } else {
        CHECK(tree->style() == QApplication::style());
        CHECK(d.effectList()->style() == QApplication::style());
    }

    SECTION("with the macOS path forced, under any style") {
        const ForceItemChecks force;
        MainWindow forcedWindow;
        LayerStyleDialog forcedDialog(forcedWindow.document(), forcedWindow.document()->activeLayer());
        CHECK(dynamic_cast<ItemCheckStyle*>(forcedWindow.layersWidget()->tree()->style()));
        CHECK(dynamic_cast<ItemCheckStyle*>(forcedDialog.effectList()->style()));
    }
}

TEST_CASE("every row of DDOS KING's Layers dock has both boxes, and each toggles its own layer", "[layers][item-checks]") {
    MainWindow w;
    const fs::path king = fs::path(BW_SAMPLES_SOURCE) / "02-ddos-king";
    QString error;
    REQUIRE(w.openSampleFile(QFile::decodeName(QByteArray::fromStdString(king.string())), &error));
    Document* doc = w.document();
    QTreeWidget* tree = w.layersWidget()->tree();
    const int n = int(doc->layers().size());
    REQUIRE(n == 14);
    REQUIRE(tree->topLevelItemCount() == n);

    const auto layerAt = [&](int row) -> const Layer& { return doc->layers()[std::size_t(n - 1 - row)]; };
    for (int row = 0; row < n; ++row) {
        INFO("row " << row);
        QTreeWidgetItem* item = tree->topLevelItem(row);
        CHECK(item->flags().testFlag(Qt::ItemIsUserCheckable));
        CHECK(item->data(LayersDock::NameColumn, Qt::CheckStateRole).isValid());
        CHECK(item->data(LayersDock::AlphaLockColumn, Qt::CheckStateRole).isValid());
        CHECK(item->checkState(LayersDock::NameColumn) == (layerAt(row).props.visible ? Qt::Checked : Qt::Unchecked));
        CHECK(item->checkState(LayersDock::AlphaLockColumn) ==
              (layerAt(row).props.alphaLocked ? Qt::Checked : Qt::Unchecked));
    }

    // a click on a box past the first row changes that row's layer and no other
    for (const int row : {1, 5, n - 1}) {
        INFO("row " << row);
        std::vector<LayerProperties> before;
        for (int r = 0; r < n; ++r)
            before.push_back(layerAt(r).props);
        clickBox(tree, tree->model()->index(row, LayersDock::NameColumn));
        CHECK(layerAt(row).props.visible != before[std::size_t(row)].visible);
        clickBox(tree, tree->model()->index(row, LayersDock::AlphaLockColumn));
        CHECK(layerAt(row).props.alphaLocked != before[std::size_t(row)].alphaLocked);
        for (int r = 0; r < n; ++r) {
            if (r == row)
                continue;
            INFO("other row " << r);
            CHECK(layerAt(r).props.visible == before[std::size_t(r)].visible);
            CHECK(layerAt(r).props.alphaLocked == before[std::size_t(r)].alphaLocked);
        }
        CHECK(tree->topLevelItem(row)->checkState(LayersDock::NameColumn) ==
              (layerAt(row).props.visible ? Qt::Checked : Qt::Unchecked));
        CHECK(tree->topLevelItem(row)->checkState(LayersDock::AlphaLockColumn) ==
              (layerAt(row).props.alphaLocked ? Qt::Checked : Qt::Unchecked));
    }
}
