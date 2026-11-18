// bw-png-same <a> <b>: exit 0 when two image files hold the same pixels, 1
// when they differ, 2 when either can't be read. A PNG's bytes depend on the
// zlib that compressed it (zlib and zlib-ng write different streams for the
// same pixels), so a render is checked against a golden PNG by its pixels.
#include <QImage>
#include <QString>

#include <cstdio>
#include <utility>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: bw-png-same <a> <b>\n");
        return 2;
    }
    const QImage a(QString::fromLocal8Bit(argv[1]));
    const QImage b(QString::fromLocal8Bit(argv[2]));
    for (const auto& [img, path] : {std::pair{&a, argv[1]}, std::pair{&b, argv[2]}}) {
        if (img->isNull()) {
            std::fprintf(stderr, "bw-png-same: %s: not an image\n", path);
            return 2;
        }
    }
    if (a.convertToFormat(QImage::Format_ARGB32) == b.convertToFormat(QImage::Format_ARGB32)) return 0;
    std::fprintf(stderr, "bw-png-same: %s and %s differ\n", argv[1], argv[2]);
    return 1;
}
