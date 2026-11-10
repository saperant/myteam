#include "myteam/skills.hpp"

#include <algorithm>
#include <cctype>

#include "myteam/frontmatter.hpp"
#include "myteam/report.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;

namespace {

std::string yaml_quote(const std::string& v) {
    std::string esc;
    for (char c : v) {
        if (c == '\n') esc += ' ';
        else if (c == '"' || c == '\\') esc += std::string("\\") + c;
        else esc += c;
    }
    return "\"" + esc + "\"";
}

std::vector<std::string> list_attr(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            std::string x(trim(cur));
            if (!x.empty() && x[0] == '@') x.erase(0, 1);
            if (!x.empty()) out.push_back(x);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

std::string yaml_list(const std::vector<std::string>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + yaml_quote(v[i]);
    return s + "]";
}

/// The word `w` in `text`, on word boundaries, ignoring case.
bool has_word(const std::string& text, const std::string& w) {
    if (w.empty()) return false;
    const std::string t = lower(text), x = lower(w);
    for (auto at = t.find(x); at != std::string::npos; at = t.find(x, at + 1)) {
        const bool before = at == 0 || !std::isalnum(static_cast<unsigned char>(t[at - 1]));
        const auto end = at + x.size();
        const bool after = end >= t.size() || !std::isalnum(static_cast<unsigned char>(t[end]));
        if (before && after) return true;
    }
    return false;
}

}  // namespace

fs::path skills_dir(const Project& project) { return project.state_dir() / "skills"; }

std::vector<Skill> load_skills(const Project& project) {
    std::vector<Skill> out;
    std::error_code ec;
    if (!fs::is_directory(skills_dir(project), ec)) return out;
    for (const auto& e : fs::directory_iterator(skills_dir(project), ec)) {
        const fs::path file = e.path() / "SKILL.md";
        if (!e.is_directory() || !fs::exists(file)) continue;
        try {
            MarkdownFile f = load_markdown_file(file);
            Skill s;
            s.slug = e.path().filename().string();
            s.name = f.meta.get("name", s.slug);
            s.description = f.meta.get("description");
            s.source = f.meta.get("source");
            if (f.meta["triggers"].is_sequence()) s.triggers = f.meta["triggers"].as_strings();
            if (f.meta["members"].is_sequence()) s.members = f.meta["members"].as_strings();
            s.body = std::string(trim(f.body));
            s.file = file;
            out.push_back(std::move(s));
        } catch (...) {
        }
    }
    std::sort(out.begin(), out.end(), [](const Skill& a, const Skill& b) { return a.slug < b.slug; });
    return out;
}

fs::path write_skill(const Project& project, const Skill& s) {
    const std::string slug = s.slug.empty() ? myteam::slug(s.name, 48) : s.slug;
    const fs::path file = skills_dir(project) / slug / "SKILL.md";
    fs::create_directories(file.parent_path());
    std::string text = "---\nname: " + yaml_quote(s.name) + "\ndescription: " + yaml_quote(s.description) +
                       "\ntriggers: " + yaml_list(s.triggers) + "\nmembers: " + yaml_list(s.members) +
                       (s.source.empty() ? "" : "\nsource: " + yaml_quote(s.source)) + "\n---\n\n" +
                       std::string(trim(s.body)) + "\n";
    write_file_atomic(file, text);
    return file;
}

std::vector<Skill> parse_skill_blocks(std::string_view text) {
    std::vector<Skill> out;
    for (const ReportBlock& b : report_blocks(text, "skill")) {
        Skill s;
        s.name = block_attr(b.attrs, "name");
        s.triggers = list_attr(block_attr(b.attrs, "triggers"));
        s.members = list_attr(block_attr(b.attrs, "members"));
        std::string body(trim(b.body));
        const auto nl = body.find('\n');
        s.description = std::string(trim(body.substr(0, nl)));
        s.body = nl == std::string::npos ? "" : std::string(trim(body.substr(nl + 1)));
        if (s.name.empty()) s.name = first_line(s.description, 60);
        if (s.name.empty() || s.body.empty()) continue;
        s.slug = slug(s.name, 48);
        out.push_back(std::move(s));
    }
    return out;
}

std::string skill_block(const Skill& s) {
    return "<skill name=\"" + s.name + "\" triggers=\"" + join(s.triggers, ", ") + "\" members=\"" + join(s.members, ", ") +
           "\">\n" + s.description + "\n" + s.body + "\n</skill>";
}

std::vector<const Skill*> skills_for(const std::vector<Skill>& skills, const taskml::Task& task, const std::string& member,
                                     std::size_t max) {
    std::string text = task.description;
    for (const auto& t : task.tags) text += " " + t;
    for (const auto& n : task.notes) text += " " + n;
    for (const auto& c : task.criteria) text += " " + c.description;
    std::vector<std::pair<int, const Skill*>> hits;
    for (const Skill& s : skills) {
        if (!s.members.empty() && std::find(s.members.begin(), s.members.end(), member) == s.members.end()) continue;
        int n = 0;
        for (const auto& w : s.triggers) n += has_word(text, w);
        if (n > 0) hits.emplace_back(n, &s);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const Skill*> out;
    for (const auto& [n, s] : hits)
        if (out.size() < max) out.push_back(s);
    return out;
}

std::string skills_section(const std::vector<Skill>& skills, const taskml::Task& task, const std::string& member) {
    std::vector<const Skill*> mine;
    for (const Skill& s : skills)
        if (s.members.empty() || std::find(s.members.begin(), s.members.end(), member) != s.members.end()) mine.push_back(&s);
    if (mine.empty()) return "";
    std::string out = "What the team has learned to do (skill_read gives any of them in full):\n\n";
    for (const Skill* s : mine) out += "- " + s->slug + ": " + s->name + (s->description.empty() ? "" : " — " + s->description) + "\n";
    for (const Skill* s : skills_for(skills, task, member)) out += "\n### Skill: " + s->name + "\n\n" + s->body + "\n";
    return out;
}

}  // namespace myteam
