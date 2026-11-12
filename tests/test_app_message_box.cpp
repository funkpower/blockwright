// app/message_box.h: a box's title is shown on every platform, as its
// heading where boxes have no window title (macOS) and as the window title
// elsewhere. main() is in test_app.cpp.

#include "message_box.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>

#include <functional>

using namespace bw;

namespace {

// Runs `answer` on the first message box that comes up.
struct BoxAnswer {
    QTimer timer;
    bool seen = false;
    BoxAnswer(std::function<void(QMessageBox*)> answer) {
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, [this, answer] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box || !box->isVisible() || seen)
                return;
            seen = true;
            answer(box);
        });
        timer.start();
    }
};

// The title a box shows: on macOS QMessageBox sets no window title, so a box
// that doesn't make its title its heading has none there.
QString shownTitle(bool headings, const QString& title) {
#ifdef Q_OS_MACOS
    return headings ? title : QString();
#else
    Q_UNUSED(headings);
    return title;
#endif
}

} // namespace

TEST_CASE("message boxes put their title in their text on macOS only", "[message-box]") {
    if (qEnvironmentVariableIntValue("BW_TEST_HEADING_BOXES")) {
        CHECK(headingBoxes());
        return;
    }
#ifdef Q_OS_MACOS
    CHECK(headingBoxes());
#else
    CHECK_FALSE(headingBoxes());
#endif
}

TEST_CASE("a box's title is its heading where boxes have no title, and its window title elsewhere",
          "[message-box]") {
    const bool headings = GENERATE(false, true);
    const bool platform = headingBoxes();
    useHeadingBoxes(headings);
    const auto restore = qScopeGuard([&] { useHeadingBoxes(platform); });
    const QString title = QStringLiteral("Open Scene");
    const QString text = QStringLiteral("A generator is running; open the scene when it ends.");

    SECTION("a warning") {
        QString shownText, informative, gotTitle, gotText;
        BoxAnswer answer([&](QMessageBox* box) {
            shownText = box->text();
            informative = box->informativeText();
            gotTitle = boxTitle(box);
            gotText = boxText(box);
            CHECK(box->icon() == QMessageBox::Warning);
            box->button(QMessageBox::Ok)->click();
        });
        CHECK(warning(nullptr, title, text) == QMessageBox::Ok);
        REQUIRE(answer.seen);
        CHECK(gotTitle == shownTitle(headings, title));
        CHECK(gotText == text);
        if (headings) {
            CHECK(shownText == title);
            CHECK(informative == text);
        } else {
            CHECK(shownText == text);
            CHECK(informative.isEmpty());
        }
    }

    SECTION("a question's buttons and default are Qt's") {
        QAbstractButton* defaultButton = nullptr;
        QList<QMessageBox::StandardButton> buttons;
        BoxAnswer answer([&](QMessageBox* box) {
            defaultButton = box->defaultButton();
            for (QAbstractButton* b : box->buttons())
                buttons << box->standardButton(b);
            CHECK(box->icon() == QMessageBox::Question);
            CHECK(boxTitle(box) == shownTitle(headings, title));
            CHECK(boxText(box) == text);
            CHECK(defaultButton == box->button(QMessageBox::No));
            box->button(QMessageBox::Yes)->click();
        });
        CHECK(question(nullptr, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
              QMessageBox::Yes);
        REQUIRE(answer.seen);
        CHECK(buttons.size() == 2);
        CHECK(buttons.contains(QMessageBox::Yes));
        CHECK(buttons.contains(QMessageBox::No));
    }

    SECTION("with no default named, the first button that accepts is it") {
        BoxAnswer answer([&](QMessageBox* box) {
            CHECK(box->icon() == QMessageBox::Information);
            CHECK(box->defaultButton() == box->button(QMessageBox::Save));
            box->button(QMessageBox::Cancel)->click();
        });
        CHECK(information(nullptr, title, text, QMessageBox::Save | QMessageBox::Cancel) == QMessageBox::Cancel);
        REQUIRE(answer.seen);
    }

    SECTION("a box made by hand") {
        QMessageBox box(QMessageBox::Warning, QString(), QString(), QMessageBox::Ok);
        setBoxText(&box, title, text);
        CHECK(boxTitle(&box) == shownTitle(headings, title));
        CHECK(boxText(&box) == text);
        CHECK(box.text() == (headings ? title : text));
        CHECK(box.informativeText() == (headings ? text : QString()));
    }

    SECTION("a box made by hand with informative text") {
        const QString informative = QStringLiteral("This one runs: python3 bars.py");
        QMessageBox box(QMessageBox::Warning, QString(), QString(), QMessageBox::Yes | QMessageBox::No);
        setBoxText(&box, title, text, informative);
        CHECK(boxTitle(&box) == shownTitle(headings, title));
        CHECK(boxText(&box) == text);
        CHECK(box.text() == (headings ? title : text));
        CHECK(box.informativeText() == (headings ? text + QStringLiteral("\n\n") + informative : informative));
    }
}

TEST_CASE("a box the helper didn't make reads as Qt shows it", "[message-box]") {
    const bool platform = headingBoxes();
    useHeadingBoxes(true);
    const auto restore = qScopeGuard([&] { useHeadingBoxes(platform); });
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Blockwright"), QStringLiteral("Save the changes?"),
                    QMessageBox::Ok);
    box.setInformativeText(QStringLiteral("They are lost otherwise."));
    CHECK(boxText(&box) == QStringLiteral("Save the changes?"));
}
