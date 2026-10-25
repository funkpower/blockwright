#include "blockwright/generator.h"

#include "blockwright/ansi.h"
#include "blockwright/selection.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <utility>

extern char** environ;

namespace fs = std::filesystem;

namespace bw::generator {
namespace {

[[noreturn]] void fail(const std::string& what) { throw GeneratorError(what); }

std::string utf8(const QString& s) { return s.toStdString(); }

// A key some object in `json` holds twice, or nothing; `json` is valid JSON.
// QJsonDocument keeps only the last of a repeated key, so repeats are found
// here, in every object at any depth (the rule project.cpp reads manifests
// by).
std::optional<QString> repeatedKey(std::string_view json) {
    std::vector<std::optional<std::set<QString>>> open;  // nullopt: an array
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{') {
            open.emplace_back(std::set<QString>{});
        } else if (c == '[') {
            open.emplace_back(std::nullopt);
        } else if ((c == '}' || c == ']') && !open.empty()) {
            open.pop_back();
        } else if (c == '"') {
            const std::size_t start = i;
            for (++i; i < json.size() && json[i] != '"'; ++i)
                if (json[i] == '\\')
                    ++i;
            if (open.empty() || !open.back())
                continue;
            std::size_t j = i + 1;
            while (j < json.size() && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r'))
                ++j;
            if (j < json.size() && json[j] == ':') {
                const std::string raw = "[" + std::string(json.substr(start, i - start + 1)) + "]";
                const QString key = QJsonDocument::fromJson(QByteArray::fromStdString(raw)).array().at(0).toString();
                if (!open.back()->insert(key).second)
                    return key;
            }
        }
    }
    return std::nullopt;
}

QJsonObject parseObject(std::string_view text, const std::string& what) {
    if (text.size() > project::kMaxJsonBytes)
        fail(what + " is over " + std::to_string(project::kMaxJsonBytes) + " bytes");
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(text.data(), qsizetype(text.size())), &err);
    if (err.error != QJsonParseError::NoError)
        fail(what + " is not JSON: " + utf8(err.errorString()) + " at offset " + std::to_string(err.offset));
    if (!doc.isObject())
        fail(what + " is not a JSON object");
    if (const auto key = repeatedKey(text))
        fail(what + " has the key \"" + utf8(*key) + "\" more than once");
    return doc.object();
}

const Layer& generatorLayer(const project::Project& project, LayerId id) {
    const Layer* l = project.layers.find(id);
    if (!l)
        fail("no layer " + std::to_string(id));
    if (l->props.kind != LayerKind::Generator)
        fail("layer " + std::to_string(id) + " (" + l->props.name + ") is a " +
             std::string(layerKindName(l->props.kind)) + " layer, not a generator");
    return *l;
}

const std::string* file(const project::Project& project, const std::string& path) {
    const auto it = project.files.find(path);
    return it == project.files.end() ? nullptr : &it->second;
}

// The process group of the generator running now (its program's pid), 0
// when none is, or kStarting while one is being started.
constexpr pid_t kStarting = -1;
std::atomic<pid_t> runningGroup{0};
// How many killRunning calls have read runningGroup and not yet signalled
// the group they read.
std::atomic<int> killers{0};
// Set for good by the first killRunning: no run starts after it.
std::atomic<bool> stopping{false};
static_assert(std::atomic<pid_t>::is_always_lock_free && std::atomic<int>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free,
              "read and written in signal handlers");

// Kills every process in `group`; nothing for 0 or less (kill(0) would be
// this process's own group, kill(-1) every process it may signal).
void killGroup(pid_t group) noexcept {
    if (group > 0) {
        if (testing::killing)
            testing::killing(group);
        ::kill(-group, SIGKILL);
    }
}

// A file descriptor, closed with it.
struct Fd {
    int fd = -1;
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { close(); }
    // Takes `f`, moved above the standard streams (so putting the child's
    // in place can't overwrite another), closing on exec.
    bool take(int f) noexcept {
        if (f >= 0 && f <= 2) {
            const int above = ::fcntl(f, F_DUPFD_CLOEXEC, 3);
            ::close(f);
            f = above;
        }
        fd = f;
        return f >= 0;
    }
    void close() noexcept {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
};

// A pipe whose ends close on exec, the read end not blocking.
bool outputPipe(Fd& read, Fd& write) {
    int fds[2];
#ifdef __linux__
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return false;
#else
    if (::pipe(fds) != 0)
        return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    const bool ok = read.take(fds[0]) & write.take(fds[1]);
    return ok && ::fcntl(read.fd, F_SETFL, ::fcntl(read.fd, F_GETFL) | O_NONBLOCK) == 0;
}

// Starts `argv` (argv[0] looked up as posix_spawnp does) in `dir` with
// `in`, `out` and `err` as its standard streams, leading a process group of
// its own, and publishes that group as the running one; returns its pid,
// the group's id. Every signal is blocked on this thread from before the
// start until the group is published, and a killRunning on another thread
// waits out kStarting, so a signal that comes meanwhile is handled with the
// group known. A killRunning that read runningGroup before kStarting was
// stored is met from this side: it set `stopping` before that read, and
// this stores kStarting before it reads `stopping` (each sequentially
// consistent), so one of the two sees the other: killRunning sees
// kStarting (and then the group), or this sees `stopping` and starts
// nothing, setting `error` to ECANCELED. The program gets the thread's
// signal mask as it was and SIGPIPE's default action (exec resets what has
// a handler). Returns 0 and sets `error` when it can't start.
pid_t start(const std::vector<std::string>& argv, const std::string& dir, int in, int out, int err, int& error) {
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    sigset_t all, old, defaults;
    sigfillset(&all);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, nullptr, &old);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    error = posix_spawn_file_actions_adddup2(&actions, in, 0);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, out, 1);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, err, 2);
    if (!error)
        error = posix_spawn_file_actions_addchdir_np(&actions, dir.c_str());
    if (!error)
        error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                                    POSIX_SPAWN_SETSIGDEF);
    if (!error)
        error = posix_spawnattr_setpgroup(&attr, 0);
    if (!error)
        error = posix_spawnattr_setsigmask(&attr, &old);
    if (!error)
        error = posix_spawnattr_setsigdefault(&attr, &defaults);
    pid_t pid = 0;
    if (!error) {
        pthread_sigmask(SIG_BLOCK, &all, nullptr);
        runningGroup = kStarting;
        if (stopping.load())
            error = ECANCELED;
        else
            error = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
        if (!error && testing::started)
            testing::started(pid);
        runningGroup = error ? 0 : pid;
        pthread_sigmask(SIG_SETMASK, &old, nullptr);  // what came meanwhile is handled here
    }
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return error ? 0 : pid;
}

