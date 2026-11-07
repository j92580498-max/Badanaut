#pragma once
#include <cstddef>
#include <string>
#include <thread>

// Capture native stdout/stderr in the app's private storage. The reader keeps
// two bounded files, so a noisy guest cannot fill the phone's storage.
class DiagnosticLog {
public:
    explicit DiagnosticLog(size_t file_limit = 2 * 1024 * 1024) : limit_(file_limit) {}
    ~DiagnosticLog();
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    void start(const std::string& path);

private:
    void read_output() noexcept;
    std::string path_, previous_;
    size_t limit_;
    int saved_out_ = -1, saved_err_ = -1;
    int reader_fd_ = -1, writer_fd_ = -1, file_fd_ = -1;
    std::thread reader_;
};
