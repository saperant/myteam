#include "myteam/contracts.hpp"

#include <cctype>
#include <sstream>

#include "myteam/frontmatter.hpp"
#include "myteam/plan.hpp"
#include "myteam/project.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;

namespace {

const char* kSeen = "contracts-seen";
const char* kChanged = "contracts-changed.md";

/// "interfaces/<name>" with no name character after it (interfaces/events, not interfaces/events2).
bool names_contract(std::string_view text, const std::string& name) {
    const std::string needle = "interfaces/" + name;
    for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1)) {
        const std::size_t end = at + needle.size();
        if (end == text.size()) return true;
        const unsigned char c = static_cast<unsigned char>(text[end]);
        if (!std::isalnum(c) && c != '-' && c != '_') return true;
    }
    return false;
}

bool task_names_contract(const taskml::Task& t, const std::string& name) {
    if (names_contract(t.description, name)) return true;
    for (const auto& n : t.notes)
        if (names_contract(n, name)) return true;
    for (const auto& c : t.criteria)
        if (names_contract(c.description, name)) return true;
    return false;
}

std::map<std::string, std::string> load_seen(const fs::path& p) {
    std::map<std::string, std::string> out;
    std::istringstream in(try_read_file(p).value_or(""));
    for (std::string line; std::getline(in, line);) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        out[line.substr(0, sp)] = line.substr(sp + 1);
    }
    return out;
}

}  // namespace

fs::path interfaces_dir(const fs::path& project_dir) {
    std::error_code ec;
    for (const fs::path& p : {project_dir / "interfaces", project_dir.parent_path() / "interfaces"})
        if (fs::is_directory(p, ec)) return p;
    return {};
}

std::map<std::string, std::string> contract_versions(const fs::path& interfaces) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (interfaces.empty() || !fs::is_directory(interfaces, ec)) return out;
    for (const auto& e : fs::directory_iterator(interfaces, ec)) {
        const fs::path readme = e.path() / "README.md";
        if (!e.is_directory() || !fs::is_regular_file(readme, ec)) continue;
        std::string v;
        try {
            MarkdownFile f = load_markdown_file(readme);
            if (f.meta.is_mapping() && f.meta["version"].is_scalar()) v = f.meta["version"].text();
        } catch (const std::exception&) {
            // A README whose front matter does not parse has no version we can compare.
        }
        out[e.path().filename().string()] = v;
    }
    return out;
}

std::vector<ContractChange> note_contract_changes(const fs::path& interfaces, const fs::path& state_dir,
                                                  const fs::path& plan_path) {
    std::vector<ContractChange> out;
    const auto now = contract_versions(interfaces);
    if (now.empty()) return out;
    const fs::path seen_path = state_dir / kSeen;
    const bool first = !fs::exists(seen_path);
    const auto seen = load_seen(seen_path);
    if (!first)
        for (const auto& [name, v] : now) {
            auto it = seen.find(name);
            const std::string was = it == seen.end() ? "" : it->second;
            if (it != seen.end() && was == v) continue;
            out.push_back({name, it == seen.end() ? "new" : was, v, {}});
        }
    if (!out.empty()) {
        Plan plan = Plan::load(plan_path);
        if (plan.ok()) {
            PlanEditor ed(plan);
            const std::string date = iso_timestamp().substr(0, 10);
            for (ContractChange& c : out)
                for (const TaskRef& r : plan.tasks()) {
                    if (is_finished(r.task->status) || !task_names_contract(*r.task, c.contract)) continue;
                    ed.add_note(r, "Contract interfaces/" + c.contract + " is version " + c.to + " now (was " + c.from +
                                       ", " + date + "): read its Changes section; check what this task uses from it");
                    c.tasks.push_back(r.key());
                }
            // Not committed (the plan changed meanwhile): the next look tries again.
            if (ed.changed() && !ed.commit().ok) return {};
        }
        std::string list = try_read_file(state_dir / kChanged).value_or("");
        for (const ContractChange& c : out) {
            list += "- interfaces/" + c.contract + ": version " + c.from + " -> " + c.to;
            if (c.tasks.empty()) list += "; no open task names it\n";
            else {
                list += "; tasks that read it:";
                for (const auto& k : c.tasks) list += " " + k;
                list += "\n";
            }
        }
        fs::create_directories(state_dir);
        write_file_atomic(state_dir / kChanged, list);
    }
    std::string text;
    for (const auto& [name, v] : now) text += name + " " + v + "\n";
    if (first || !out.empty() || seen.size() != now.size()) {
        fs::create_directories(state_dir);
        write_file_atomic(seen_path, text);
    }
    return out;
}

std::vector<ContractChange> note_contract_changes(const Project& project) {
    const fs::path i = interfaces_dir(project.dir());
    if (i.empty()) return {};
    return note_contract_changes(i, project.state_dir(), project.plan_path());
}

std::string contract_changes_for_standup(const fs::path& state_dir) {
    return try_read_file(state_dir / kChanged).value_or("");
}

void clear_contract_changes(const fs::path& state_dir) {
    std::error_code ec;
    fs::remove(state_dir / kChanged, ec);
}

}  // namespace myteam
