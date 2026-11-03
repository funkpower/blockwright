setText(
#ifdef Q_OS_MACOS
    tr("%n cells merged", nullptr, n)
#else
    QString()
#endif
);
