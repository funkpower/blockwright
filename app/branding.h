#pragma once
// The Blockwright logos and app icon (branding/, #205), from the Qt resources
// under :/branding. The logos are .ans files drawn by the app's own renderer;
// the icon is the PNGs branding/render makes from branding/icon*.ans.

#include "blockwright/ansi.h"

#include <QDialog>
#include <QIcon>
#include <QPixmap>
#include <QString>

#include <optional>

class QLabel;

namespace bw {

// The large logo (at most 80 x 20 cells, the start screen's) and the small
// one (at most 40 x 6, the About box's)
inline constexpr const char* kLogoLarge = ":/branding/logo-large.ans";
inline constexpr const char* kLogoSmall = ":/branding/logo-small.ans";

// The logo at `path` parsed, or nothing if it is missing or doesn't parse
std::optional<AnsiDocument> loadLogo(const QString& path);

// The logo drawn as renderDocument draws it, at the whole scale at or above
// `devicePixelRatio` and with that scale as its device pixel ratio, so it
// keeps its cell size in device-independent pixels; null if loadLogo fails
QPixmap logoPixmap(const QString& path, qreal devicePixelRatio = 1.0);

// The app icon: the PNGs at 16, 32, 48, 128, 256 and 512 px
QIcon applicationIcon();

// The desktop entry the app names as its own (QGuiApplication's desktop file
// name, the Wayland app id): in a Flatpak, where FLATPAK_ID is the app id
// its exported entry is named for (online.pumpin.funkpower.blockwright), that
// id; else the file name it runs as, so the dev install's blockwright-dev
// matches blockwright-dev.desktop
QString desktopFileName(const QString& argv0, const QString& flatpakId);

// Help > About Blockwright: the small logo over the version, the licence and
// the credits
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
    // The line under the version on the installed help bundle (#210), or
    // empty for none.
    void setHelpLine(const QString& line);

protected:
    // The logo is drawn again for a screen of another pixel ratio
    bool event(QEvent* e) override;

private:
    void drawLogo();
    QLabel* logo_ = nullptr;
    QLabel* help_ = nullptr;
};

// Shows the window's About dialog, made the first time, with `helpLine`
// under the version, and returns it
AboutDialog* showAbout(QWidget* window, const QString& helpLine = {});

} // namespace bw
