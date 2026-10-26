std::variant<std::conditional_t<(sizeof(int) > 2), int, long>, QSettings> v;
void f() { v.emplace<1>(org, app); }
