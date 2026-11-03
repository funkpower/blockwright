#include "blockwright/samples.h"

#include "blockwright/project.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>

#ifndef BW_SAMPLES_FROM_BINDIR
#define BW_SAMPLES_FROM_BINDIR "../share/blockwright/samples"
#endif

namespace fs = std::filesystem;

namespace bw::samples {

namespace {

std::string trim(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && ws(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && ws(s.back()))
        s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        out.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

// The cells of a table row "| a | b |", trimmed; a '|' escaped as "\|" is
// part of its cell.
std::vector<std::string> cells(std::string_view row) {
    std::string r = trim(row);
    if (!r.empty() && r.front() == '|')
        r.erase(0, 1);
    if (r.size() >= 2 && r.back() == '|' && r[r.size() - 2] != '\\')
        r.pop_back();
    else if (r.size() == 1 && r.back() == '|')
        r.pop_back();
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i] == '\\' && i + 1 < r.size() && r[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (r[i] == '|') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += r[i];
        }
    }
    out.push_back(trim(cur));
    return out;
}

bool isRule(const std::vector<std::string>& row) {
    return std::all_of(row.begin(), row.end(), [](const std::string& c) {
        return !c.empty() && c.find_first_not_of("-: ") == std::string::npos;
    });
}

// The rows of the table under the "## <heading>" section, header and rule
// rows skipped; each padded to the header's width. A header with fewer than
// `columns` cells is refused: the rows under it couldn't be read.
std::vector<std::vector<std::string>> table(const std::vector<std::string_view>& ls, std::string_view heading,
                                            std::size_t columns) {
    std::vector<std::vector<std::string>> rows;
    std::size_t i = 0;
    for (; i < ls.size(); ++i) {
        const std::string l = trim(ls[i]);
        if (l.rfind("## ", 0) == 0 && trim(std::string_view(l).substr(3)) == heading)
            break;
    }
    if (i == ls.size())
        return rows;
    std::size_t width = 0;
    bool started = false;
    for (++i; i < ls.size(); ++i) {
        const std::string l = trim(ls[i]);
        if (l.rfind("#", 0) == 0)
            break;
        if (l.empty() || l.front() != '|') {
            if (started)
                break;
            continue;
        }
        std::vector<std::string> row = cells(l);
        if (!started) {
            started = true;
            width = row.size();
            if (width < columns)
                throw SampleError("the " + std::string(heading) + " table has " + std::to_string(width) +
                                  (width == 1 ? " column" : " columns") + ", not " + std::to_string(columns));
            continue;
        }
        if (isRule(row))
            continue;
        row.resize(std::max(width, row.size()));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> splitChanges(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t semi = s.find(';', start);
        std::string part = trim(std::string_view(s).substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        if (!part.empty())
            out.push_back(std::move(part));
        if (semi == std::string::npos)
            break;
        start = semi + 1;
    }
    return out;
}

LayerId layerNamed(const LayerStack& stack, const std::string& name, const std::string& change) {
    std::optional<LayerId> id;
    for (const Layer& l : stack.layers())
        if (l.props.name == name) {
            if (id)
                throw SampleError("\"" + change + "\": two layers are named " + name);
            id = l.props.id;
        }
    if (!id)
        throw SampleError("\"" + change + "\": no layer is named " + name);
    return *id;
}

// "<a> <word> <b>": each way to split it at a " <word> ", a and b trimmed,
// from the first occurrence to the last.
std::vector<std::pair<std::string, std::string>> splitsAt(const std::string& rest, std::string_view word) {
    const std::string sep = " " + std::string(word) + " ";
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t at = rest.find(sep); at != std::string::npos; at = rest.find(sep, at + 1))
        out.emplace_back(trim(std::string_view(rest).substr(0, at)), trim(std::string_view(rest).substr(at + sep.size())));
    return out;
}

bool hasLayer(const LayerStack& stack, const std::string& name) {
    return std::any_of(stack.layers().begin(), stack.layers().end(),
                       [&](const Layer& l) { return l.props.name == name; });
}

fs::path envPath(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}

bool hidden(const fs::path& p) {
    const std::string n = p.filename().string();
    return n.empty() || n.front() == '.';
}

bool endsWithBwp(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(c | 0x20); });
    return e == ".bwp";
}

} // namespace

