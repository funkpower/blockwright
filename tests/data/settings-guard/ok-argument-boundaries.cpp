void f() { call(1, QSettings::IniFormat); }
void g(int n, QSettings* s);
class Window { void save(int n, QSettings& s); };
using Format = QSettings::Format;
