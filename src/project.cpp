#include "myteam/project.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>

#include "myteam/util.hpp"
#include "myteam/yaml.hpp"

namespace myteam {

Project Project::open(const fs::path& dir) {
    Project p;
    p.dir_ = fs::absolute(dir).lexically_normal();
    if (p.dir_.has_filename() == false) p.dir_ = p.dir_.parent_path();
    auto text = try_read_file(p.config_path());
    if (!text) return p;
    p.has_config_ = true;
    ProjectConfig& c = p.config_;
    try {
        yaml::Node y = yaml::parse(*text);
        c.team = y.get("team", c.team);
        c.methodology = y.get("methodology", c.methodology);
        c.plan = y.get("plan", c.plan);
        c.brief = y.get("brief", c.brief);
        c.changelog = y.get("changelog", c.changelog);
        c.backlog = y.get("backlog", c.backlog);
        if (y.has("mcp")) c.mcp = y.get("mcp") == "true" || y.get("mcp") == "yes" || y.get("mcp") == "on";
        c.commits = y.get("commits", c.commits);
        if (c.commits != "tool" && c.commits != "off")
            throw yaml::Error("commits: expected tool or off", y["commits"].line);
        if (y.has("commit_hooks")) c.commit_hooks = y.get("commit_hooks") != "false" && y.get("commit_hooks") != "no";
        c.commit_hook_timeout = static_cast<int>(y.get_int("commit_hook_timeout", c.commit_hook_timeout));
        c.sandbox = y.get("sandbox", c.sandbox);
        if (c.sandbox != "auto" && c.sandbox != "bwrap" && c.sandbox != "none")
            throw yaml::Error("sandbox: expected auto, bwrap or none", y["sandbox"].line);
        if (y.has("sandbox_network")) c.sandbox_network = y.get("sandbox_network") != "false" && y.get("sandbox_network") != "no";
        if (y["sandbox_writable"].is_sequence()) c.sandbox_writable = y["sandbox_writable"].as_strings();
        if (y["sandbox_hide"].is_sequence()) c.sandbox_hide = y["sandbox_hide"].as_strings();
        if (y["rules"].is_sequence()) c.rules = y["rules"].as_strings();
        if (y["readable"].is_sequence()) c.readable = y["readable"].as_strings();
        if (y["writable"].is_sequence()) c.writable = y["writable"].as_strings();
        if (y["readonly"].is_sequence())
            for (std::string r : y["readonly"].as_strings()) {
                while (starts_with(r, "./")) r.erase(0, 2);
                while (!r.empty() && r.back() == '/') r.pop_back();
                if (r.empty() || r[0] == '/' || starts_with(r, "..")) throw yaml::Error("readonly: " + r + ": expected a directory in the project", y["readonly"].line);
                c.readonly.push_back(r);
            }
        if (y["external"].is_mapping())
            for (const auto& [k, v] : y["external"].entries()) {
                const std::string h = normalize_handle(k);
                if (h == "operator" || h == "customer")
                    throw yaml::Error("external: \"" + h + "\" is reserved", v.line);
                c.external[h] = v.is_scalar() ? v.text() : "";
            }
        if (y.has("guard")) {
            const yaml::Node& g = y["guard"];
            auto list = [&](const char* k) {
                const yaml::Node& n = g[k];
                return n.is_sequence() ? n.as_strings() : std::vector<std::string>{};
            };
            if (g.has("enabled")) c.guard.enabled = g.get("enabled") != "false" && g.get("enabled") != "no";
            if (g.has("checkpoints")) c.guard.checkpoints = g.get("checkpoints") != "false" && g.get("checkpoints") != "no";
            c.guard.deny_commands = list("deny_commands");
            c.guard.allow_commands = list("allow_commands");
            c.guard.protect = list("protect");
            c.guard.unprotect = list("unprotect");
            c.guard.allow_paths = list("allow_paths");
            c.guard.max_files = static_cast<int>(g.get_int("max_files", c.guard.max_files));
            c.guard.max_lines = static_cast<int>(g.get_int("max_lines", c.guard.max_lines));
        }
        if (y["services"].is_mapping()) {
            auto map_of = [](const yaml::Node& n) {
                std::map<std::string, std::string> m;
                if (n.is_mapping())
                    for (const auto& [k, v] : n.entries()) m[k] = v.as_string();
                return m;
            };
            for (const auto& [name, n] : y["services"].entries()) {
                ServiceSpec s;
                s.name = name;
                s.image = n.get("image");
                if (n["ports"].is_sequence()) s.ports = n["ports"].as_strings();
                s.env = map_of(n["env"]);
                s.files = map_of(n["files"]);
                s.data = n.get("data");
                if (n["command"].is_sequence()) s.command = n["command"].as_strings();
                else if (n.has("command")) s.command = {"sh", "-c", n.get("command")};
                s.network = n.get("network");
                s.agent_env = map_of(n["agent_env"]);
                s.description = n.get("description");
                c.services.push_back(std::move(s));
            }
        }
        if (y["services_podman"].is_sequence()) c.services_podman = y["services_podman"].as_strings();
        if (y["web"].is_sequence()) c.web = y["web"].as_strings();
        if (y["notes"].is_sequence()) c.notes = y["notes"].as_strings();
        if (y.has("commands")) {
            const yaml::Node& cm = y["commands"];
            c.commands.clear();
            if (cm.is_sequence()) {
                c.commands = cm.as_strings();
            } else {
                std::string v = y.get("commands");
                if (v == "all") c.commands = {"all"};
                else if (v != "none" && !v.empty()) c.commands = {v};
            }
        }
        c.brief_limit = static_cast<std::size_t>(y.get_int("brief_limit", static_cast<long long>(c.brief_limit)));
        if (y.has("permission")) {
            auto perm = permission_from_string(y.get("permission"));
            if (!perm) throw yaml::Error("permission: expected read-only, edit or full", y["permission"].line);
            c.permission = *perm;
        }
        if (y.has("coach")) {
            const yaml::Node& co = y["coach"];
            c.coach.tasks_per_step = static_cast<int>(co.get_int("tasks_per_step", c.coach.tasks_per_step));
            c.coach.acceptances_per_step = static_cast<int>(co.get_int("acceptances_per_step", c.coach.acceptances_per_step));
            c.coach.max_rejections = static_cast<int>(co.get_int("max_rejections", c.coach.max_rejections));
            c.coach.digest_tasks = static_cast<int>(co.get_int("digest_tasks", c.coach.digest_tasks));
            c.coach.memory_limit = static_cast<std::size_t>(co.get_int("memory_limit", static_cast<long long>(c.coach.memory_limit)));
            c.coach.acceptance_turns = static_cast<int>(co.get_int("acceptance_turns", c.coach.acceptance_turns));
            c.coach.step_cost = co.get_double("step_cost", c.coach.step_cost);
            c.coach.quiet_steps = static_cast<int>(co.get_int("quiet_steps", c.coach.quiet_steps));
        }
        const yaml::Node& l = y["limits"];
        c.limits.timeout_seconds = static_cast<int>(l.get_int("timeout", c.limits.timeout_seconds));
        c.limits.max_turns = static_cast<int>(l.get_int("max_turns", c.limits.max_turns));
        c.limits.max_cost_usd = l.get_double("max_cost", c.limits.max_cost_usd);
        for (const auto& [id, r] : y["runners"].entries()) {
            RunnerSettings s;
            s.command = r.get("command");
            s.isolate = r.get_bool("isolate", false);
            s.slim = r.get_bool("slim", true);
            s.args = r["args"].as_strings();
            for (const auto& [k, v] : r["env"].entries()) s.env[k] = v.text();
            c.runners[id] = std::move(s);
        }
    } catch (const yaml::Error& e) {
        throw std::runtime_error(p.config_path().string() + ": " + e.what());
    }
    return p;
}

RunnerSettings Project::runner_settings(const std::string& runner) const {
    auto it = config_.runners.find(runner);
    return it == config_.runners.end() ? RunnerSettings{} : it->second;
}

namespace {

// A short outline when the project has no brief: its name and top level.
std::string generated_brief(const fs::path& dir) {
    static const std::set<std::string> skip = {".git", ".myteam", "node_modules", "build", ".venv", "__pycache__"};
    std::vector<std::string> entries;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (skip.count(name) || (name.size() > 1 && name[0] == '.')) continue;
        entries.push_back(e.is_directory() ? name + "/" : name);
    }
    std::sort(entries.begin(), entries.end());
    std::string out = "Project directory: " + dir.filename().string() + "\n\nTop level:\n";
    std::size_t shown = 0;
    for (const std::string& e : entries) {
        if (++shown > 40) {
            out += "- ... (" + std::to_string(entries.size() - 40) + " more)\n";
            break;
        }
        out += "- " + e + "\n";
    }
    out += "\n(No project brief yet: describe the project in .myteam/project.md.)\n";
    return out;
}

}  // namespace

