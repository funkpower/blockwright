// Finds every place C++ sources could build a QSettings outside the settings
// factory (app/settings.cpp), so the tests' scratch INI file covers all of the
// app's settings. It reads tokens, not lines: comments and string literals are
// dropped and a declaration split across lines is still one declaration.
//
//   bw-settings-guard [--allow <file>]... <dir-or-file>...
//       prints each construction as path:line and exits 1 if there is one
//   bw-settings-guard --fixtures <dir>
//       checks the guard itself: every flag-* file there must be reported and
//       no pass-* file may be

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Token {
    enum Kind { Ident, Literal, Punct } kind;
    std::string text;
    int line;
};

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Skips a quoted literal starting at s[i] (the quote), counting newlines.
size_t skipQuoted(const std::string& s, size_t i, int& line) {
    const char q = s[i++];
    while (i < s.size() && s[i] != q) {
        if (s[i] == '\\' && i + 1 < s.size())
            ++i;
        if (s[i] == '\n')
            ++line;
        ++i;
    }
    return i + 1;
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;
    // A preprocessor directive ends at its line's end, so a #define's body is
    // read as a statement of its own.
    bool directive = false, lineStart = true;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            if (directive)
                out.push_back({Token::Punct, ";", line});
            directive = false;
            lineStart = true;
            ++line;
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < s.size() && s[i + 1] == '\n') {
            ++line;
            i += 2;
            continue;
        }
        if (c == '#' && lineStart)
            directive = true;
        if (!std::isspace(static_cast<unsigned char>(c)))
            lineStart = false;
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n')
                    ++line;
                ++i;
            }
            i += 2;
        } else if (identStart(c)) {
            size_t j = i;
            while (j < s.size() && identChar(s[j]))
                ++j;
            std::string word = s.substr(i, j - i);
            // a raw string: R"delim( ... )delim", possibly with an encoding prefix
            if (j < s.size() && s[j] == '"' && !word.empty() && word.back() == 'R' &&
                (word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R")) {
                const size_t open = s.find('(', j);
                const std::string end = ")" + s.substr(j + 1, open - j - 1) + "\"";
                const size_t close = open == std::string::npos ? std::string::npos : s.find(end, open);
                const size_t stop = close == std::string::npos ? s.size() : close + end.size();
                out.push_back({Token::Literal, "\"\"", line});
                line += static_cast<int>(std::count(s.begin() + i, s.begin() + stop, '\n'));
                i = stop;
                continue;
            }
            // an encoding prefix on an ordinary literal
            if (j < s.size() && (s[j] == '"' || s[j] == '\'') &&
                (word == "L" || word == "u" || word == "U" || word == "u8")) {
                const int at = line;
                i = skipQuoted(s, j, line);
                out.push_back({Token::Literal, "\"\"", at});
                continue;
            }
            out.push_back({Token::Ident, word, line});
            i = j;
        } else if (c == '"' || c == '\'') {
            const int at = line;
            i = skipQuoted(s, i, line);
            out.push_back({Token::Literal, "\"\"", at});
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < s.size() && (identChar(s[j]) || s[j] == '.' || s[j] == '\''))
                ++j;
            out.push_back({Token::Literal, s.substr(i, j - i), line});
            i = j;
        } else if (c == ':' && i + 1 < s.size() && s[i + 1] == ':') {
            out.push_back({Token::Punct, "::", line});
            i += 2;
        } else {
            out.push_back({Token::Punct, std::string(1, c), line});
            ++i;
        }
    }
    return out;
}

bool is(const std::vector<Token>& t, size_t i, const char* text) { return i < t.size() && t[i].text == text; }
bool ident(const std::vector<Token>& t, size_t i) { return i < t.size() && t[i].kind == Token::Ident; }

const std::set<std::string> kTypeWords = {"const", "volatile", "int", "bool", "char", "double", "float",
                                          "long", "short", "unsigned", "signed", "void", "auto"};

// Whether the parenthesised list opening at t[open] declares parameters (so
// "QSettings name(...)" declares a function) rather than passing arguments.
// Empty parentheses declare a function; so does a type followed by a name.
bool declaresParameters(const std::vector<Token>& t, size_t open) {
    if (is(t, open + 1, ")"))
        return true;
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i].text == "(")
            ++depth;
        else if (t[i].text == ")" && --depth == 0)
            return false;
        if (depth != 1 || t[i].kind != Token::Ident)
            continue;
        if (kTypeWords.count(t[i].text))
            return true;
        // Type name, Type& name, Type* name, Type&& name
        size_t j = i + 1;
        while (j < t.size() && (t[j].text == "&" || t[j].text == "*"))
            ++j;
        if (ident(t, j) && (is(t, j + 1, ",") || is(t, j + 1, ")") || is(t, j + 1, "=")))
            return true;
    }
    return false;
}

