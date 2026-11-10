// A member's memory: what makes an expert the same person from task to task.
//
//   .myteam/memory/<handle>.md
//     ## From the coach        the coach's coaching (<coaching to="h">), replaced as a whole
//     ## Lessons               what the member learned (<lesson> in its reports), newest last
//
// Every context of the member has it after the role. The file is myteam's to write (agents
// cannot: .myteam/ is protected); the coach keeps it short, and lessons beyond `keep` drop
// the oldest. The coach's own notes (<notes>) are its memory file, coach.md.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "myteam/project.hpp"

namespace myteam {

std::filesystem::path memory_dir(const Project& project);
std::filesystem::path memory_path(const Project& project, const std::string& handle);

struct Memory {
    std::string coaching;              ///< the "From the coach" section
    std::vector<std::string> lessons;  ///< "- date ^task: text" lines, oldest first
    std::string other;                 ///< anything else in the file (kept as it is)
};

Memory parse_memory(std::string_view text);
std::string render_memory(const Memory& m);
Memory load_memory(const Project& project, const std::string& handle);
void save_memory(const Project& project, const std::string& handle, const Memory& m);

/// The memory as the member's context shows it; "" when there is none. Cut to `limit`
/// characters, keeping the coaching and the newest lessons.
std::string memory_section(const Project& project, const std::string& handle, std::size_t limit = 6000);

/// Add lessons (one line each in the file), keeping the newest `keep`. Returns the lines added.
std::vector<std::string> add_lessons(const Project& project, const std::string& handle, const std::string& task_id,
                                     const std::vector<std::string>& lessons, const std::string& date, std::size_t keep = 30);
/// Replace the coach's section.
void set_coaching(const Project& project, const std::string& handle, const std::string& text);

}  // namespace myteam