// The running program's process group, from its start to its reap. Its id
// is the program's pid, which no other process can have until the program
// is reaped, so the group is only signalled before the reap: end() kills it
// a last time, retires it (no longer running, and a killRunning that read
// it let finish), and only then reaps the program. Nothing signals that
// number after. Made only for a program that started (`leader` > 0).
class Group {
public:
    explicit Group(pid_t leader) noexcept : leader_(leader), reaped_(leader <= 0) {}
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    ~Group() { end(); }

    void kill() const noexcept {
        if (!reaped_)
            killGroup(leader_);
    }

    // How the program ended, seen without reaping it; nullopt while it runs.
    std::optional<siginfo_t> ended() noexcept {
        siginfo_t info{};
        if (::waitid(P_PID, id_t(leader_), &info, WEXITED | WNOHANG | WNOWAIT) == 0)
            return info.si_pid == leader_ ? std::optional(info) : std::nullopt;
        // reaped by something else, against run's precondition (SIGCHLD
        // ignored since run looked, or a wait for any child): its number is
        // no longer ours
        if (errno == ECHILD) {
            retire();
            reaped_ = true;
            info.si_code = CLD_KILLED;
            return info;
        }
        return std::nullopt;
    }

    void end() noexcept {
        if (reaped_)
            return;
        killGroup(leader_);
        retire();
        while (::waitpid(leader_, nullptr, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
        if (testing::reaped)
            testing::reaped(leader_);
    }

private:
    void retire() noexcept {
        pid_t running = leader_;
        runningGroup.compare_exchange_strong(running, 0);
        while (killers.load() != 0)
            std::this_thread::yield();
    }

    pid_t leader_;
    bool reaped_;
};

// Whether this process ignores SIGCHLD (SIG_IGN, or SA_NOCLDWAIT), when
// the kernel reaps each child as it exits.
bool sigchldIgnored() noexcept {
    struct sigaction sa {};
    return ::sigaction(SIGCHLD, nullptr, &sa) == 0 && (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT));
}

// The last `n` bytes of `s`, from a line start where there is one.
std::string tail(const QByteArray& s, qsizetype n) {
    QByteArray t = s.size() > n ? s.right(n) : s;
    if (s.size() > n)
        if (const qsizetype nl = t.indexOf('\n'); nl >= 0)
            t = t.mid(nl + 1);
    return t.trimmed().toStdString();
}

} // namespace

