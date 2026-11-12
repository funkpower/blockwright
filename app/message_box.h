#pragma once
// Message boxes whose title says what they are about ("Open Scene" over
// "Could not open %1"). On macOS a QMessageBox has no window title
// (QMessageBox::setWindowTitle sets none, as Apple's guidelines have it), so
// there the title is the box's text, its heading, and the text its
// informative text; elsewhere they are the window title and the text, as
// Qt's own static functions make them.

#include <QMessageBox>
#include <QString>

namespace bw {

// Whether boxes put their title in their text (headingBoxes(): on macOS),
// and a test's switch for it.
bool headingBoxes();
void useHeadingBoxes(bool on);

// Gives `box` the title and text so the title is shown on every platform.
// `informative`, when given, is the box's informative text; where the title
// is the heading it follows the text there, after a blank line.
void setBoxText(QMessageBox* box, const QString& title, const QString& text,
                const QString& informative = QString());
// The title and text a box was given, whichever way it shows them.
QString boxTitle(const QMessageBox* box);
QString boxText(const QMessageBox* box);

// QMessageBox's static functions, with the title shown on every platform.
QMessageBox::StandardButton warning(QWidget* parent, const QString& title, const QString& text,
                                    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton question(QWidget* parent, const QString& title, const QString& text,
                                     QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton information(QWidget* parent, const QString& title, const QString& text,
                                        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

} // namespace bw
