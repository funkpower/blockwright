#pragma once

// Timeout guards for tests that put a FIFO where a reader expects a file: a
// reader that regresses to a blocking open fails the test rather than hangs
// it. POSIX only (Windows has no FIFOs).

#if defined(__unix__) || defined(__APPLE__)

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bwtest {

// Gives the FIFO `fifo` a writer if it is still alive after `limit`, so a
// reader blocked opening or reading it gets end of file and returns. Make one
// before calling the reader on this thread (one that must stay here, such as
// a widget's), then check unblocked() after it returns.
class FifoUnblocker {
public:
    explicit FifoUnblocker(std::filesystem::path fifo, std::chrono::seconds limit = std::chrono::seconds(60))
        : fifo_(std::move(fifo)) {
        thread_ = std::thread([this, limit] {
            std::unique_lock lock(m_);
            if (cv_.wait_for(lock, limit, [this] { return done_; }))
                return;
            // the reader is past its limit: open a writer once it is waiting
            while (!done_) {
                const int fd = ::open(fifo_.c_str(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0) {
                    unblocked_ = true;
                    ::close(fd);
                }
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return done_; });
            }
        });
    }
    ~FifoUnblocker() {
        {
            std::lock_guard lock(m_);
            done_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    FifoUnblocker(const FifoUnblocker&) = delete;
    FifoUnblocker& operator=(const FifoUnblocker&) = delete;
    // Whether the reader had to be given a writer: it blocked.
    bool unblocked() const { return unblocked_; }

private:
    std::filesystem::path fifo_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::atomic<bool> unblocked_{false};
    std::thread thread_;
};

// What `f` returns, run on a thread of its own ("threw: <what>" if it
// throws). If it is still running after a minute, whatever waits on the FIFO
// `fifo` is given a writer, so the thread ends (it is given a while to,
// before the test's locals it may use go), and the test fails rather than
// hangs.
inline std::string within(const std::filesystem::path& fifo, std::function<std::string()> f) {
    auto result = std::make_shared<std::promise<std::string>>();
    std::future<std::string> done = result->get_future();
    std::thread([f = std::move(f), result] {
        try {
            result->set_value(f());
        } catch (const std::exception& e) {
            result->set_value(std::string("threw: ") + e.what());
        }
    }).detach();
    if (done.wait_for(std::chrono::seconds(60)) != std::future_status::ready) {
        const int fd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
            ::close(fd);
        done.wait_for(std::chrono::seconds(10));
        FAIL("blocked on " << fifo.string());
    }
    return done.get();
}

} // namespace bwtest

#endif