std::string directory(LayerId id) { return "scripts/" + std::to_string(id) + "/"; }

void testing::allowRuns() noexcept { stopping = false; }

void killRunning() noexcept {
    ++killers;
    stopping = true;  // before runningGroup is read (start)
    pid_t group;
    // a run starting on another thread, with every signal blocked there,
    // publishes its group in a moment
    while ((group = runningGroup.load()) == kStarting) {
    }
    killGroup(group);
    --killers;
}

Spec parseSpec(std::string_view json) {
    const std::string what(kSpecFile);
    const QJsonObject o = parseObject(json, what);
    Spec spec;
    const QJsonValue command = o.value(QLatin1String("command"));
    if (!command.isArray() || command.toArray().isEmpty())
        fail(what + ": \"command\" is not a list of the program and its arguments");
    for (const QJsonValue& v : command.toArray()) {
        if (!v.isString() || v.toString().isEmpty() || v.toString().contains(QChar(0)))
            fail(what + ": \"command\" holds something other than a word (a non-empty string)");
        spec.command.push_back(utf8(v.toString()));
    }
    const QJsonValue key = o.value(QLatin1String("key"));
    if (!key.isUndefined() && !key.isNull()) {
        const double d = key.toDouble(std::nan(""));
        if (!key.isDouble() || d != std::floor(d) || d < 0 || d > 15)
            fail(what + ": \"key\" is not a palette colour, a whole number from 0 to 15");
        spec.key = std::uint8_t(d);
    }
    return spec;
}

std::string specJson(const Spec& spec) {
    QJsonArray command;
    for (const std::string& w : spec.command)
        command.append(QString::fromStdString(w));
    QJsonObject o{{QStringLiteral("command"), command}};
    if (spec.key)
        o.insert(QStringLiteral("key"), int(*spec.key));
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Indented);
    return std::string(b.constData(), std::size_t(b.size()));
}

std::vector<LayerId> generatorLayers(const LayerStack& stack) {
    std::vector<LayerId> ids;
    for (const Layer& l : stack.layers())
        if (l.props.kind == LayerKind::Generator)
            ids.push_back(l.props.id);
    return ids;
}

Spec layerSpec(const project::Project& project, LayerId id) {
    const Layer& l = generatorLayer(project, id);
    const std::string path = directory(id) + std::string(kSpecFile);
    const std::string* json = file(project, path);
    if (!json)
        fail("layer " + std::to_string(id) + " (" + l.props.name + ") has no " + path);
    try {
        return parseSpec(*json);
    } catch (const GeneratorError& e) {
        fail(directory(id) + e.what());
    }
}

std::string input(const project::Project& project, const Layer& layer) {
    QJsonObject params;
    const std::string path = directory(layer.props.id) + std::string(kParamsFile);
    if (const std::string* json = file(project, path))
        params = parseObject(*json, path);
    const LayerStack& s = project.layers;
    const QJsonObject o{
        {QStringLiteral("canvas"),
         QJsonObject{{QStringLiteral("width"), s.width()},
                     {QStringLiteral("height"), s.height()},
                     {QStringLiteral("ice"), project.ice}}},
        {QStringLiteral("layer"),
         QJsonObject{{QStringLiteral("id"), qint64(layer.props.id)},
                     {QStringLiteral("name"), QString::fromStdString(layer.props.name)},
                     {QStringLiteral("x"), layer.props.x},
                     {QStringLiteral("y"), layer.props.y},
                     {QStringLiteral("width"), layer.width()},
                     {QStringLiteral("height"), layer.height()}}},
        {QStringLiteral("params"), params},
    };
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return std::string(b.constData(), std::size_t(b.size())) + "\n";
}

