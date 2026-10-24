#include "lint.h"

#include "read_file.h"

#include <cstdio>

namespace bwcli {

namespace {

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

// A JSON string: quotes, backslashes and control characters escaped.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

} // namespace

bw::lint::LintReport lintDocument(const bw::AnsiDocument& doc, const LintCliOptions& o) {
    const bw::Canvas& c = doc.canvas;
    // as shade refuses a box off the canvas: a box past its edges is clipped
    if (o.box && (o.box->x0 >= c.width() || o.box->y0 >= c.height()))
        throw Failure{"the box " + std::to_string(o.box->x0) + "," + std::to_string(o.box->y0) + "," +
                      std::to_string(o.box->x1) + "," + std::to_string(o.box->y1) + " is off the " +
                      std::to_string(c.width()) + "x" + std::to_string(c.height()) + " canvas"};
    bw::lint::LintOptions lo;
    lo.ramps = o.ramps.empty() ? bw::namedRamps() : o.ramps;
    lo.craftPairs = o.ramps.empty();
    lo.box = o.box;
    lo.ice = doc.ice;
    return bw::lint::lintCanvas(doc.canvas, lo);
}

std::string lintText(const bw::lint::LintReport& r) {
    std::string out;
    for (const bw::lint::Finding& f : r.findings)
        out += std::to_string(f.x) + "," + std::to_string(f.y) + " " + std::string(bw::lint::kindName(f.kind)) + " " +
               (f.ramp.empty() ? "-" : f.ramp) + " " + f.message + "\n";
    out += bw::lint::summary(r) + "\n";
    return out;
}

std::string lintJson(const bw::lint::LintReport& r) {
    std::string out = "{\"findings\":[";
    for (std::size_t i = 0; i < r.findings.size(); ++i) {
        const bw::lint::Finding& f = r.findings[i];
        out += std::string(i ? "," : "") + "{\"kind\":" + quoted(std::string(bw::lint::kindName(f.kind))) +
               ",\"ramp\":" + (f.ramp.empty() ? std::string("null") : quoted(f.ramp)) +
               ",\"x\":" + std::to_string(f.x) + ",\"y\":" + std::to_string(f.y) + ",\"x2\":" + std::to_string(f.x2) +
               ",\"y2\":" + std::to_string(f.y2) + ",\"message\":" + quoted(f.message) + "}";
    }
    out += "],\"noise\":" + number(r.noise) + ",\"ice\":" + (r.ice ? "true" : "false") + "}\n";
    return out;
}

} // namespace bwcli
