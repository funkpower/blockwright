#pragma once
// The start screen (#206, PLAN §11): shown before the main window on a
// launch with no file or project named, and over it from File > Start
// Screen.... The
// large logo, drawn by the renderer on black, with the version under it; then
// the actions, each a button in one column with its key (New, Open...,
// Open Project Directory...), and the sections beside them, side by side
// (Recent, and Samples when any came with the app). Every
// action goes through the window's own path. Anything that opens or makes a
// document while the screen is up closes it, whatever started it (its
// buttons, or a file the system hands the app); one cancelled or refused
// leaves it up. Esc or closing it gives a new default canvas, as starting did
// before it. Enter runs the focused button, or New from anywhere else.

#include <QDialog>
#include <QImage>
#include <QSize>
#include <QString>

#include <functional>

class QBoxLayout;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace bw {

class MainWindow;

// The logo the screen draws (#205), a Qt resource of at most 80 x 20 cells.
inline constexpr const char* kStartLogoPath = ":/branding/logo-large.ans";
// The size New offers until it is changed.
inline constexpr int kStartNewWidth = 80;
inline constexpr int kStartNewHeight = 40;

// Whether the screen shows at startup ("Show at startup", kept through
// appSettings() as startScreen/showAtStartup; on until turned off).
bool startScreenAtStartup();
void setStartScreenAtStartup(bool on);

// The .ans at `path` (a file or a Qt resource) rendered at 1x by the app's
// renderer, or a null image when it is missing, doesn't parse or is empty.
QImage renderStartLogo(const QString& path);
// The logo's scale: 2 when the screen at 2x (the logo `logo` pixels at 1x,
// beside `rest`, the rest of the screen, which sits under it) fits
// `available`, less 60 pixels of height for the title bar, borders and a
// desktop panel, as the main window reserves (#183); else 1.
int startLogoScale(QSize logo, QSize rest, QSize available);

class StartScreen : public QDialog {
    Q_OBJECT
public:
    struct Options {
        QString logoPath = QString::fromLatin1(kStartLogoPath);
        // The screen area the logo's scale is chosen for; empty for the
        // available geometry of the window's screen.
        QSize available;
        // At launch, before the window shows: the screen is a window of its
        // own, and the main window shows only once it closes.
        bool launch = false;
    };
    explicit StartScreen(MainWindow* window, const Options& options);
    explicit StartScreen(MainWindow* window) : StartScreen(window, Options{}) {}

    // 1 or 2, or 0 when the wordmark stands in for a missing logo.
    int logoScale() const { return logoScale_; }
    // Lists the recent files again (each, a missing one disabled with a
    // Remove button beside it).
    void refreshRecent();

    // The actions, as the buttons and keys run them.
    void newCanvas();
    void openFile();
    void openDirectory();
    void openRecent(const QString& path);
    void openSample(const QString& path);

protected:
    void done(int result) override;

private:
    // One more action in the actions column: a button labelled `text`, run
    // by it and by `keys`.
    QPushButton* addAction(const QString& name, const QString& text, const QList<QKeySequence>& keys,
                           const std::function<void()>& run);
    // One more section beside the actions and the sections before it, under
    // `title`, taking `stretch` of the spare width.
    void addSection(const QString& title, QWidget* body, int stretch = 1);
    MainWindow* window_;
    // In done(), whose own clear() resets the document.
    bool closing_ = false;
    bool launch_ = false;
    int logoScale_ = 0;
    QVBoxLayout* actions_ = nullptr;
    QBoxLayout* sections_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QCheckBox* ice_ = nullptr;
    QVBoxLayout* recent_ = nullptr;
    QWidget* recentBox_ = nullptr;
};

// What main() does at launch: opens `file` as a file named on the command
// line, or, with none, shows the start screen unless it is turned off. The
// window shows first unless the screen does; then the screen comes up alone
// and the window shows when it closes. The screen shown, or null.
StartScreen* startUp(MainWindow* window, const QString& file);

} // namespace bw
