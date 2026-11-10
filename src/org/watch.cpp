// What needs you: operator tasks, the coach's messages, unreleased tool fixes, a waiting or
// stopped org (org.hpp, "what needs you").
#include <ctime>

#include "myorg/org.hpp"
#include "myteam/blockers.hpp"
#include "myteam/coach.hpp"
#include "myteam/plan.hpp"
#include "myteam/process.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"

namespace myorg {

using taskml::Json;
using taskml::TaskStatus;

namespace {

fs::path watch_path(const fs::path& org) { return org / ".myorg" / "watch.json"; }

std::optional<std::time_t> utc_time(const std::string& s) {
    std::tm tm{};
    if (s.size() < 19 || !strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return std::nullopt;
    return timegm(&tm);
}

/// The last tick myorg logged, or null.
Json last_tick(const fs::path& org) {
    auto text = myteam::try_read_file(org / ".myorg" / "log" / "ticks.jsonl");
    if (!text) return nullptr;
    auto lines = myteam::split_lines(*text);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (myteam::trim(*it).empty()) continue;
        try {
            return Json::parse(*it);
        } catch (...) {
            return nullptr;
        }
    }
    return nullptr;
}

std::string str(const Json& j, const char* k) {
    const Json* v = j.is_object() ? j.find(k) : nullptr;
    return v && v->is_string() ? v->as_string() : std::string();
}

Json strings(const std::set<std::string>& s) {
    Json a = Json::array();
    for (const auto& x : s) a.push_back(x);
    return a;
}

std::set<std::string> strings_of(const Json* j) {
    std::set<std::string> out;
    if (j && j->is_array())
        for (const Json& x : j->as_array())
            if (x.is_string()) out.insert(x.as_string());
    return out;
}

/// The units with writes: (tools maintained in a worktree) and the ids of their experts' tasks
/// the Gatekeeper accepted.
std::map<std::string, std::vector<const myteam::TaskRef*>> accepted_tool_work(const fs::path& org, const myteam::Plan& plan) {
    std::map<std::string, std::vector<const myteam::TaskRef*>> out;
    OrgSpec spec;
    try {
        spec = load_org_spec(org);
    } catch (...) {
        return out;
    }
    myteam::Project p = myteam::Project::open(org);
    auto team_dir = myteam::find_team_dir(p.config().team, org);
    if (!team_dir) return out;
    myteam::Team team;
    try {
        team = myteam::load_team(*team_dir, p.team_overlay());
    } catch (...) {
        return out;
    }
    for (const Unit& u : spec.units) {
        if (u.writes.empty()) continue;
        std::set<std::string> experts;
        for (const auto& m : team.members)
            if (!m.unit.empty() && (m.unit == u.dir || m.unit == u.handle)) experts.insert(m.handle);
        for (const myteam::TaskRef& r : plan.tasks()) {
            if (!r.task->id || !r.task->assignee || r.task->status != TaskStatus::Completed) continue;
            if (!experts.count(myteam::normalize_handle(*r.task->assignee))) continue;
            bool accepted = false;
            for (const auto& n : r.task->notes) accepted = accepted || myteam::starts_with(n, "Accepted by the Gatekeeper");
            if (accepted) out[u.handle].push_back(&r);
        }
    }
    return out;
}

}  // namespace

WatchState load_watch_state(const fs::path& org) {
    WatchState s;
    auto text = myteam::try_read_file(watch_path(org));
    if (!text) return s;
    try {
        Json j = Json::parse(*text);
        s.notified = strings_of(j.find("notified"));
        s.watched = strings_of(j.find("watched"));
        if (const Json* r = j.find("released"); r && r->is_object())
            for (const auto& [unit, ids] : r->as_object()) s.released[unit] = strings_of(&ids);
    } catch (...) {
    }
    return s;
}

void save_watch_state(const fs::path& org, const WatchState& s) {
    Json j = Json::object();
    j.set("notified", strings(s.notified));
    j.set("watched", strings(s.watched));
    Json r = Json::object();
    for (const auto& [unit, ids] : s.released) r.set(unit, strings(ids));
    j.set("released", r);
    myteam::write_file_atomic(watch_path(org), j.dump(2) + "\n");
}

std::vector<Need> org_needs(const fs::path& org, int stale_minutes) {
    std::vector<Need> out;
    myteam::Project p = myteam::Project::open(org);
    const WatchState ws = load_watch_state(org);
    if (fs::exists(p.plan_path())) {
        myteam::Plan plan = myteam::Plan::load(p.plan_path());
        for (const myteam::TaskRef& r : plan.tasks()) {
            if (!myteam::is_operator_task(*r.task) || myteam::is_finished(r.task->status)) continue;
            out.push_back({"operator", "operator:" + r.key(),
                           "an operator task waits for you: " + r.key() + " " + myteam::first_line(r.task->description, 160)});
        }
        for (const auto& [unit, tasks] : accepted_tool_work(org, plan)) {
            auto it = ws.released.find(unit);
            for (const myteam::TaskRef* r : tasks) {
                const std::string id = *r->task->id;
                if (it != ws.released.end() && it->second.count(id)) continue;
                out.push_back({"release", "release:" + unit + ":^" + id,
                               unit + ": ^" + id + " is accepted and not released (release it, then myorg released " +
                                   unit + "): " + myteam::first_line(r->task->description, 120)});
            }
        }
    }
    auto conv = myteam::load_conversation(p);
    if (!conv.empty() && conv.back().who == "coach")
        out.push_back({"customer", "customer:" + conv.back().when,
                       "the coach wrote to you (myteam say): " + myteam::first_line(conv.back().text, 160)});
    const Json tick = last_tick(org);
    if (!tick.is_null()) {
        const std::string when = str(tick, "time");
        const Json* w = tick.find("waiting");
        if (w && w->is_bool() && w->as_bool() && !str(tick, "waiting_for").empty())
            out.push_back({"waiting", "waiting:" + when, "myorg waits: " + str(tick, "waiting_for")});
        if (stale_minutes > 0)
            if (auto t = utc_time(when); t && std::time(nullptr) - *t > stale_minutes * 60)
                out.push_back({"stopped", "stopped:" + when,
                               "no tick since " + when + ": is myorg run still running? (myorg run)"});
    }
    return out;
}

std::vector<Need> new_needs(const std::vector<Need>& now, std::set<std::string>& seen) {
    std::vector<Need> fresh;
    std::set<std::string> keys;
    for (const Need& n : now) {
        keys.insert(n.key);
        if (!seen.count(n.key)) fresh.push_back(n);
    }
    seen = keys;  // what is no longer needed is forgotten: it is new if it comes back
    return fresh;
}

std::vector<std::string> mark_released(const fs::path& org, const std::string& unit) {
    std::vector<std::string> ids;
    myteam::Project p = myteam::Project::open(org);
    if (!fs::exists(p.plan_path())) return ids;
    myteam::Plan plan = myteam::Plan::load(p.plan_path());
    auto work = accepted_tool_work(org, plan);
    WatchState ws = load_watch_state(org);
    for (const myteam::TaskRef* r : work[unit])
        if (ws.released[unit].insert(*r->task->id).second) ids.push_back(*r->task->id);
    save_watch_state(org, ws);
    return ids;
}

std::vector<std::string> notify_needs(const fs::path& org, const std::vector<Need>& needs) {
    std::vector<std::string> problems;
    const OrgConfig cfg = load_org_config(org);
    if (cfg.notify.empty()) return problems;
    for (const Need& n : needs) {
        myteam::ProcessOptions o;
        o.argv = {"sh", "-c", cfg.notify, "myorg-notify", n.text};
        o.cwd = org;
        o.env = {{"MYORG_NEED_KIND", n.kind}, {"MYORG_NEED_KEY", n.key}, {"MYORG_NEED_TEXT", n.text}};
        o.timeout_seconds = 30;
        auto r = myteam::run_process(o, [](std::string_view) {});
        if (r.exit_code != 0) problems.push_back("notify (" + n.kind + ") exited " + std::to_string(r.exit_code));
    }
    return problems;
}

}  // namespace myorg
