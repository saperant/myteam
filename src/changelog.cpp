#include "myteam/changelog.hpp"

#include "myteam/util.hpp"

namespace myteam {

void append_changelog(const fs::path& path, std::string_view entry) {
    std::string out;
    auto existing = try_read_file(path);
    if (!existing || trim(*existing).empty()) {
        out = "# Changelog\n\n";
    } else if (existing->back() != '\n') {
        out = "\n\n";
    } else if (existing->size() < 2 || (*existing)[existing->size() - 2] != '\n') {
        out = "\n";
    }
    out += entry;
    if (out.empty() || out.back() != '\n') out += '\n';
    if (existing)
        append_file(path, out);
    else
        write_file_atomic(path, out);
}

}  // namespace myteam