// The lines in t where a QSettings is (or could be) constructed.
std::vector<int> constructions(const std::vector<Token>& t) {
    std::vector<int> lines;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind != Token::Ident || t[i].text != "QSettings")
            continue;
        const std::string prev = i > 0 ? t[i - 1].text : std::string();
        // a name inside QSettings, the class's own scope, a conversion operator
        if (is(t, i + 1, "::") || prev == "operator" || prev == "~")
            continue;
        bool hit = false;
        // new QSettings, new QSettings(...), new (where) QSettings, new ::QSettings
        {
            size_t k = i;
            while (k > 0 && t[k - 1].text == "::")
                k = k >= 2 && ident(t, k - 2) && t[k - 2].text != "new" ? k - 2 : k - 1;
            if (k > 0 && t[k - 1].text == ")") {
                int depth = 0;
                size_t j = k - 1;
                for (;; --j) {
                    if (t[j].text == ")")
                        ++depth;
                    else if (t[j].text == "(" && --depth == 0)
                        break;
                    if (j == 0)
                        break;
                }
                k = j;
            }
            if (k > 0 && t[k - 1].text == "new")
                hit = true;
        }
        // std::make_unique<QSettings>(...), make_shared, allocate_shared, Q_GLOBAL_STATIC(QSettings, ...)
        if (prev == "<" && i >= 2 &&
            (t[i - 2].text == "make_unique" || t[i - 2].text == "make_shared" || t[i - 2].text == "allocate_shared" ||
             t[i - 2].text == "make_unique_for_overwrite" || t[i - 2].text == "make_shared_for_overwrite"))
            hit = true;
        if (prev == "(" && i >= 2 &&
            (t[i - 2].text == "Q_GLOBAL_STATIC" || t[i - 2].text == "Q_GLOBAL_STATIC_WITH_ARGS"))
            hit = true;
        // another name for it, or a class built on it
        if ((prev == "=" && i >= 3 && t[i - 3].text == "using") || prev == "typedef" ||
            ((prev == "public" || prev == "protected" || prev == "private") && i >= 2 &&
             (t[i - 2].text == ":" || t[i - 2].text == ",")))
            hit = true;
        // QSettings(...) or QSettings{...}: a temporary
        if (is(t, i + 1, "(") || is(t, i + 1, "{"))
            hit = true;
        // QSettings name; QSettings name = ...; QSettings name{...}; QSettings a, b;
        // QSettings name(args) — but not QSettings name() or QSettings name(Type param)
        size_t n = i + 1;
        while (is(t, n, "const") || is(t, n, "volatile"))
            ++n;
        if (ident(t, n)) {
            const size_t after = n + 1;
            if (is(t, after, ";") || is(t, after, ",") || is(t, after, "=") || is(t, after, "{") || is(t, after, "["))
                hit = true;
            else if (is(t, after, "(") && !declaresParameters(t, after))
                hit = true;
        }
        if (hit)
            lines.push_back(t[i].line);
    }
    return lines;
}

std::vector<int> scanFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return constructions(tokenize(text));
}

bool source(const fs::path& p) {
    const std::string ext = p.extension().string();
    return ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" || ext == ".mm";
}

std::vector<fs::path> sources(const fs::path& root) {
    std::vector<fs::path> out;
    if (fs::is_regular_file(root)) {
        out.push_back(root);
    } else {
        for (const auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && source(e.path()))
                out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int fixtures(const fs::path& dir) {
    int bad = 0, checked = 0;
    for (const fs::path& p : sources(dir)) {
        const std::string name = p.filename().string();
        const bool flag = name.rfind("flag-", 0) == 0;
        if (!flag && name.rfind("pass-", 0) != 0)
            continue;
        ++checked;
        const auto hits = scanFile(p);
        if (flag == hits.empty()) {
            std::fprintf(stderr, "%s: expected %s, got %zu construction(s)\n", p.string().c_str(),
                         flag ? "a construction" : "none", hits.size());
            ++bad;
        }
    }
    if (checked == 0) {
        std::fprintf(stderr, "no flag-* or pass-* fixtures in %s\n", dir.string().c_str());
        return 1;
    }
    std::printf("%d fixture(s), %d wrong\n", checked, bad);
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 2 && args[0] == "--fixtures")
        return fixtures(args[1]);
    std::set<fs::path> allowed;
    std::vector<fs::path> roots;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--allow" && i + 1 < args.size())
            allowed.insert(fs::weakly_canonical(args[++i]));
        else
            roots.emplace_back(args[i]);
    }
    if (roots.empty()) {
        std::fprintf(stderr, "usage: bw-settings-guard [--allow <file>]... <dir-or-file>... | --fixtures <dir>\n");
        return 2;
    }
    bool found = false;
    for (const fs::path& root : roots) {
        if (!fs::exists(root)) {
            std::fprintf(stderr, "%s: no such file or directory\n", root.string().c_str());
            return 2;
        }
        for (const fs::path& p : sources(root)) {
            if (allowed.count(fs::weakly_canonical(p)))
                continue;
            for (int line : scanFile(p)) {
                if (!found)
                    std::fprintf(stderr, "QSettings built outside app/settings.cpp:\n");
                std::fprintf(stderr, "%s:%d\n", p.string().c_str(), line);
                found = true;
            }
        }
    }
    return found ? 1 : 0;
}
