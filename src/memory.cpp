#include "myteam/memory.hpp"

#include "myteam/util.hpp"

namespace myteam {

namespace {
constexpr const char* kCoachHead = "## From the coach";
constexpr const char* kLessonsHead = "## Lessons";
}  // namespace

fs::path memory_dir(const Project& project) { return project.state_dir() / "memory"; }
fs::path memory_path(const Project& project, const std::string& handle) { return memory_dir(project) / (handle + ".md"); }

Memory parse_memory(std::string_view text) {
    Memory m;
    enum { Other, Coach, Lessons } where = Other;
    std::string coaching;
    for (const std::string& line : split_lines(text)) {
        const std::string_view t = trim(line);
        if (t == kCoachHead) {
            where = Coach;
            continue;
        }
        if (t == kLessonsHead) {
            where = Lessons;
            continue;
        }
        if (starts_with(t, "## ")) where = Other;
        switch (where) {
            case Coach: coaching += line + "\n"; break;
            case Lessons:
                if (starts_with(t, "- ")) m.lessons.emplace_back(t.substr(2));
                break;
            case Other: m.other += line + "\n"; break;
        }
    }
    m.coaching = std::string(trim(coaching));
    m.other = std::string(trim(m.other));
    return m;
}

std::string render_memory(const Memory& m) {
    std::string out;
    if (!m.other.empty()) out += m.other + "\n\n";
    if (!m.coaching.empty()) out += std::string(kCoachHead) + "\n\n" + m.coaching + "\n\n";
    if (!m.lessons.empty()) {
        out += std::string(kLessonsHead) + "\n\n";
        for (const auto& l : m.lessons) out += "- " + l + "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out.empty() ? out : out + "\n";
}

Memory load_memory(const Project& project, const std::string& handle) {
    auto text = try_read_file(memory_path(project, handle));
    return text ? parse_memory(*text) : Memory{};
}

void save_memory(const Project& project, const std::string& handle, const Memory& m) {
    write_file_atomic(memory_path(project, handle), render_memory(m));
}

std::string memory_section(const Project& project, const std::string& handle, std::size_t limit) {
    Memory m = load_memory(project, handle);
    if (m.coaching.empty() && m.lessons.empty() && m.other.empty()) return "";
    std::string head;
    if (!m.other.empty()) head += m.other + "\n\n";
    if (!m.coaching.empty()) head += "From the coach:\n\n" + m.coaching + "\n\n";
    if (head.size() > limit) head = head.substr(0, limit) + "\n[cut]\n\n";
    // The newest lessons that fit.
    std::vector<std::string> kept;
    std::size_t room = limit > head.size() ? limit - head.size() : 0;
    for (auto it = m.lessons.rbegin(); it != m.lessons.rend(); ++it) {
        if (it->size() + 3 > room) break;
        room -= it->size() + 3;
        kept.push_back(*it);
    }
    std::string out = head;
    if (!kept.empty()) {
        out += "What you learned before (newest last):\n\n";
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) out += "- " + *it + "\n";
    }
    return out;
}

std::vector<std::string> add_lessons(const Project& project, const std::string& handle, const std::string& task_id,
                                     const std::vector<std::string>& lessons, const std::string& date, std::size_t keep) {
    if (lessons.empty()) return {};
    Memory m = load_memory(project, handle);
    std::vector<std::string> added;
    for (const std::string& l : lessons) {
        std::string text;
        for (const auto& line : split_lines(l))
            if (!trim(line).empty()) text += (text.empty() ? "" : " ") + std::string(trim(line));
        if (text.empty()) continue;
        if (text.size() > 600) text = first_line(text, 600);
        std::string entry = date + (task_id.empty() ? "" : " ^" + task_id) + ": " + text;
        // The same lesson again: kept once, with its newest date.
        bool same = false;
        for (auto& old : m.lessons) {
            auto colon = old.find(": ");
            if (colon != std::string::npos && similar_text(old.substr(colon + 2), text, 0.8)) {
                old = entry;
                same = true;
            }
        }
        if (!same) m.lessons.push_back(entry);
        added.push_back(entry);
    }
    while (m.lessons.size() > keep) m.lessons.erase(m.lessons.begin());
    save_memory(project, handle, m);
    return added;
}

void set_coaching(const Project& project, const std::string& handle, const std::string& text) {
    Memory m = load_memory(project, handle);
    m.coaching = std::string(trim(text));
    save_memory(project, handle, m);
}

}  // namespace myteam
