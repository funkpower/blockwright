std::variant<std::conditional_t<((1 <= 2)), int, long>, QSettings> v;
void g(P* p) { p->reset(); }
