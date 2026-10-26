struct Store : std::array<int, sizeof(int)>, QSettings, std::pair<int, int> {};
