std::variant<std::monostate, QSettings> v;
void f() { v.emplace<1>(org, app); }
