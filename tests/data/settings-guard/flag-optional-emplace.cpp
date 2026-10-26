std::optional<QSettings> s;
void f() { s.emplace(QStringLiteral("Blockwright"), QStringLiteral("Blockwright")); }
