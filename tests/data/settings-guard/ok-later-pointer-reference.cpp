std::map<QString, const ::QSettings*> stores;
std::tuple<std::pair<int, int>, QSettings const&> refs;
#define DECLARE(n, T) T n
DECLARE(s, volatile QSettings*);
DECLARE(r, const ::QSettings&);
