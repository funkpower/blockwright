#include "reference_scale.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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
    std::vector<float> line(std::size_t(dw) * 4);  // source row `lineRow`, reduced across
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
            float c[4] = {};
            for (int x = std::max(int(a), x0); x < b && x < x1; ++x) {
                const QRgb px = in[x - x0];
                const float wgt = float(std::min<double>(b, x + 1) - std::max<double>(a, x));
                c[0] += wgt * float(qAlpha(px));
                c[1] += wgt * float(qRed(px));
                c[2] += wgt * float(qGreen(px));
                c[3] += wgt * float(qBlue(px));
            }
            for (int k = 0; k < 4; ++k)
                line[std::size_t(i) * 4 + k] += c[k];
        }
    };

    std::vector<float> acc(std::size_t(dw) * 4);
    const float norm = float(kx * ky);
    for (int j = 0; j < dh; ++j) {
        const double a = j / ky, b = std::min<double>((j + 1) / ky, sh);
        if (a >= b)
            break;
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int y = int(a); y < b && y < rowsRead; ++y) {
            if (y != lineRow) {
                std::fill(line.begin(), line.end(), 0.0f);
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
            const float wgt = float(std::min<double>(b, y + 1) - std::max<double>(a, y));
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

}  // namespace bw