Canvas cellsFromOutput(std::string_view ans, int width, std::optional<std::uint8_t> key) {
    ParseOptions opts;
    opts.defaultWidth = std::clamp(width, 1, kMaxColumns);
    AnsiDocument doc;
    try {
        doc = parseAnsi(ans, opts);
    } catch (const AnsiError& e) {
        fail(std::string("the output is not an .ans: ") + e.what());
    }
    openAsEditor(doc);
    if (key)
        for (int y = 0; y < doc.canvas.height(); ++y)
            for (int x = 0; x < doc.canvas.width(); ++x) {
                Cell& c = doc.canvas.at(x, y);
                const Cell shown = iceCell(c, doc.ice);
                if (blankGlyph(shown.ch) && !(shown.attrs & attr::Blink) && shown.bg.isIndexed() &&
                    shown.bg.index == *key)
                    c = transparentCell();
            }
    return std::move(doc.canvas);
}

Canvas run(const project::Project& project, LayerId id, const RunOptions& options) {
    const Layer& layer = generatorLayer(project, id);
    const Spec spec = layerSpec(project, id);
    const std::string stdinJson = input(project, layer);
    const std::string name = "layer " + std::to_string(id) + " (" + layer.props.name + ")";
    // Checked here, before anything starts: once the program has been
    // reaped by the kernel its group's id is free for reuse, so the run
    // could no longer kill what the program left running.
    if (sigchldIgnored())
        fail(name + ": not run: SIGCHLD is ignored in this process (SIG_IGN or SA_NOCLDWAIT), so the program "
                    "would be reaped as it exits and what it left running could not be stopped");

    QTemporaryDir tmp;
    if (!tmp.isValid())
        fail(name + ": no temporary directory to run the generator in: " + utf8(tmp.errorString()));
    const fs::path root = tmp.path().toStdString();
    const std::string dir = directory(id);
    for (auto it = project.files.lower_bound(dir); it != project.files.end() && it->first.starts_with(dir); ++it) {
        const fs::path to = root / it->first.substr(dir.size());
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        std::ofstream out(to, std::ios::binary);
        out.write(it->second.data(), std::streamsize(it->second.size()));
        out.close();
        if (ec || !out)
            fail(name + ": could not copy " + it->first + " to run it");
        fs::permissions(to, fs::perms::owner_all, ec);
    }

    std::vector<std::string> argv = spec.command;
    if (!argv[0].starts_with('/') && argv[0].find('/') != std::string::npos) {
        std::string_view rel = spec.command[0];
        while (rel.starts_with("./"))
            rel.remove_prefix(2);
        if (!project::safeEntryPath(rel))
            fail(name + ": the program " + spec.command[0] + " is not a file in " + dir);
        argv[0] = (root / rel).string();
    }

    // stdin is a file holding the JSON, so a program that doesn't read it
    // can't stall the run, and nothing is written to a pipe it has closed.
    QTemporaryFile inFile;
    if (!inFile.open() || inFile.write(stdinJson.data(), qint64(stdinJson.size())) != qint64(stdinJson.size()) ||
        !inFile.flush())
        fail(name + ": could not write the generator's input: " + utf8(inFile.errorString()));
    Fd in, outR, outW, errR, errW;
    if (!in.take(::open(QFile::encodeName(inFile.fileName()).constData(), O_RDONLY | O_CLOEXEC)) ||
        !outputPipe(outR, outW) || !outputPipe(errR, errW))
        fail(name + ": could not set up the run: " + std::strerror(errno));

    // The program leads a process group of its own, which everything it
    // starts joins unless it leaves on purpose (setsid, setpgid): however the
    // run ends, the group is killed, so nothing it started outlives it.
    //
    // Output is read as it arrives, at most one buffer from each pipe
    // between looks at the clock and at the program, so the cap and the
    // deadline hold against a flood and what is held stays near the cap.
    int error = 0;
    const pid_t leader = start(argv, utf8(tmp.path()), in.fd, outW.fd, errW.fd, error);
    if (error == ECANCELED)
        fail(name + ": not run: generators are being stopped");
    if (error)
        fail(name + ": could not run " + spec.command[0] + ": " + std::strerror(error));
    Group group(leader);
    in.close();
    outW.close();
    errW.close();
    const QDeadlineTimer deadline(options.timeout);
    QByteArray out, err;
    std::string stopped;  // why the group was killed
    std::vector<char> buf(64 * 1024);
    // Takes what one read of `fd` gives; false once it gives nothing now.
    const auto take = [&](Fd& fd) {
        if (fd.fd < 0)
            return false;
        const ssize_t n = ::read(fd.fd, buf.data(), buf.size());
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                fd.close();
            return false;
        }
        if (&fd == &errR) {
            err.append(buf.data(), n);
            if (err.size() > 8192)
                err = err.right(4096);
        } else if (std::size_t(out.size()) + std::size_t(n) > kMaxOutputBytes) {
            stopped = spec.command[0] + " wrote more than " + std::to_string(kMaxOutputBytes) + " bytes; stopped";
        } else {
            out.append(buf.data(), n);
        }
        return true;
    };
    std::optional<siginfo_t> ended;
    while (stopped.empty() && !(ended = group.ended())) {
        if (deadline.hasExpired()) {
            stopped = spec.command[0] + " took longer than " + std::to_string(options.timeout.count()) +
                      " ms; stopped";
            break;
        }
        pollfd fds[2];
        nfds_t n = 0;
        for (const Fd* f : {&outR, &errR})
            if (f->fd >= 0)
                fds[n++] = pollfd{f->fd, POLLIN, 0};
        ::poll(fds, n, int(std::clamp<qint64>(deadline.remainingTime(), 1, 50)));
        take(outR);
        take(errR);
    }
    if (!stopped.empty())
        fail(name + ": " + stopped);  // ~Group kills the group
    // What it left running ends with it; what was written by then is read.
    group.end();
    while (stopped.empty() && take(outR)) {
    }
    while (take(errR)) {
    }
    if (!stopped.empty())
        fail(name + ": " + stopped);
    const std::string said = tail(err, 2000);
    const std::string stderrNote = said.empty() ? "" : ":\n" + said;
    if (ended->si_code != CLD_EXITED)
        fail(name + ": " + spec.command[0] + " crashed" + stderrNote);
    if (ended->si_status != 0)
        fail(name + ": " + spec.command[0] + " exited with status " + std::to_string(ended->si_status) + stderrNote);
    const int width = layer.width() > 0 ? layer.width() : project.layers.width();
    try {
        return cellsFromOutput(std::string_view(out.constData(), std::size_t(out.size())), width, spec.key);
    } catch (const GeneratorError& e) {
        fail(name + ": " + e.what());
    }
}

