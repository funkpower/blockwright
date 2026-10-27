std::variant<std::conditional_t<(1 < 2), int, long>, QSettings> v;
struct W { void go(P* p) { p->reset(); } };
