struct P { int x; };
P* pp = nullptr;
std::variant<decltype(pp->x), QSettings> v;
void f() { v.emplace<1>(org, app); }
