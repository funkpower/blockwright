#include "blockwright/brushes/distance.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bw {

namespace {

constexpr double kFar = 1e18;

// The work edgeDistance's `steps` asks for; without the test hooks it holds
// nothing and adding to it compiles away.
struct Steps {
#ifdef BW_TEST_HOOKS
    long long n = 0;
    void operator+=(long long k) { n += k; }
#else
    void operator+=(long long) {}
#endif
};

// One pass of the lower envelope: out[q] = min over p of f[p] + a·(q − p)²,
// f[p] = kFar where there is no site. Linear in n: adds its steps, n
// for each loop, a parabola dropped at most once each and the next one
// moved to at most n − 1 times, so under 4n.
void envelope(const std::vector<double>& f, std::vector<double>& out, double a, std::vector<int>& v,
              std::vector<double>& z, Steps& steps) {
    const int n = int(f.size());
    steps += 2LL * n;
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[q] >= kFar)
            continue;
        double s = -std::numeric_limits<double>::infinity();
        while (k >= 0) {
            const int p = v[k];
            s = ((f[q] + a * q * q) - (f[p] + a * p * p)) / (2 * a * (q - p));
            if (s > z[k])
                break;
            --k;
            steps += 1;
        }
        ++k;
        v[k] = q;
        z[k] = k == 0 ? -std::numeric_limits<double>::infinity() : s;
    }
    for (int q = 0, j = 0; q < n; ++q) {
        if (k < 0) {
            out[q] = kFar;
            continue;
        }
        while (j < k && z[j + 1] < q)
            ++j, steps += 1;
        out[q] = f[v[j]] + a * (q - v[j]) * (q - v[j]);
    }
}

// edgeDistance, its work added to `work`.
std::vector<double> distance(const Mask& shape, Steps& work) {
    const int w = shape.width(), h = shape.height();
    std::vector<double> out(size_t(w) * h, 0.0);
    work += static_cast<long long>(w) * h;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (shape.contains(x, y)) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return out;
    // The bounding box with a ring of cells off the shape round it: every
    // shape cell's nearest off cell is in the box or on the ring.
    const int bw = x1 - x0 + 3, bh = y1 - y0 + 3;
    std::vector<double> sq(size_t(bw) * bh);
    work += static_cast<long long>(bw) * bh;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            sq[size_t(y) * bw + x] = shape.contains(x0 - 1 + x, y0 - 1 + y) ? kFar : 0.0;
    const int n = std::max(bw, bh);
    std::vector<double> f, g;
    std::vector<int> v(n);
    std::vector<double> z(n + 1);
    // Down each column, a row counting two columns (squared, four), then
    // along each row.
    f.resize(bh);
    g.resize(bh);
    for (int x = 0; x < bw; ++x) {
        for (int y = 0; y < bh; ++y)
            f[y] = sq[size_t(y) * bw + x];
        envelope(f, g, 4.0, v, z, work);
        for (int y = 0; y < bh; ++y)
            sq[size_t(y) * bw + x] = g[y];
    }
    f.resize(bw);
    g.resize(bw);
    for (int y = 0; y < bh; ++y) {
        std::copy_n(sq.begin() + ptrdiff_t(y) * bw, bw, f.begin());
        envelope(f, g, 1.0, v, z, work);
        for (int x = 0; x < bw; ++x) {
            const int cx = x0 - 1 + x, cy = y0 - 1 + y;
            if (shape.contains(cx, cy))
                out[size_t(cy) * w + cx] = std::sqrt(g[x]);
        }
    }
    return out;
}

} // namespace

#ifdef BW_TEST_HOOKS
std::vector<double> edgeDistance(const Mask& shape, long long* steps) {
    Steps work;
    auto out = distance(shape, work);
    if (steps)
        *steps += work.n;
    return out;
}
#else
std::vector<double> edgeDistance(const Mask& shape) {
    Steps work;
    return distance(shape, work);
}
#endif

} // namespace bw