namespace {
std::vector<std::string> existing_dirs(const fs::path& base, const std::vector<std::string>& list) {
    std::vector<std::string> out;
    for (const std::string& r : list) {
        fs::path p(r);
        if (r == "~" || r.rfind("~/", 0) == 0) {
            const char* h = std::getenv("HOME");
            if (!h) continue;
            p = fs::path(h) / (r.size() > 2 ? r.substr(2) : "");
        } else if (p.is_relative()) {
            p = base / p;
        }
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (!ec && fs::is_directory(p, ec) && std::find(out.begin(), out.end(), p.string()) == out.end())
            out.push_back(p.string());
    }
    return out;
}
}  // namespace


std::vector<std::string> Project::read_paths() const {
    // What it writes, it reads.
    std::vector<std::string> all = config_.readable;
    all.insert(all.end(), config_.writable.begin(), config_.writable.end());
    return existing_dirs(dir_, all);
}

std::vector<std::string> Project::write_paths() const { return existing_dirs(dir_, config_.writable); }

std::string Project::brief() const {
    // The brief goes to every team member: it may not come from the
    // customer's side.
    fs::path b = fs::weakly_canonical(brief_path());
    fs::path c = fs::weakly_canonical(customer_dir());
    auto [ci, bi] = std::mismatch(c.begin(), c.end(), b.begin(), b.end());
    if (ci == c.end())
        throw std::runtime_error("brief: " + config_.brief + " is on the customer's side (" + customer_rel() +
                                 "); the team may not read it");
    auto text = try_read_file(brief_path());
    std::string out = text ? std::string(trim(*text)) : generated_brief(dir_);
    // A file the brief names in `backticks` that is not there yet is marked, so
    // nobody goes looking for it (the brief named scripts/verify.sh before it existed).
    {
        static const std::regex path(R"(`((?:[\w.-]+/)*[\w-]+\.(?:sh|py|md|txt|conf|ya?ml|toml|json|html|css|js|ts|sql|cfg|ini))`)");
        std::string marked;
        std::size_t last = 0;
        for (auto it = std::sregex_iterator(out.begin(), out.end(), path); it != std::sregex_iterator(); ++it) {
            const auto& m = *it;
            const std::string rel = m[1].str();
            std::error_code ec;
            const std::size_t end = static_cast<std::size_t>(m.position(0) + m.length(0));
            marked += out.substr(last, end - last);
            if (rel.find("..") == std::string::npos && !fs::exists(dir_ / rel, ec)) marked += " (not in the project yet)";
            last = end;
        }
        marked += out.substr(last);
        out = std::move(marked);
    }
    // The org's rules come last, and are never cut with the brief.
    std::string rules;
    if (!config_.rules.empty()) {
        rules = "\n\n## The org's rules\n\nThe org sets these; they win over the rest of the brief, your role's habits and\nany task note. Only the org changes them (ask the org through a blocker).\n\n";
        for (const auto& r : config_.rules) rules += "- " + r + "\n";
        while (!rules.empty() && rules.back() == '\n') rules.pop_back();
    }
    if (out.size() > config_.brief_limit) {
        // At a line, and saying where the rest is.
        std::size_t cut = out.rfind('\n', config_.brief_limit);
        out.resize(cut == std::string::npos || cut < config_.brief_limit / 2 ? config_.brief_limit : cut);
        out += "\n\n[The brief goes on: it is longer than " + std::to_string(config_.brief_limit) +
               " characters. Read the rest in " + config_.brief +
               " when you need it; keep the brief short, and put long material in docs/.]";
    }
    return out + rules;
}

