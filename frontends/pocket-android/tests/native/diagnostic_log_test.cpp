#include "diagnostic_log.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

int main() {
    char temporary[] = "/tmp/bada-log-test-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const fs::path directory(temporary);
    const fs::path path = directory / "native.log";
    try {
        {
            DiagnosticLog log;
            log.start(path.string());
            printf("stdout: ELF loading started\n");
            fprintf(stderr, "stderr: emulation failed at pc=0x1234\n");
        }
        auto captured = read(path);
        require(captured.find("stdout: ELF loading started") != std::string::npos, "stdout was not captured");
        require(captured.find("stderr: emulation failed at pc=0x1234") != std::string::npos, "stderr was not drained on shutdown");
        {
            DiagnosticLog log(512);
            log.start(path.string());
            for (int i = 0; i < 2000; ++i) printf("frame=%d diagnostic output\n", i);
            fprintf(stderr, "FINAL ERROR: missing Bada API\n");
        }
        const fs::path previous(path.string() + ".previous");
        require(fs::file_size(path) <= 512 && fs::file_size(previous) <= 512, "logs exceeded the configured bound");
        require((read(previous) + read(path)).find("FINAL ERROR: missing Bada API") != std::string::npos,
                "rotation lost the latest error");
        bool failed = false;
        try {
            DiagnosticLog log;
            log.start((directory / "missing-directory/native.log").string());
        } catch (const std::exception&) { failed = true; }
        require(failed, "unavailable storage was not reported");
        fs::remove_all(directory);
        puts("Native diagnostics: capture, drain, rotation and storage failure passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        fs::remove_all(directory);
        return 1;
    }
}