Notes parseNotes(std::string_view markdown) {
    Notes n;
    const std::vector<std::string_view> ls = lines(markdown);
    std::size_t i = 0;
    for (; i < ls.size(); ++i) {
        const std::string l = trim(ls[i]);
        if (l.rfind("# ", 0) == 0) {
            n.title = trim(std::string_view(l).substr(2));
            break;
        }
    }
    if (i < ls.size()) {
        std::string para;
        for (++i; i < ls.size(); ++i) {
            const std::string l = trim(ls[i]);
            if (l.empty()) {
                if (!para.empty())
                    break;
                continue;
            }
            if (l.front() == '#' || l.front() == '|')
                break;
            para += (para.empty() ? "" : " ") + l;
        }
        n.summary = para;
    }
    for (auto& row : table(ls, "Layers", 5))
        n.layers.push_back({row[0], row[1], row[2], row[3], row[4]});
    for (auto& row : table(ls, "Variants", 2))
        n.variants.push_back({row[0], splitChanges(row[1])});
    return n;
}

void applyVariant(LayerStack& stack, const Variant& variant) {
    for (const std::string& change : variant.changes) {
        const std::size_t sp = change.find(' ');
        const std::string verb = change.substr(0, sp);
        const std::string rest = sp == std::string::npos ? std::string() : trim(std::string_view(change).substr(sp + 1));
        if ((verb == "show" || verb == "hide") && !rest.empty()) {
            stack.find(layerNamed(stack, rest, change))->props.visible = verb == "show";
        } else if (verb == "raise" || verb == "lower") {
            const char* word = verb == "raise" ? "above" : "below";
            std::vector<std::pair<std::string, std::string>> splits = splitsAt(rest, word);
            std::erase_if(splits, [](const auto& p) { return p.first.empty() || p.second.empty(); });
            if (splits.empty())
                throw SampleError("\"" + change + "\": not " + verb + " <layer> " + word + " <layer>");
            // A name may hold the word: the split whose two halves both name
            // layers. With none, the first split names the layer that isn't there.
            std::vector<std::pair<std::string, std::string>> named;
            for (const auto& p : splits)
                if (hasLayer(stack, p.first) && hasLayer(stack, p.second))
                    named.push_back(p);
            if (named.size() > 1) {
                // Each name quoted, so the two readings read differently.
                const auto reading = [&](const auto& p) {
                    return verb + " \"" + p.first + "\" " + word + " \"" + p.second + "\"";
                };
                throw SampleError("\"" + change + "\": could be " + reading(named[0]) + " or " + reading(named[1]));
            }
            const auto& parts = named.empty() ? splits.front() : named.front();
            const LayerId moved = layerNamed(stack, parts.first, change);
            const LayerId other = layerNamed(stack, parts.second, change);
            if (moved == other)
                throw SampleError("\"" + change + "\": a layer can't move past itself");
            // To the bottom first, so `other` is at `at` >= 1 and the others
            // below it are the `at` - 1 under it with the moved layer left out:
            // its new index `at` puts it just above `other`, `at` - 1 just below.
            stack.move(moved, 0);
            const std::size_t at = *stack.indexOf(other);
            stack.move(moved, verb == "raise" ? at : at - 1);
        } else {
            throw SampleError("\"" + change + "\": not show, hide, raise or lower");
        }
    }
}

std::vector<Sample> list(const fs::path& dir) {
    std::vector<Sample> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return out;
    std::vector<fs::path> paths;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec)
            break;
        const fs::path p = it->path();
        if (hidden(p))
            continue;
        std::error_code e2;
        if ((it->is_regular_file(e2) && endsWithBwp(p)) || (it->is_directory(e2) && fs::exists(p / "manifest.json", e2)))
            paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    for (const fs::path& p : paths) {
        Sample s;
        s.path = p;
        std::string stem = p.filename().string();
        if (endsWithBwp(p))
            stem = p.stem().string();
        try {
            const project::Project proj = project::read(p);
            const Notes n = parseNotes(proj.notes);
            s.title = n.title;
            s.summary = n.summary;
        } catch (const std::exception& e) {
            s.problem = e.what();
        }
        if (s.title.empty())
            s.title = stem;
        out.push_back(std::move(s));
    }
    return out;
}

fs::path bundledDir(const fs::path& binDir) {
    if (fs::path p = envPath("BLOCKWRIGHT_SAMPLES"); !p.empty())
        return p;
    return (binDir / BW_SAMPLES_FROM_BINDIR).lexically_normal();
}

} // namespace bw::samples
