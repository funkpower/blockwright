std::variant<std::array<int, (1 << 2)>, QSettings> v;
void g(P* p) { p->reset(); p->reset(); }
