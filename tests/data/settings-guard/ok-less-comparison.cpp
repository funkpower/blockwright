void compare(decltype(1 < 2), QSettings);
void f(int n, QSettings* settings) { call((n < 2), settings); }
std::variant<std::conditional_t<((1 <= 2)), int, long>, QSettings*> stores;
#define MAKE(e, T) T made
MAKE((1 < 2), QSettings*);
