void f(QSettings& s, const QSettings* p, QSettings&& r);
QSettings* g;
std::unique_ptr<QSettings> appSettings();
