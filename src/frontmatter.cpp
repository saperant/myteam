#include "myteam/frontmatter.hpp"

#include "myteam/util.hpp"

namespace myteam {

MarkdownFile parse_markdown_file(std::string_view text) {
    MarkdownFile f;
    if (starts_with(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
    auto first_end = text.find('\n');
    std::string_view first = trim(text.substr(0, first_end));
    if (first != "---" || first_end == std::string_view::npos) {
        f.body = std::string(text);
        return f;
    }
    // Find the closing fence.
    std::size_t pos = first_end + 1;
    int line = 2;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        std::string_view l = trim(text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        if (l == "---" || l == "...") {
            f.meta = yaml::parse(text.substr(first_end + 1, pos - first_end - 1), 2);
            if (!f.meta.is_null() && !f.meta.is_mapping())
                throw yaml::Error("front matter must be a mapping", 2);
            std::string_view body = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
            f.body_line = line + 1;
            // Drop the blank lines that usually follow the fence.
            while (!body.empty() && (body[0] == '\n' || body[0] == '\r')) {
                if (body[0] == '\n') ++f.body_line;
                body.remove_prefix(1);
            }
            f.body = std::string(body);
            return f;
        }
        if (end == std::string_view::npos) break;
        pos = end + 1;
        ++line;
    }
    throw yaml::Error("unterminated front matter: expected ---", 1);
}

MarkdownFile load_markdown_file(const std::filesystem::path& path) {
    try {
        return parse_markdown_file(read_file(path));
    } catch (const yaml::Error& e) {
        throw yaml::Error(path.string() + ": " + e.what(), e.line());
    }
}

}  // namespace myteam
