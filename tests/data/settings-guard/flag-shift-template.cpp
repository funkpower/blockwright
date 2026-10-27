std::variant<std::array<int, (1 << 2)>, QSettings> v;
void f() { v.emplace<1>(org, app); }
