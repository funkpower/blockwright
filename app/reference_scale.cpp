#include "reference_scale.h"

#include <QBuffer>
#include <QImageReader>
#include <QScopeGuard>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <new>
#include <mutex>

namespace bw {

std::vector<QRect> referenceChunks(int width, int rows) {
    std::vector<QRect> out;
    if (width <= 0 || rows <= 0)
        return out;
    if (width <= kMaxConvertPixels) {
        const int n = kMaxConvertPixels / width;
        for (int y = 0; y < rows; y += n)
            out.emplace_back(0, y, width, std::min(n, rows - y));
        return out;
    }
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < width; x += kMaxConvertPixels)
            out.emplace_back(x, y, std::min(kMaxConvertPixels, width - x), 1);
    return out;
}

bool areaScale(const QImage& src, QImage& dst, double kx, double ky) {
    const int sw = src.width(), sh = src.height(), dw = dst.width(), dh = dst.height();
    const int rowsRead = int(std::min<double>(sh, std::ceil(dh / ky)));
    const std::vector<QRect> chunks = referenceChunks(sw, rowsRead);
    std::size_t next = 0;  // the first chunk not yet converted
    QImage rows;  // the last chunk converted, as ARGB32 premultiplied
    QRect rowsAt;  // where it is in the source
    std::vector<double> line(std::size_t(dw) * 4);  // source row `lineRow`, reduced across
    int lineRow = -1;

    // Adds row y of `rows` to `line`: each dst column gets the source pixels
    // it covers there, by how much of each it covers.
    const auto addRow = [&](int y) {
        const auto* in = reinterpret_cast<const QRgb*>(rows.constScanLine(y - rowsAt.top()));
        const int x0 = rowsAt.left(), x1 = x0 + rowsAt.width();
        for (int i = std::max(0, int(x0 * kx) - 1); i < dw; ++i) {
            const double a = i / kx, b = std::min<double>((i + 1) / kx, sw);
            if (a >= x1)
                break;
            double c[4] = {};
            for (int x = std::max(int(a), x0); x < b && x < x1; ++x) {
                const QRgb px = in[x - x0];
                const double wgt = std::min<double>(b, x + 1) - std::max<double>(a, x);
                c[0] += wgt * qAlpha(px);
                c[1] += wgt * qRed(px);
                c[2] += wgt * qGreen(px);
                c[3] += wgt * qBlue(px);
            }
            for (int k = 0; k < 4; ++k)
                line[std::size_t(i) * 4 + k] += c[k];
        }
    };

    std::vector<double> acc(std::size_t(dw) * 4);
    const double norm = kx * ky;
    for (int j = 0; j < dh; ++j) {
        const double a = j / ky, b = std::min<double>((j + 1) / ky, sh);
        if (a >= b)
            break;
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int y = int(a); y < b && y < rowsRead; ++y) {
            if (y != lineRow) {
                std::fill(line.begin(), line.end(), 0.0);
                if (!rows.isNull() && rowsAt.width() == sw && y <= rowsAt.bottom()) {
                    addRow(y);
                } else {
                    while (next < chunks.size() && chunks[next].top() <= y) {
                        rowsAt = chunks[next++];
                        rows = src.copy(rowsAt).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                        if (rows.isNull())
                            return false;
                        if (rowsAt.bottom() >= y)
                            addRow(y);
                    }
                }
                lineRow = y;
            }
            const double wgt = std::min<double>(b, y + 1) - std::max<double>(a, y);
            for (std::size_t k = 0; k < acc.size(); ++k)
                acc[k] += wgt * line[k];
        }
        auto* out = reinterpret_cast<QRgb*>(dst.scanLine(j));
        for (int i = 0; i < dw; ++i) {
            const auto level = [&](int k, int most) {
                return std::clamp(int(std::lround(acc[std::size_t(i) * 4 + k] * norm)), 0, most);
            };
            const int alpha = level(0, 255);
            out[i] = qRgba(level(1, alpha), level(2, alpha), level(3, alpha), alpha);
        }
    }
    return true;
}

QSize fitPixels(double w, double h, qint64 budget) {
    budget = std::max<qint64>(budget, 1);
    const double f = std::min(1.0, std::sqrt(double(budget) / (w * h)));
    qint64 cw = std::max<qint64>(1, qint64(std::min(w * f, double(INT_MAX))));
    qint64 ch = std::max<qint64>(1, qint64(std::min(h * f, double(INT_MAX))));
    // a side held at 1 can carry the product past the budget: cut the other
    if (cw * ch > budget) {
        if (cw >= ch)
            cw = std::max<qint64>(1, budget / ch);
        else
            ch = std::max<qint64>(1, budget / cw);
    }
    return QSize(int(cw), int(ch));
}

