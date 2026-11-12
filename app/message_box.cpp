#include "message_box.h"

#include <QDialogButtonBox>
#include <QPushButton>

namespace bw {

namespace {

#ifdef Q_OS_MACOS
bool headings = true;
#else
bool headings = false;
#endif

// Set on a box whose text is its title.
constexpr char kHeadingProperty[] = "bwHeading";
// The text a heading box was given, which its informative text begins with.
constexpr char kTextProperty[] = "bwText";

QMessageBox::StandardButton show(QMessageBox::Icon icon, QWidget* parent, const QString& title, const QString& text,
                                 QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
    if (!headings) {
        switch (icon) {
        case QMessageBox::Question: return QMessageBox::question(parent, title, text, buttons, defaultButton);
        case QMessageBox::Information: return QMessageBox::information(parent, title, text, buttons, defaultButton);
        default: return QMessageBox::warning(parent, title, text, buttons, defaultButton);
        }
    }
    // The box Qt's static functions make: the buttons in order, the default
    // the one named or else the first that accepts.
    QMessageBox box(icon, QString(), QString(), QMessageBox::NoButton, parent);
    setBoxText(&box, title, text);
    auto* buttonBox = box.findChild<QDialogButtonBox*>();
    for (uint mask = QMessageBox::FirstButton; mask <= QMessageBox::LastButton; mask <<= 1) {
        const uint sb = buttons & mask;
        if (!sb)
            continue;
        QPushButton* button = box.addButton(QMessageBox::StandardButton(sb));
        if (box.defaultButton())
            continue;
        if ((defaultButton == QMessageBox::NoButton && buttonBox &&
             buttonBox->buttonRole(button) == QDialogButtonBox::AcceptRole) ||
            (defaultButton != QMessageBox::NoButton && sb == uint(defaultButton)))
            box.setDefaultButton(button);
    }
    if (box.exec() == -1)
        return QMessageBox::Cancel;
    return box.standardButton(box.clickedButton());
}

} // namespace

bool headingBoxes() { return headings; }
void useHeadingBoxes(bool on) { headings = on; }

void setBoxText(QMessageBox* box, const QString& title, const QString& text, const QString& informative) {
    box->setWindowTitle(title);
    box->setProperty(kHeadingProperty, headings);
    box->setProperty(kTextProperty, text);
    if (headings) {
        box->setText(title);
        box->setInformativeText(informative.isEmpty() ? text : text + QStringLiteral("\n\n") + informative);
    } else {
        box->setText(text);
        box->setInformativeText(informative);
    }
}

QString boxTitle(const QMessageBox* box) {
    return box->property(kHeadingProperty).toBool() ? box->text() : box->windowTitle();
}

QString boxText(const QMessageBox* box) {
    return box->property(kHeadingProperty).toBool() ? box->property(kTextProperty).toString() : box->text();
}

QMessageBox::StandardButton warning(QWidget* parent, const QString& title, const QString& text,
                                    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
    return show(QMessageBox::Warning, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text,
                                     QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
    return show(QMessageBox::Question, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton information(QWidget* parent, const QString& title, const QString& text,
                                        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton) {
    return show(QMessageBox::Information, parent, title, text, buttons, defaultButton);
}

} // namespace bw
