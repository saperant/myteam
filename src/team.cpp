#include "myteam/team.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>

#include "myteam/frontmatter.hpp"
#include "myteam/util.hpp"

namespace myteam {

ModelRef ModelRef::parse(std::string_view spec) {
    spec = trim(spec);
    ModelRef r;
    auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        r.runner = std::string(spec);
    } else {
        r.runner = std::string(trim(spec.substr(0, colon)));
        r.model = std::string(trim(spec.substr(colon + 1)));
    }
    return r;
}

const ModelRef& ModelProfile::for_priority(std::optional<int> priority) const {
    if (priority && *priority == 0 && premium) return *premium;
    if (priority && *priority >= 2 && economy) return *economy;
    return standard;
}

const Member* Team::find(std::string_view handle) const {
    std::string h = normalize_handle(handle);
    for (const Member& m : members)
        if (m.handle == h) return &m;
    return nullptr;
}

bool Team::is_external(std::string_view handle) const {
    return !handle.empty() && external.count(normalize_handle(handle)) > 0 && !find(handle);
}

const Member& Team::product_owner_member() const {
    if (const Member* m = find(product_owner)) return *m;
    if (members.empty()) throw std::runtime_error("team " + id + " has no members");
    return members.front();
}

namespace {

[[noreturn]] void fail(const fs::path& file, const std::string& what) {
    throw std::runtime_error(file.string() + ": " + what);
}

ModelProfile parse_models(const yaml::Node& n, const fs::path& file) {
    ModelProfile p;
    if (n.is_scalar()) {
        p.standard = ModelRef::parse(n.text());
        return p;
    }
    if (!n.is_mapping()) fail(file, "models: expected a model or a mapping of default/premium/economy");
    std::string standard = n.get("default");
    if (standard.empty()) fail(file, "models: missing default");
    p.standard = ModelRef::parse(standard);
    if (n.has("premium")) p.premium = ModelRef::parse(n.get("premium"));
    if (n.has("economy")) p.economy = ModelRef::parse(n.get("economy"));
    return p;
}

Member load_member(const fs::path& file, const std::optional<ModelProfile>& team_models) {
    MarkdownFile f = load_markdown_file(file);
    const yaml::Node& m = f.meta;
    Member member;
    member.source = file;
    member.handle = normalize_handle(m.get("handle", file.stem().string()));
    member.name = m.get("name", member.handle);
    member.role = m.get("role");
    member.summary = m.get("summary");
    member.unit = m.get("unit");
    member.skills = m["skills"].as_strings();
    if (m.has("models"))
        member.models = parse_models(m["models"], file);
    else if (team_models)
        member.models = *team_models;
    else
        fail(file, "no models (set models: in the member or in team.md)");
    for (const yaml::Node& h : m["handoffs"].items()) {
        HandoffRule r;
        r.when = h.get("when");
        r.to = normalize_handle(h.get("to"));
        r.note = h.get("note");
        if (r.when.empty() || r.to.empty()) fail(file, "handoffs: each needs when: and to:");
        member.handoffs.push_back(std::move(r));
    }
    member.prompt = std::string(trim(f.body));
    if (member.role.empty()) fail(file, "missing role");
    return member;
}

std::vector<std::pair<std::string, std::string>> word_map(const yaml::Node& n) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [k, v] : n.entries()) out.emplace_back(lower(k), normalize_handle(v.text()));
    return out;
}

// The file to read for `rel`: the overlay's when present.
fs::path pick(const fs::path& dir, const fs::path& overlay, const fs::path& rel) {
    if (!overlay.empty() && fs::exists(overlay / rel)) return overlay / rel;
    return dir / rel;
}

}  // namespace

