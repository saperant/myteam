// CHANGELOG.md: one entry per agent run, newest last.
#pragma once

#include <filesystem>
#include <string_view>

namespace myteam {

/// Append a Markdown entry (starting with "## "), creating the file with a
/// "# Changelog" heading when missing.
void append_changelog(const std::filesystem::path& path, std::string_view entry);

}  // namespace myteam