void refresh(project::Project& project, LayerId id, const RunOptions& options) {
    Canvas cells = run(project, id, options);
    project.layers.find(id)->cells = std::move(cells);
}

LayerId add(project::Project& project, const std::map<std::string, std::string>& files, std::string name,
            const RunOptions& options) {
    if (!files.contains(std::string(kSpecFile)))
        fail("a generator needs a " + std::string(kSpecFile));
    parseSpec(files.at(std::string(kSpecFile)));
    project::Project p = project;
    LayerId id = 0;
    try {
        id = p.layers.add(makeLayer(LayerKind::Generator, p.layers.width(), p.layers.height(), std::move(name)));
    } catch (const LayerError& e) {
        fail(e.what());
    }
    const std::string dir = directory(id);
    for (auto it = p.files.lower_bound(dir); it != p.files.end() && it->first.starts_with(dir); ++it)
        fail("the project already has files in " + dir + " (" + it->first + ")");
    for (const auto& [path, bytes] : files) {
        if (!project::safeEntryPath(dir + path))
            fail("a generator can't hold the file " + path + " (" + dir + path + " is not a path a project takes)");
        p.files[dir + path] = bytes;
    }
    refresh(p, id, options);
    project = std::move(p);
    return id;
}

} // namespace bw::generator
