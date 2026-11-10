// Markdown files with YAML front matter: team, member and project files.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "myteam/yaml.hpp"

namespace myteam {

struct MarkdownFile {
    yaml::Node meta;      ///< the front matter (a mapping, or null when absent)
    std::string body;     ///< the Markdown after the front matter
    int body_line = 1;    ///< source line where the body starts
};

/// Split "---\n<yaml>\n---\n<body>". Throws yaml::Error for bad front matter.
MarkdownFile parse_markdown_file(std::string_view text);
MarkdownFile load_markdown_file(const std::filesystem::path& path);

}  // namespace myteam
