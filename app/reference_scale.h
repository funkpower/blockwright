#pragma once
// Reducing the reference image (PLAN §4): an area average, read a few source
// pixels at a time so a large or odd-format picture is never converted whole.

#include <QImage>
#include <QRect>

#include <vector>

namespace bw {

// The most source pixels areaScale converts at once.
inline constexpr int kMaxConvertPixels = 1 << 20;

// The source rects areaScale converts, in order, to read the first `rows`
// rows of a picture `width` px wide: whole rows, as many as fit in
// kMaxConvertPixels, or a row wider than that in pieces. They cover those
// rows once each and none is over kMaxConvertPixels.
std::vector<QRect> referenceChunks(int width, int rows);

// Area-averages `src` onto `dst` (ARGB32 premultiplied, transparent), `kx`
// and `ky` dst pixels to a source pixel: dst pixel (i, j) is the mean of the
// source over [i/kx, (i+1)/kx) x [j/ky, (j+1)/ky). Only the picture's own
// pixels count, so a pixel it covers in part is that part opaque. Only the
// source rows under `dst` are read, in referenceChunks. False when a
// conversion fails.
bool areaScale(const QImage& src, QImage& dst, double kx, double ky);

}  // namespace bw
