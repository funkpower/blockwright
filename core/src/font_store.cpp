#include "blockwright/font_store.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <utility>
#include <unordered_map>

namespace bw {

std::string sha256Hex(std::string_view bytes) {
    const QByteArray hash =
        QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                 QCryptographicHash::Sha256)
            .toHex();
    return hash.toStdString();
}

bool isSha256Hex(std::string_view s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

FontFile::FontFile(std::string b, std::vector<std::string> n, std::vector<TdfType> t)
    : sha256(sha256Hex(b)), bytes(std::move(b)), names(std::move(n)), types(std::move(t)) {}

const Face* FontFile::face(std::size_t i) const {
    std::call_once(made_, [this] {
        std::vector<std::unique_ptr<Face>> faces;
        try {
            std::vector<TdfFont> fonts = readTdf(bytes);
            if (fonts.size() == names.size())
                for (TdfFont& font : fonts)
                    faces.push_back(tdfFace(std::move(font)));
        } catch (const TdfError&) {
            faces.clear();
        }
        faces_ = std::move(faces);
        madeFlag_ = true;
    });
    if (i >= faces_.size() || faces_[i]->name() != names[i])
        return nullptr;
    return faces_[i].get();
}

bool FontFile::facesMade() const { return madeFlag_; }

FontFilePtr loadFontFile(std::string bytes) {
    if (bytes.size() > kMaxTdfBytes)
        throw TdfError("the file is over " + std::to_string(kMaxTdfBytes >> 20) + " MiB");
    std::vector<std::string> names;
    std::vector<TdfType> types;
    for (const TdfFont& font : readTdf(bytes)) {
        names.push_back(font.name);
        types.push_back(font.type);
    }
    if (names.empty())
        throw TdfError("the file holds no font");
    return std::make_shared<const FontFile>(std::move(bytes), std::move(names), std::move(types));
}

FontFilePtr FontStore::add(FontFilePtr file) {
    auto [it, added] = files_.try_emplace(file->sha256, file);
    return it->second;
}

FontFilePtr FontStore::file(std::string_view sha256) const {
    const auto it = files_.find(sha256);
    return it == files_.end() ? nullptr : it->second;
}

namespace {

const Face* faceIn(const FontFilePtr& file, const FaceRef& ref) {
    if (!file || ref.index < 0 || static_cast<std::size_t>(ref.index) >= file->names.size() ||
        file->names[static_cast<std::size_t>(ref.index)] != ref.name)
        return nullptr;
    return file->face(static_cast<std::size_t>(ref.index));
}

} // namespace

const Face* FontStore::face(const FaceRef& ref) const {
    if (ref.kind != FaceRef::Kind::Tdf)
        return nullptr;
    return faceIn(file(ref.file), ref);
}

bool FontStore::operator==(const FontStore& other) const {
    return std::equal(files_.begin(), files_.end(), other.files_.begin(), other.files_.end(),
                      [](const auto& a, const auto& b) { return a.first == b.first; });
}

const Face* FontSources::face(const FaceRef& ref) const {
    // the project's copy when it has the file, the folder's otherwise: a
    // file is the same bytes wherever it is, so the first holding it decides
    if (const FontFilePtr f = file(ref.file))
        return ref.kind == FaceRef::Kind::Tdf ? faceIn(f, ref) : nullptr;
    return nullptr;
}

FontFilePtr FontSources::file(std::string_view sha256) const {
    if (project)
        if (FontFilePtr f = project->file(sha256))
            return f;
    if (folder)
        if (FontFilePtr f = folder->file(sha256))
            return f;
    if (held)
        if (FontFilePtr f = held->file(sha256))
            return f;
    return nullptr;
}

std::vector<FaceEntry> FontCatalogue::entries() const {
    std::vector<FaceEntry> out;
    out.reserve(fonts.size());
    for (const Font& f : fonts)
        out.push_back({f.ref, f.displayName, nullptr, store.file(f.ref.file),
                       f.type == TdfType::Outline ? "outline" : f.type == TdfType::Block ? "block" : "colour"});
    return out;
}

namespace {

bool isTdfName(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".tdf";
}

} // namespace

FontCatalogue scanFontsFolder(const std::filesystem::path& folder, const std::function<bool()>& cancelled) {
    namespace fs = std::filesystem;
    FontCatalogue cat;
    cat.folder = folder;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec))
        return cat;
    std::vector<fs::path> files;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        cat.rejected.push_back({folder, "can't read the folder: " + ec.message()});
        return cat;
    }
    for (; it != end; it.increment(ec)) {
        if (cancelled && cancelled())
            return cat;
        if (ec) {
            cat.rejected.push_back({folder, "can't read the folder: " + ec.message()});
            break;
        }
        std::error_code fileEc;
        if (isTdfName(it->path()) && it->is_regular_file(fileEc))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        if (cancelled && cancelled())
            return cat;
        FontFilePtr file;
        try {
            std::string bytes = readTdfBytes(path);
            // a second copy of a file already listed adds nothing
            if (cat.store.file(sha256Hex(bytes)))
                continue;
            file = cat.store.add(loadFontFile(std::move(bytes)));
        } catch (const std::exception& e) {
            // TdfError, or the allocation a broken file asked for
            cat.rejected.push_back({path, e.what()});
            continue;
        }
        const std::string fileName = path.filename().string();
        for (std::size_t i = 0; i < file->names.size(); ++i) {
            FaceRef ref;
            ref.kind = FaceRef::Kind::Tdf;
            ref.file = file->sha256;
            ref.index = static_cast<int>(i);
            ref.name = file->names[i];
            cat.fonts.push_back({ref, file->names[i] + " (" + fileName + ")", path, file->types[i], file});
        }
    }
    // Keep short labels unless two listed variants would look identical.
    std::unordered_map<std::string, std::size_t> labels;
    for (const auto& font : cat.fonts)
        ++labels[font.displayName];
    for (auto& font : cat.fonts)
        if (labels[font.displayName] > 1)
            font.displayName = font.ref.name + " (" + font.file.lexically_relative(folder).generic_string() + ")";
    // Two fonts with one name in one file still collide; number them by
    // their place in the file, counting from 1.
    labels.clear();
    for (const auto& font : cat.fonts)
        ++labels[font.displayName];
    for (auto& font : cat.fonts)
        if (labels[font.displayName] > 1)
            font.displayName = font.ref.name + " (" + font.file.lexically_relative(folder).generic_string() + " #"
                + std::to_string(font.ref.index + 1) + ")";
    return cat;
}

} // namespace bw
