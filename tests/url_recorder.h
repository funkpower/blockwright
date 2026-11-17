#pragma once
// Takes the place of the system's browser for a scheme while it lives:
// every URL QDesktopServices::openUrl is asked to open with it is kept.

#include <QDesktopServices>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class UrlRecorder : public QObject {
    Q_OBJECT
public:
    explicit UrlRecorder(const QStringList& schemes = {QStringLiteral("https"), QStringLiteral("http"),
                                                       QStringLiteral("file")})
        : schemes_(schemes) {
        for (const QString& s : schemes_)
            QDesktopServices::setUrlHandler(s, this, "open");
    }
    ~UrlRecorder() override {
        for (const QString& s : schemes_)
            QDesktopServices::unsetUrlHandler(s);
    }
    QList<QUrl> urls;

public slots:
    void open(const QUrl& url) { urls << url; }

private:
    QStringList schemes_;
};