Team load_team(const fs::path& dir, const fs::path& overlay) {
    Team team;
    team.dir = dir;
    if (!overlay.empty() && fs::is_directory(overlay)) team.overlay = overlay;

    const fs::path team_file = pick(dir, team.overlay, "team.md");
    if (!fs::exists(team_file)) throw std::runtime_error("not a team directory (no team.md): " + dir.string());
    MarkdownFile tf;
    try {
        tf = load_markdown_file(team_file);
    } catch (const yaml::Error& e) {
        throw std::runtime_error(e.what());
    }
    const yaml::Node& t = tf.meta;
    try {
        team.id = t.get("id", dir.filename().string());
        team.name = t.get("name", team.id);
        team.overview = std::string(trim(tf.body));
        team.product_owner = normalize_handle(t.get("product_owner", t.get("facilitator")));
        team.steward = normalize_handle(t.get("steward"));
        team.repairer = normalize_handle(t.get("repairer"));
        team.coach = normalize_handle(t.get("coach"));
        team.gatekeeper = normalize_handle(t.get("gatekeeper"));
        team.summary = t.get("summary");
        team.topology = t.get("topology");
        team.domain = t.get("domain");
        if (t.has("delivers")) team.delivers = t["delivers"].as_strings();
        if (!team.topology.empty() &&
            std::find(std::begin(kTopologies), std::end(kTopologies), team.topology) == std::end(kTopologies))
            throw yaml::Error("topology: expected stream-aligned, platform, complicated-subsystem or enabling",
                              t["topology"].line);

        std::optional<ModelProfile> team_models;
        if (t.has("models")) team_models = parse_models(t["models"], team_file);

        const yaml::Node& a = t["allocation"];
        team.allocation.roles = word_map(a["roles"]);
        team.allocation.keywords = word_map(a["keywords"]);
        const yaml::Node& w = a["weights"];
        AllocationWeights& wt = team.allocation.weights;
        wt.priority = static_cast<int>(w.get_int("priority", wt.priority));
        wt.deadline = static_cast<int>(w.get_int("deadline", wt.deadline));
        wt.blocker = static_cast<int>(w.get_int("blocker", wt.blocker));
        wt.skill = static_cast<int>(w.get_int("skill", wt.skill));
        wt.workload = static_cast<int>(w.get_int("workload", wt.workload));

        // Members: base files, replaced or extended by the overlay. The order
        // follows team.md's members: list when given, then file names.
        std::map<std::string, fs::path> files;
        for (const fs::path& d : {dir / "members", team.overlay.empty() ? fs::path() : team.overlay / "members"}) {
            if (d.empty() || !fs::is_directory(d)) continue;
            for (const auto& e : fs::directory_iterator(d))
                if (e.is_regular_file() && e.path().extension() == ".md") files[e.path().stem().string()] = e.path();
        }
        std::vector<std::string> order;
        for (const std::string& h : t["members"].as_strings()) {
            std::string n = normalize_handle(h);
            if (!files.count(n)) fail(team_file, "members: no file members/" + n + ".md");
            order.push_back(n);
        }
        for (const auto& [stem, path] : files)
            if (std::find(order.begin(), order.end(), stem) == order.end()) order.push_back(stem);
        for (const std::string& stem : order) team.members.push_back(load_member(files[stem], team_models));
    } catch (const yaml::Error& e) {
        throw std::runtime_error(team_file.string() + ": " + e.what());
    }

    if (team.members.empty()) fail(team_file, "the team has no members (members/*.md)");
    for (const Member& m : team.members)
        if (m.handle == "operator" || m.handle == "customer")
            fail(m.source, "\"" + m.handle + "\" is reserved (the person running myteam, the customer)");
    auto check = [&](const std::string& handle, const std::string& what, const fs::path& file) {
        if (!handle.empty() && !team.find(handle)) fail(file, what + ": no member \"" + handle + "\"");
    };
    check(team.product_owner, "product_owner", team_file);
    check(team.steward, "steward", team_file);
    check(team.repairer, "repairer", team_file);
    check(team.coach, "coach", team_file);
    check(team.gatekeeper, "gatekeeper", team_file);
    if (team.product_owner.empty() && !team.coach.empty()) team.product_owner = team.coach;
    if (team.product_owner.empty()) team.product_owner = team.members.front().handle;
    for (const auto& [word, h] : team.allocation.roles) check(h, "allocation.roles." + word, team_file);
    for (const auto& [word, h] : team.allocation.keywords) check(h, "allocation.keywords." + word, team_file);
    for (const Member& m : team.members)
        for (const HandoffRule& r : m.handoffs) check(r.to, "handoffs", m.source);
    return team;
}

std::vector<fs::path> team_search_path(const fs::path& project_dir) {
    std::vector<fs::path> out;
    if (!project_dir.empty()) out.push_back(project_dir / ".myteam" / "teams");
    if (const char* env = std::getenv("MYTEAM_TEAMS")) {
        std::string s = env;
        std::size_t start = 0;
        while (start <= s.size()) {
            std::size_t end = s.find(':', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        out.push_back(fs::path(xdg) / "myteam" / "teams");
    else if (const char* home = std::getenv("HOME"))
        out.push_back(fs::path(home) / ".config" / "myteam" / "teams");
    out.push_back(fs::path(MYTEAM_SOURCE_DIR) / "plugins" / "teams");
    return out;
}

std::optional<fs::path> find_team_dir(std::string_view name, const fs::path& project_dir) {
    fs::path direct(name);
    if (name.find('/') != std::string_view::npos && fs::exists(direct / "team.md")) return direct;
    for (const fs::path& d : team_search_path(project_dir))
        if (fs::exists(d / std::string(name) / "team.md")) return d / std::string(name);
    return std::nullopt;
}

}  // namespace myteam

namespace myteam {

std::vector<CatalogEntry> team_catalog(const fs::path& project_dir) {
    std::vector<CatalogEntry> out;
    std::set<std::string> seen;
    for (const fs::path& dir : team_search_path(project_dir)) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory() && fs::exists(e.path() / "team.md")) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& d : dirs) {
            const std::string name = d.filename().string();
            if (!seen.insert(name).second) continue;  // an earlier directory overrides it
            CatalogEntry c;
            c.name = name;
            c.dir = d;
            try {
                c.team = load_team(d);
            } catch (const std::exception& e) {
                c.error = e.what();
            }
            out.push_back(std::move(c));
        }
    }
    return out;
}

}  // namespace myteam
