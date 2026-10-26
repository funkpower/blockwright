std::map<QString, QSettings> stores;
void f1() { stores.try_emplace(QStringLiteral("k"), QStringLiteral("Blockwright"), QStringLiteral("Blockwright")); }
