#include "diagnostic_log.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

void DiagnosticLog::start(const std::string& path) {
    if (!limit_ || file_fd_ >= 0) throw std::runtime_error("Invalid native log configuration");
    path_ = path;
    previous_ = path + ".previous";
    file_fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file_fd_ < 0) throw std::runtime_error("Could not open native launch log");
    unlink(previous_.c_str());
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) throw std::runtime_error("Could not create native log pipe");
    reader_fd_ = pipe_fds[0];
    writer_fd_ = pipe_fds[1];
    saved_out_ = dup(STDOUT_FILENO);
    saved_err_ = dup(STDERR_FILENO);
    if (saved_out_ < 0 || saved_err_ < 0) throw std::runtime_error("Could not preserve native output");
    fflush(stdout);
    fflush(stderr);
    reader_ = std::thread(&DiagnosticLog::read_output, this);
    if (dup2(writer_fd_, STDOUT_FILENO) < 0 || dup2(writer_fd_, STDERR_FILENO) < 0) {
        throw std::runtime_error("Could not redirect native output");
    }
    close(writer_fd_);
    writer_fd_ = -1;
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
}

DiagnosticLog::~DiagnosticLog() {
    fflush(stdout);
    fflush(stderr);
    if (saved_out_ >= 0) { dup2(saved_out_, STDOUT_FILENO); close(saved_out_); }
    if (saved_err_ >= 0) { dup2(saved_err_, STDERR_FILENO); close(saved_err_); }
    if (writer_fd_ >= 0) close(writer_fd_);
    if (reader_.joinable()) reader_.join();
    if (reader_fd_ >= 0) close(reader_fd_);
    if (file_fd_ >= 0) close(file_fd_);
}

void DiagnosticLog::read_output() noexcept {
    char buffer[4096];
    size_t written = 0;
    for (;;) {
        const ssize_t count = read(reader_fd_, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            if (written == limit_) {
                close(file_fd_);
                rename(path_.c_str(), previous_.c_str());
                file_fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                written = 0;
            }
            // Keep draining the pipe even if storage becomes unavailable.
            if (file_fd_ < 0) break;
            const size_t bytes = std::min(limit_ - written, static_cast<size_t>(count) - offset);
            const ssize_t saved = write(file_fd_, buffer + offset, bytes);
            if (saved < 0 && errno == EINTR) continue;
            if (saved <= 0) break;
            written += saved;
            offset += saved;
        }
    }
}
