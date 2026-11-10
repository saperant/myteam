// Helpers for the tests: temporary project directories.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "myteam/util.hpp"

namespace myteam::test {

/// A fresh directory, removed when the object goes away.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("myteam-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& rel) const { return path_ / rel; }
    void write(const std::string& rel, const std::string& content) const { write_file_atomic(path_ / rel, content); }
    std::string read(const std::string& rel) const { return read_file(path_ / rel); }

private:
    std::filesystem::path path_;
};

inline std::filesystem::path data_dir() { return MYTEAM_TEST_DATA; }

}  // namespace myteam::test
