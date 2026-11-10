// Running a child process with its stdout read line by line.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace myteam {

struct ProcessOptions {
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    std::map<std::string, std::string> env;  ///< added to (or replacing in) the current environment
    std::string stdin_text;
    std::filesystem::path stderr_path;       ///< empty: discard
    int timeout_seconds = 0;                 ///< 0: none
};

struct ProcessResult {
    int exit_code = -1;   ///< -1 when it could not start; 128+n for signal n
    bool timed_out = false;
    std::string error;    ///< why it could not start
};

/// Runs the process in its own process group. `on_line` gets each stdout
/// line (without the newline). On timeout the group gets SIGTERM, then
/// SIGKILL after five seconds.
ProcessResult run_process(const ProcessOptions& options, const std::function<void(std::string_view)>& on_line);

}  // namespace myteam
