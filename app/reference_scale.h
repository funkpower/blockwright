#pragma once
// Reducing the reference image (PLAN §4): an area average, read a few source
// pixels at a time so a large or odd-format picture is never converted whole.

#include "blockwright/layer.h"

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

#include <vector>

namespace bw {

// The most source pixels areaScale converts at once.
inline constexpr int kMaxConvertPixels = 1 << 20;

// Full decoding is bounded by checked raster headers (PNG, JPEG, BMP),
// or Qt's allocation limit for the handlers referenceDecode allows.
inline constexpr qint64 kMaxReferenceDecodePixels = ReferenceImage::kMaxDecodePixels;

// How a full decode of a picture Qt reads as `format` (QImageReader::format()
// of its bytes, not a file suffix) is bounded.
enum class ReferenceDecode {
    Header,  // png, jpeg, bmp: the header's size is the decoded raster
    Capped,  // handlers checked to fail an image over Qt's allocation limit:
             // gif, webp, tiff, ico, pbm, pgm, ppm, xbm, xpm
    Refused, // anything else: pdf, which decodes past the limit; svg and
             // svgz, which decode embedded images of any format; icns, which
             // reports no size; or a plugin installed later
};
ReferenceDecode referenceDecode(const QByteArray& format);

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

// The size `w` x `h` (each at least 1) is scaled to so it is at most
// `budget` px (at least 1): the same aspect where both sides still round to
// 1 or more, and where one is held at 1, the other cut to the budget. Never
// larger than w x h.
QSize fitPixels(double w, double h, qint64 budget);

// Returns a reference picture of at most `limit` px (PLAN §4). Uses native
// scaled decoding where supported (JPEG); otherwise refuses headers beyond
// kMaxReferenceDecodePixels before decoding in full and shrinking. Caps Qt's
// allocation limit at 256 MiB (at least 4 bytes/px), preserving a stricter
// limit, before detecting the format (Qt's XBM detector decodes the whole
// picture) and while decoding a Capped format; if the cap cannot be enforced,
// refuses a Capped format, and bytes the XBM detector might decode before
// detection. Reads are serialized and restore the previous global limit.
// Refuses a Refused format and unreadable dimensions too. `original` gets the full size after rotation;
// `error` explains a refusal (the returned image is then null).
QImage readReferencePicture(const QByteArray& bytes, qint64 limit, QSize* original = nullptr,
                            QString* error = nullptr);

}  // namespace bw