ReferenceDecode referenceDecode(const QByteArray& format) {
    if (format == "png" || format == "jpeg" || format == "bmp")
        return ReferenceDecode::Header;
    // Each of these was probed on Qt 6.12: a 1024 x 1024 picture fails to
    // decode under a 1 MiB allocation limit. Reading the limit back proves
    // nothing about a handler (the PDF plugin ignores it), so a format joins
    // this list only once its handler is checked the same way. SVG and SVGZ
    // stay off it: an <image> data URL is decoded through QImage::fromData in
    // whatever format its bytes are, PDF included, past the outer check.
    // ICNS stays off too: its handler reports no size to check.
    static const QByteArray capped[] = {"gif", "webp", "tiff", "ico", "pbm",
                                        "pgm", "ppm", "xbm", "xpm"};
    for (const QByteArray& f : capped)
        if (format == f)
            return ReferenceDecode::Capped;
    return ReferenceDecode::Refused;
}

// Whether Qt's XBM handler might recognise `bytes`. Its detector decodes
// the whole picture to answer, before anyone can check a size: it looks for
// a line starting "#define" in the first 4 KiB. No other detector in Qt 6.12
// decodes; they read a header or magic bytes.
static bool mayBeXbm(const QByteArray& bytes) {
    return bytes.left(4096).contains("#define");
}

QImage readReferencePicture(const QByteArray& bytes, qint64 limit, QSize* original, QString* error) {
    // The allocation limit is process-wide. Serialize every reference read,
    // including header-bounded formats, so no read overlaps another's change
    // or restoration. Other app image reads run on the GUI thread; background
    // jobs do not decode images or change this setting.
    static std::mutex decodeMutex;
    const std::lock_guard lock(decodeMutex);
    // Detecting the format runs every handler's detector on the bytes, so
    // the cap goes on before it and comes off on every exit. Qt accounts for
    // at least four bytes per pixel, even for grayscale and indexed images.
    // This cap therefore enforces the pixel ceiling on detectors and embedded
    // decoders too, and never raises a stricter caller limit.
    constexpr int maxMiB = int(kMaxReferenceDecodePixels * 4 / (1 << 20));
    const int previous = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([previous] { QImageReader::setAllocationLimit(previous); });
    QImageReader::setAllocationLimit(previous > 0 ? std::min(previous, maxMiB) : maxMiB);
    const int effective = QImageReader::allocationLimit();
    // QT_IMAGEIO_MAXALLOC overrides setAllocationLimit, even with zero
    // (disabled). Refuse rather than decode under an unenforced bound.
    const bool enforced = effective > 0 && effective <= maxMiB;
    const auto unenforced = [error] {
        if (error)
            *error = QStringLiteral("The picture cannot be decoded safely: Qt's allocation limit "
                                    "could not be bounded to %1 MiB (QT_IMAGEIO_MAXALLOC).")
                         .arg(maxMiB);
        return QImage();
    };
    if (!enforced && mayBeXbm(bytes))
        return unenforced();
    QByteArray data = bytes;
    QBuffer buffer(&data);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QByteArray format = reader.format();
    // Single-image headers (Header) describe the actual decoded raster. Other
    // handlers, containers among them, can decode more than size() advertises,
    // so only those known to honour the allocation limit are read at all.
    const ReferenceDecode decode = referenceDecode(format);
    if (decode == ReferenceDecode::Refused) {
        if (error)
            *error = format.isEmpty()
                         ? QStringLiteral("The picture's format is not recognised.")
                         : QStringLiteral("%1 pictures cannot be decoded safely as a reference image.")
                               .arg(QString::fromLatin1(format.toUpper()));
        return {};
    }
    if (decode == ReferenceDecode::Capped && !enforced)
        return unenforced();
    // A header-bounded picture decodes under the caller's own limit, so a
    // deeper raster (16-bit PNG) up to the pixel ceiling still fits.
    if (decode == ReferenceDecode::Header)
        QImageReader::setAllocationLimit(previous);
    const QSize size = reader.size();
    if (size.isEmpty()) {
        if (error)
            *error = QStringLiteral("The picture's dimensions could not be read.");
        return {};
    }
    const bool scaledDecode = reader.supportsOption(QImageIOHandler::ScaledSize);
    if (!scaledDecode && qint64(size.width()) * size.height() > kMaxReferenceDecodePixels) {
        if (error)
            *error = QStringLiteral("The picture is too large to decode: %1 × %2 pixels (limit %3).")
                         .arg(size.width()).arg(size.height()).arg(kMaxReferenceDecodePixels);
        return {};
    }
    if (scaledDecode && qint64(size.width()) * size.height() > limit)
        reader.setScaledSize(fitPixels(size.width(), size.height(), limit));
    QImage image;
    try {
        image = reader.read();
        // The header or allocation guard bounds full decoding. Bound the
        // retained copy after any orientation.
        if (!image.isNull() && qint64(image.width()) * image.height() > limit)
            image = image.scaled(fitPixels(image.width(), image.height(), limit),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } catch (const std::bad_alloc&) {
        image = {};
    }
    if (image.isNull() && error)
        *error = reader.errorString();
    if (original) {
        *original = size.isValid() ? size : image.size();
        if (size.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90))
            original->transpose();
    }
    return image;
}

}  // namespace bw
