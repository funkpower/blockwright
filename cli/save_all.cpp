#include "save_all.h"

#include "read_file.h"

#include "blockwright/project.h"

#include <QSaveFile>
#include <QString>

#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>

namespace bwcli {

std::optional<unsigned long long> tooFewOpenFiles(std::size_t files) {
#if __has_include(<sys/resource.h>)
    rlimit l{};
    if (getrlimit(RLIMIT_NOFILE, &l) != 0)
        return std::nullopt;
    const rlim_t want = rlim_t(files) + kSpareOpenFiles;
    if (l.rlim_cur != RLIM_INFINITY && l.rlim_cur < want) {
        const rlim_t had = l.rlim_cur;
        l.rlim_cur = l.rlim_max == RLIM_INFINITY ? want : std::min(want, l.rlim_max);
        if (setrlimit(RLIMIT_NOFILE, &l) != 0)
            return static_cast<unsigned long long>(had);
        if (l.rlim_cur < want)
            return static_cast<unsigned long long>(l.rlim_cur);
    }
#else
    (void)files;
#endif
    return std::nullopt;
}

void saveAll(const std::vector<std::pair<std::string, std::string>>& files, const std::function<void()>& beforeCommit) {
    namespace fs = std::filesystem;
    std::vector<bool> existed;
    for (const auto& [path, bytes] : files) {
        std::error_code ec;
        if (fs::is_directory(path, ec))
            throw Failure{path + ": not written: a directory is in the way"};
        if (!path.ends_with(".bwp") && bw::project::isProject(path))
            throw Failure{path + ": not written: it is a project (pack and unpack write projects)"};
        existed.push_back(fs::exists(fs::symlink_status(path, ec)));
    }
    // a QSaveFile dropped before its commit removes its temporary
    std::vector<std::unique_ptr<QSaveFile>> out;
    for (const auto& [path, bytes] : files) {
        auto f = std::make_unique<QSaveFile>(QString::fromStdString(path));
        f->setDirectWriteFallback(false);
        if (!f->open(QIODevice::WriteOnly))
            throw Failure{path + ": " + f->errorString().toStdString()};
        const auto n = static_cast<qint64>(bytes.size());
        errno = 0;
        if (f->write(bytes.data(), n) != n || !f->flush()) {
            // Qt's error string for a failed write is often "Unknown error"
            const std::string why = errno ? std::strerror(errno) : f->errorString().toStdString();
            throw Failure{path + ": write failed: " + why};
        }
        out.push_back(std::move(f));
    }
    if (beforeCommit)
        beforeCommit();
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!out[i]->commit()) {
            const std::string why = out[i]->errorString().toStdString();
            std::error_code ignored;
            for (std::size_t j = 0; j < i; ++j)
                if (!existed[j])
                    fs::remove(files[j].first, ignored);
            throw Failure{files[i].first + ": write failed: " + why};
        }
    }
}

} // namespace bwcli