InitResult init_project(const fs::path& dir, const std::string& team, bool force) {
    InitResult r;
    auto write = [&](const fs::path& path, const std::string& content) {
        if (fs::exists(path) && !force) {
            r.kept.push_back(path);
            return;
        }
        write_file_atomic(path, content);
        r.created.push_back(path);
    };
    const fs::path state = dir / ".myteam";
    write(state / "config.yaml",
          "# myteam project configuration\n"
          "team: " + team + "\n"
          "methodology: direct        # direct (a task round) | coach (the coach decides, the Gatekeeper checks)\n"
          "plan: PLAN.md\n"
          "brief: .myteam/project.md  # what every agent is told about the project\n"
          "changelog: CHANGELOG.md\n"
          "permission: edit           # read-only | edit | full, for task work\n"
          "commands: all              # shell commands agents may run with edit: all | none | [\"pytest:*\", ...]\n"
          "sandbox: auto              # run agents in bubblewrap when it works: auto | bwrap | none\n"
          "mcp: true                  # give agents the myteam plan tools (claude, codex)\n"
          "\n"
          "limits:\n"
          "  timeout: 1800            # seconds per agent run\n"
          "  max_turns: 0             # 0: no limit\n"
          "  max_cost: 0              # USD per run, where the CLI supports it\n"
          "\n"
          "runners:\n"
          "  claude:\n"
          "    command: claude\n"
          "    isolate: false         # true: --bare (no CLAUDE.md, memory or hooks; needs an API key)\n"
          "    slim: true             # leave out your own MCP servers, skills and settings (fewer tokens per turn)\n"
          "  codex:\n"
          "    command: codex\n"
          "  vibe:\n"
          "    command: vibe\n");
    write(state / "project.md",
          "# Project brief\n"
          "\n"
          "Every agent gets this file at the start of its work, so keep it short:\n"
          "what the project is, how it is laid out, and the conventions to follow.\n"
          "Put long material (requirements, designs, security rules) in docs/ and\n"
          "link it from here.\n"
          "\n"
          "## Purpose\n"
          "\n"
          "## Layout\n"
          "\n"
          "## Conventions\n"
          "\n"
          "## Build and test\n");
    write(dir / "PLAN.md",
          "---\n"
          "project: " + dir.filename().string() + "\n"
          "team: " + team + "\n"
          "---\n"
          "\n"
          "# " + dir.filename().string() + "\n"
          "\n"
          "## Backlog\n"
          "\n"
          "- [ ] Describe the project in .myteam/project.md #p1\n");
    return r;
}

}  // namespace myteam
