// ORG.md, the units' views, config blocks, init and sync, the org state.
#include <algorithm>
#include <set>
#include <stdexcept>

#include "myorg/org.hpp"
#include "myteam/frontmatter.hpp"
#include "myteam/process.hpp"
#include "myteam/project.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"
#include "myteam/yaml.hpp"

namespace myorg {

using myteam::join;
using myteam::normalize_handle;
using myteam::read_file;
using myteam::starts_with;
using myteam::trim;
using myteam::try_read_file;
using myteam::write_file_atomic;
using taskml::Json;

namespace {

const char* kBegin = "# --- myorg: written by myorg sync from ORG.md; edit ORG.md, not this ---";
const char* kEnd = "# --- myorg: end ---";

bool known_mode(const std::string& m) {
    return std::find_if(std::begin(kModes), std::end(kModes), [&](const char* k) { return m == k; }) != std::end(kModes);
}

std::string yq(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

/// git in `dir`; its output.
myteam::ProcessResult git(const fs::path& dir, std::vector<std::string> args, std::string* out = nullptr) {
    myteam::ProcessOptions o;
    o.argv = {"git", "-C", dir.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.timeout_seconds = 120;
    std::string text;
    auto r = myteam::run_process(o, [&](std::string_view l) {
        text += l;
        text += "\n";
    });
    if (out) *out = text;
    return r;
}

bool is_repo_root(const fs::path& dir) { return fs::exists(dir / ".git"); }

}  // namespace

// --- interactions ---------------------------------------------------------------------

bool Interaction::involves(std::string_view u) const {
    return std::find(between.begin(), between.end(), u) != between.end();
}

std::string Interaction::other(std::string_view u) const {
    for (const auto& b : between)
        if (b != u) return b;
    return "";
}

std::string Interaction::mode_in(int milestone) const {
    if (until > 0 && milestone > until) return then == "none" ? "" : then;
    return mode;
}

const Unit* OrgSpec::find(std::string_view h) const {
    const std::string n = normalize_handle(h);
    for (const Unit& u : units)
        if (u.handle == n) return &u;
    return nullptr;
}

// --- ORG.md ---------------------------------------------------------------------------

OrgSpec parse_org_spec(std::string_view text) {
    OrgSpec s;
    myteam::MarkdownFile f;
    try {
        f = myteam::parse_markdown_file(text);
    } catch (const myteam::yaml::Error& e) {
        throw std::runtime_error(std::string("ORG.md: ") + e.what());
    }
    s.body = f.body;
    const auto& m = f.meta;
    try {
        s.name = m.get("name");
        if (m["rules"].is_sequence()) s.rules = m["rules"].as_strings();
        if (m["units"].is_mapping())
            for (const auto& [k, v] : m["units"].entries()) {
                Unit u;
                u.handle = normalize_handle(k);
                if (v.is_scalar()) u.team = v.text();
                else {
                    u.team = v.get("team");
                    u.type = v.get("type");
                    u.purpose = v.get("purpose");
                    u.dir = v.get("dir");
                    if (v["rules"].is_sequence()) u.rules = v["rules"].as_strings();
                    else if (v["rules"].is_scalar()) u.rules = {v["rules"].text()};
                    if (v["owns"].is_sequence()) u.owns = v["owns"].as_strings();
                    if (v["reads"].is_sequence()) u.reads = v["reads"].as_strings();
                    if (v["writes"].is_sequence()) u.writes = v["writes"].as_strings();
                }
                if (u.dir.empty()) u.dir = u.handle;
                s.units.push_back(u);
            }
        if (m["interactions"].is_sequence())
            for (const auto& v : m["interactions"].items()) {
                Interaction x;
                x.between = v["between"].is_null() ? std::vector<std::string>{} : v["between"].as_strings();
                for (auto& b : x.between) b = normalize_handle(b);
                x.mode = v.get("mode");
                x.provider = normalize_handle(v.get("provider"));
                if (v.has("via")) x.via = v["via"].as_strings();
                x.until = static_cast<int>(v.get_int("until", 0));
                x.then = v.get("then", x.then);
                x.note = v.get("note");
                s.interactions.push_back(x);
            }
    } catch (const myteam::yaml::Error& e) {
        throw std::runtime_error(std::string("ORG.md: ") + e.what());
    }
    return s;
}

OrgSpec load_org_spec(const fs::path& org_dir) {
    auto t = try_read_file(org_dir / "ORG.md");
    if (!t) throw std::runtime_error("no ORG.md in " + org_dir.string() + " (myorg init makes one)");
    return parse_org_spec(*t);
}

std::vector<std::string> check_org_spec(const OrgSpec& s) {
    std::vector<std::string> p;
    std::set<std::string> seen;
    for (const Unit& u : s.units) {
        if (u.handle.empty() || u.handle == "operator" || u.handle == "customer" || u.handle == "org")
            p.push_back("unit \"" + u.handle + "\": not a usable handle");
        if (!seen.insert(u.handle).second) p.push_back("unit " + u.handle + " is listed twice");
        for (const auto& o : u.owns) {
            const fs::path op(o);
            if (o.empty() || op.is_absolute() || o.find("..") != std::string::npos)
                p.push_back("unit " + u.handle + " owns \"" + o + "\": a path in the org (interfaces/events)");
            for (const Unit& w : s.units)
                if (&w != &u && std::find(w.owns.begin(), w.owns.end(), o) != w.owns.end() && u.handle < w.handle)
                    p.push_back(o + " is owned by both " + u.handle + " and " + w.handle);
        }
        for (const auto& w : u.writes)
            if (!(fs::path(w).is_absolute() || w.rfind("~/", 0) == 0))
                p.push_back("unit " + u.handle + " writes \"" + w + "\": a directory outside the org (absolute or ~/)");
        if (!u.type.empty() && std::find_if(std::begin(myteam::kTopologies), std::end(myteam::kTopologies),
                                            [&](const char* k) { return u.type == k; }) == std::end(myteam::kTopologies))
            p.push_back("unit " + u.handle + ": type " + u.type +
                        " is not stream-aligned, platform, complicated-subsystem or enabling");
        if (u.dir.find("..") != std::string::npos || (!u.dir.empty() && u.dir[0] == '/') || u.dir.empty() ||
            starts_with(u.dir, "."))
            p.push_back("unit " + u.handle + ": dir must be a directory in the org");
    }
    for (const Interaction& x : s.interactions) {
        const std::string what = "interaction " + join(x.between, "–");
        if (x.between.size() != 2) p.push_back(what + ": between needs two units");
        for (const auto& b : x.between)
            if (!s.find(b)) p.push_back(what + ": no unit " + b);
        if (!known_mode(x.mode)) p.push_back(what + ": mode " + x.mode + " is not collaboration, x-as-a-service or facilitating");
        if ((x.mode == "x-as-a-service" || x.mode == "facilitating") && !x.involves(x.provider))
            p.push_back(what + ": " + x.mode + " needs a provider, one of the two");
        if (x.until > 0 && x.then != "none" && !known_mode(x.then)) p.push_back(what + ": then " + x.then + " is not a mode");
        if (x.until > 0 && x.then == "x-as-a-service" && !x.involves(x.provider))
            p.push_back(what + ": becomes x-as-a-service after milestone " + std::to_string(x.until) + ": name the provider");
    }
    return p;
}

std::string render_org_spec(const OrgSpec& s) {
    std::string o = "---\n";
    o += "name: " + s.name + "\n";
    if (!s.rules.empty()) {
        o += "\n# What the org requires of every unit's work; each unit's rules: add its own.\nrules:\n";
        for (const auto& r : s.rules) o += "  - " + yq(r) + "\n";
    }
    o += "\n# The units: each a team from the catalog (myorg catalog) in its own directory and\n"
         "# git repository. type: stream-aligned | platform | complicated-subsystem | enabling\n";
    o += "units:\n";
    if (s.units.empty()) o.pop_back(), o += " {}\n";
    for (const Unit& u : s.units) {
        o += "  " + u.handle + ":\n";
        o += "    team: " + u.team + "\n";
        if (!u.type.empty()) o += "    type: " + u.type + "\n";
        if (!u.purpose.empty()) o += "    purpose: " + yq(u.purpose) + "\n";
        if (u.dir != u.handle) o += "    dir: " + u.dir + "\n";
        if (!u.owns.empty()) o += "    owns: [" + join(u.owns, ", ") + "]\n";
        if (!u.reads.empty()) o += "    reads: [" + join(u.reads, ", ") + "]\n";
        if (!u.writes.empty()) o += "    writes: [" + join(u.writes, ", ") + "]\n";
        if (!u.rules.empty()) {
            o += "    rules:\n";
            for (const auto& r : u.rules) o += "      - " + yq(r) + "\n";
        }
    }
    o += "\n# How units work together (Team Topologies): what each unit's experts see.\n"
         "# collaboration: each reads the other's repository; give it an end with until: N, the\n"
         "#   last milestone it lasts (then: the mode after it, x-as-a-service by default);\n"
         "# x-as-a-service: the consumer uses what the provider publishes (via:, default\n"
         "#   interfaces/<provider>); facilitating: the provider helps the other learn.\n"
         "# Units with no interaction see only the contracts (interfaces/).\n";
    o += "interactions:\n";
    if (s.interactions.empty()) o.pop_back(), o += " []\n";
    for (const Interaction& x : s.interactions) {
        o += "  - between: [" + join(x.between, ", ") + "]\n";
        o += "    mode: " + x.mode + "\n";
        if (!x.provider.empty()) o += "    provider: " + x.provider + "\n";
        if (!x.via.empty()) o += "    via: [" + join(x.via, ", ") + "]\n";
        if (x.until > 0) o += "    until: " + std::to_string(x.until) + "\n";
        if (x.until > 0 && x.then != "x-as-a-service") o += "    then: " + x.then + "\n";
        if (!x.note.empty()) o += "    note: " + yq(x.note) + "\n";
    }
    o += "---\n";
    std::string body = s.body;
    if (!body.empty() && body[0] != '\n') o += "\n";
    o += body;
    if (o.back() != '\n') o += "\n";
    return o;
}

OrgConfig load_org_config(const fs::path& org_dir) {
    OrgConfig c;
    auto t = try_read_file(org_dir / ".myorg" / "config.yaml");
    if (!t) return c;
    try {
        auto y = myteam::yaml::parse(*t);
        c.runner = y.get("runner");
        c.max_steps = static_cast<int>(y.get_int("max_steps", c.max_steps));
        c.rate_limit_wait = static_cast<int>(y.get_int("rate_limit_wait", c.rate_limit_wait));
        c.notify = y.get("notify");
    } catch (const myteam::yaml::Error& e) {
        throw std::runtime_error(".myorg/config.yaml: " + std::string(e.what()));
    }
    return c;
}

// --- views ----------------------------------------------------------------------------

UnitView unit_view(const OrgSpec& spec, std::string_view unit, int milestone, const fs::path& org_dir) {
    UnitView v;
    const Unit* me = spec.find(unit);
    if (!me) return v;
    std::set<std::string> visible;
    auto who = [&](const Unit& u) { return u.purpose.empty() ? "the " + u.team + " unit" : u.purpose; };
    for (const Interaction& x : spec.interactions) {
        if (!x.involves(me->handle)) continue;
        const std::string mode = x.mode_in(milestone);
        const Unit* o = spec.find(x.other(me->handle));
        if (!o || mode.empty()) continue;
        std::string line = "@" + o->handle + "'s unit (" + who(*o) + "): ";
        auto vias = [&] {
            std::vector<std::string> vv = x.via.empty() ? std::vector<std::string>{"interfaces/" + o->handle} : x.via;
            for (auto& p : vv) p = "../" + p;
            return join(vv, ", ");
        };
        if (mode == "collaboration") {
            visible.insert(o->handle);
            line += "collaboration. You work closely together: their repository is ../" + o->dir + " (read it)";
            if (x.until > 0) line += ", until milestone " + std::to_string(x.until);
            line += ".";
        } else if (mode == "x-as-a-service") {
            if (x.provider == o->handle) {
                for (const auto& p : x.via)
                    if (starts_with(p, o->dir + "/") || p == o->dir) visible.insert(o->handle);
                line += "x-as-a-service. Use what they provide (" + vias() +
                        ") as it is documented; report what it lacks (a finding, or a handoff to their expert).";
            } else {
                line += "x-as-a-service. They use what you provide: keep it documented, and fix what they report.";
            }
        } else if (mode == "facilitating") {
            if (x.provider == me->handle) {
                visible.insert(o->handle);
                line += "you facilitate them: help them learn and adopt; their repository is ../" + o->dir + " (read it).";
            } else {
                line += "they facilitate you: ask them for guidance, reviews and examples.";
            }
        }
        if (!x.note.empty()) line += " " + x.note;
        v.notes.push_back(line);
    }
    std::string up;
    for (const auto& part : fs::path(me->dir)) {
        (void)part;
        up += "../";
    }
    // A unit the ORG.md entry reads outright (a tool's team reading its users' work) is not hidden.
    for (const Unit& u : spec.units)
        for (std::string r : me->reads) {
            while (!r.empty() && r.back() == '/') r.pop_back();
            if (r == up + u.dir) visible.insert(u.handle);
        }
    for (const Unit& u : spec.units) {
        if (u.handle == me->handle || visible.count(u.handle)) continue;
        // Only what exists can be hidden; a unit made later is hidden on the next sync.
        if (!org_dir.empty() && !fs::is_directory(org_dir / u.dir)) continue;
        v.hide.push_back(up + u.dir);
    }
    v.readable.push_back(up + "interfaces");
    for (const Unit& u : spec.units)
        if (visible.count(u.handle)) v.readable.push_back(up + u.dir);
    for (const auto& o : me->owns) v.writable.push_back(up + o);
    v.readable.insert(v.readable.end(), me->reads.begin(), me->reads.end());
    v.writable.insert(v.writable.end(), me->writes.begin(), me->writes.end());
    v.rules = spec.rules;
    v.rules.insert(v.rules.end(), me->rules.begin(), me->rules.end());
    return v;
}

namespace {
std::string yaml_list(const std::vector<std::string>& v) {
    std::string b = "[";
    for (std::size_t i = 0; i < v.size(); ++i) b += (i ? ", " : "") + yq(v[i]);
    return b + "]";
}
}  // namespace

std::string unit_config_block(const UnitView& v) {
    std::string b;
    b += "sandbox_hide: " + yaml_list(v.hide) + "\n";
    b += "writable: " + yaml_list(v.writable) + "\n";
    b += "readable: " + yaml_list(v.readable) + "\n";
    b += "rules:";
    if (v.rules.empty()) b += " []";
    for (const auto& r : v.rules) b += "\n  - " + yq(r);
    b += "\n";
    b += "notes:";
    if (v.notes.empty()) b += " []";
    for (const auto& n : v.notes) b += "\n  - " + yq(n);
    b += "\n";
    return b;
}

std::string org_config_block(const OrgSpec& spec) {
    // The coach and the Gatekeeper read the units; nothing at the org's level writes in them.
    std::vector<std::string> ro;
    for (const Unit& u : spec.units) ro.push_back(u.dir);
    for (const char* p : {".myorg/config.yaml", ".myorg/state.json", ".myorg/log"}) ro.push_back(p);
    return "readonly: " + yaml_list(ro) + "\n";
}

std::string replace_managed_block(std::string_view config, std::string_view block) {
    std::string text(config);
    const auto lines = myteam::split_lines(text);
    bool in = false;
    std::vector<std::string> outside;
    for (const auto& l : lines) {
        if (trim(l) == kBegin) {
            in = true;
            continue;
        }
        if (in && trim(l) == kEnd) {
            in = false;
            continue;
        }
        if (!in) outside.push_back(l);
    }
    // Keys of the block that the rest also sets would be a duplicate.
    std::set<std::string> keys;
    for (const auto& l : myteam::split_lines(block))
        if (!l.empty() && l[0] != ' ' && l[0] != '#' && l[0] != '-')
            if (auto c = l.find(':'); c != std::string::npos) keys.insert(l.substr(0, c));
    for (const auto& l : outside)
        if (!l.empty() && l[0] != ' ' && l[0] != '#')
            if (auto c = l.find(':'); c != std::string::npos && keys.count(l.substr(0, c)))
                throw std::runtime_error("\"" + l.substr(0, c) + ":\" is myorg's (from ORG.md): take it out of config.yaml");
    while (!outside.empty() && trim(outside.back()).empty()) outside.pop_back();
    std::string out = join(outside, "\n");
    out += "\n\n";
    out += kBegin;
    out += "\n";
    out += block;
    if (!block.empty() && block.back() != '\n') out += "\n";
    out += kEnd;
    out += "\n";
    return out;
}

// --- state ----------------------------------------------------------------------------

OrgState load_org_state(const fs::path& org_dir) {
    OrgState s;
    auto t = try_read_file(org_dir / ".myorg" / "state.json");
    if (!t) return s;
    try {
        Json j = Json::parse(*t);
        auto num = [&](const char* k, int d) {
            const Json* v = j.find(k);
            return v && v->is_number() ? static_cast<int>(v->as_number()) : d;
        };
        s.milestone = std::max(1, num("milestone", 1));
        s.ticks = num("ticks", 0);
    } catch (const std::exception&) {
    }
    return s;
}

void save_org_state(const fs::path& org_dir, const OrgState& s) {
    Json j = Json::object();
    j.set("milestone", s.milestone);
    j.set("ticks", s.ticks);
    write_file_atomic(org_dir / ".myorg" / "state.json", j.dump(2) + "\n");
}

// --- init and sync --------------------------------------------------------------------

namespace {

void ensure_lines(const fs::path& file, const std::vector<std::string>& wanted) {
    std::string t = try_read_file(file).value_or("");
    auto have = myteam::split_lines(t);
    std::string add;
    for (const auto& w : wanted)
        if (std::find(have.begin(), have.end(), w) == have.end()) add += w + "\n";
    if (add.empty()) return;
    if (!t.empty() && t.back() != '\n') t += "\n";
    write_file_atomic(file, t + add);
}

/// The managed block of `config` is `block`; whether it changed.
bool write_block(const fs::path& config, const std::string& block) {
    const std::string before = try_read_file(config).value_or("");
    const std::string after = replace_managed_block(before, block);
    if (after == before) return false;
    write_file_atomic(config, after);
    return true;
}

/// A config line `key: value`, set (replacing the key's line) or added.
void set_config_line(const fs::path& config, const std::string& key, const std::string& line) {
    std::string t = try_read_file(config).value_or("");
    auto lines = myteam::split_lines(t);
    bool set = false;
    for (auto& l : lines)
        if (starts_with(l, key + ":")) {
            l = line;
            set = true;
        }
    if (!set) lines.insert(lines.begin(), line);
    write_file_atomic(config, join(lines, "\n") + "\n");
}

std::string unit_brief(const OrgSpec& spec, const Unit& u) {
    std::string b = "# " + u.handle + "\n\n";
    b += "This repository is the unit **" + u.handle + "** of the org " + (spec.name.empty() ? "" : "**" + spec.name + "** ") +
         "(myorg): " + (u.purpose.empty() ? "the " + u.team + " unit" : u.purpose) + ".\n\n";
    b += "## The org\n\n"
         "The crew's experts for this unit work here; the other units are repositories next to this one\n"
         "(../<unit>), which you see only when your unit works closely with them. The contracts between\n"
         "units are in ../interfaces/: read them; change one only through its change rule. The coach\n"
         "gives you your task; what you need from another unit goes through a handoff or a blocker.\n\n";
    b += "## Purpose\n\n## Layout\n\n## Conventions\n\n## Build and test\n";
    return b;
}

}  // namespace

std::vector<std::string> init_org(const fs::path& dir, const InitOptions& o) {
    std::vector<std::string> did;
    if (o.team.empty() && (!fs::exists(dir / ".myteam" / "config.yaml") || o.force))
        throw std::runtime_error("the org needs a team with a coach and a Gatekeeper (-t TEAM; myteam teams lists them)");
    fs::create_directories(dir);
    if (!is_repo_root(dir)) {
        if (git(dir, {"init", "-q"}).exit_code != 0) throw std::runtime_error("git init failed in " + dir.string());
        did.push_back("git repository");
    }
    if (!fs::exists(dir / ".myteam" / "config.yaml") || o.force) {
        if (!myteam::find_team_dir(o.team, dir)) throw std::runtime_error("no team \"" + o.team + "\" (myteam teams)");
        myteam::init_project(dir, o.team, o.force);
        did.push_back("the crew's myteam project (team: " + o.team + ", methodology: coach)");
    }
    const fs::path cfg = dir / ".myteam" / "config.yaml";
    set_config_line(cfg, "methodology", "methodology: coach         # the coach decides, the Gatekeeper checks, the experts work");
    set_config_line(cfg, "brief", "brief: ORG.md               # the org: units, interactions, the system");
    if (!fs::exists(dir / "ORG.md") || o.force) {
        OrgSpec s;
        s.name = o.name.empty() ? fs::weakly_canonical(dir).filename().string() : o.name;
        s.body = "\n# " + s.name +
                 "\n\nWhat the org builds, for whom, and how its parts fit together. The coach and the\n"
                 "Gatekeeper read this; the experts read their unit's brief and ../interfaces/.\n\n"
                 "## System\n\n## Interfaces\n\nThe contracts between units are in `interfaces/` (one directory per\n"
                 "contract, with its owner and version).\n";
        write_file_atomic(dir / "ORG.md", render_org_spec(s));
        did.push_back("ORG.md");
    }
    fs::create_directories(dir / "interfaces");
    if (!fs::exists(dir / "interfaces" / "README.md"))
        write_file_atomic(dir / "interfaces" / "README.md",
                          "# Interfaces\n\nThe contracts between the org's units: one directory per contract\n"
                          "(`routes/`, `events/`, `config/`, ...), each with a README naming its owner unit, its\n"
                          "version and who uses it. Machine-readable files (CSV, YAML, SystemRDL, .dtsi) where a\n"
                          "tool can check them.\n");
    fs::create_directories(dir / ".myorg" / "log");
    if (!fs::exists(dir / ".myorg" / "config.yaml"))
        write_file_atomic(dir / ".myorg" / "config.yaml",
                          "# myorg settings\n"
                          "runner: \"\"            # every agent on one runner (claude, codex, vibe); empty: the members'\n"
                          "max_steps: 200        # coach steps per myorg run, at most\n"
                          "rate_limit_wait: 600  # seconds to wait out a rate limit (0: stop)\n"
                          "notify: \"\"            # run for each new need: e.g. notify-send myorg \\\"$1\\\" (myorg watch)\n");
    ensure_lines(dir / ".gitignore", {".myorg/log/", ".myorg/state.json", ".myteam/runs/", ".myteam/checkpoints.git/"});
    if (!fs::exists(dir / "PLAN.md") || o.force)
        write_file_atomic(dir / "PLAN.md", "---\nproject: " + fs::weakly_canonical(dir).filename().string() +
                                               "\n---\n\n# The org\n\n## Work\n\n");
    did.push_back(".myorg/ and interfaces/");
    return did;
}

SyncResult sync_org(const fs::path& org, int milestone, bool dry) {
    SyncResult r;
    OrgSpec spec = load_org_spec(org);
    r.problems = check_org_spec(spec);
    if (!r.problems.empty()) return r;
    for (const Unit& u : spec.units) {
        const fs::path d = org / u.dir;
        if (fs::exists(d / ".myteam" / "config.yaml")) continue;
        if (dry) {
            r.created.push_back(u.handle + " (would be created: " + u.dir + "/)");
            continue;
        }
        fs::create_directories(d);
        if (!is_repo_root(d) && git(d, {"init", "-q"}).exit_code != 0) {
            r.problems.push_back("unit " + u.handle + ": git init failed");
            continue;
        }
        // The unit's experts are the crew's: the unit's config holds only its boundaries.
        write_file_atomic(d / ".myteam" / "config.yaml", "# The unit " + u.handle + " of the org: its boundaries.\n");
        write_file_atomic(d / ".myteam" / "project.md", unit_brief(spec, u));
        ensure_lines(d / ".gitignore", {".myteam/checkpoints.git/"});
        write_block(d / ".myteam" / "config.yaml", unit_config_block(unit_view(spec, u.handle, milestone, org)));
        git(d, {"add", "-A"});
        git(d, {"-c", "user.name=myorg", "-c", "user.email=myorg@myteam.invalid", "commit", "-q", "-m",
                "The unit " + u.handle + " of the org"});
        // A submodule of the org, its repository staying where it is.
        std::string out;
        if (git(org, {"-c", "protocol.file.allow=always", "submodule", "add", "./" + u.dir, u.dir}, &out).exit_code != 0)
            r.notes.push_back("unit " + u.handle + ": not added as a submodule (" + std::string(trim(out)) + ")");
        r.created.push_back(u.handle + " (" + u.dir + "/)");
    }
    if (dry) return r;
    for (const Unit& u : spec.units) {
        const fs::path cfg = org / u.dir / ".myteam" / "config.yaml";
        if (!fs::exists(cfg)) continue;
        // A unit that is a plain directory of the org repository gives its experts no git_commit.
        if (!is_repo_root(org / u.dir))
            r.notes.push_back("unit " + u.handle + " is not a git repository: its experts cannot commit (git init in " +
                              u.dir + "/, then add it as a submodule)");
        try {
            if (write_block(cfg, unit_config_block(unit_view(spec, u.handle, milestone, org))))
                r.updated.push_back(u.handle + ": what its experts see, and the org's rules");
        } catch (const std::exception& e) {
            r.problems.push_back(u.handle + "/.myteam/config.yaml: " + e.what());
        }
    }
    try {
        if (write_block(org / ".myteam" / "config.yaml", org_config_block(spec))) r.updated.push_back("the org: its units");
    } catch (const std::exception& e) {
        r.problems.push_back(".myteam/config.yaml: " + std::string(e.what()));
    }
    return r;
}

}  // namespace myorg
