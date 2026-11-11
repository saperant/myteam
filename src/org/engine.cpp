// The org's tick: the units' views, a coach step, the milestone's commit.
#include "myorg/org.hpp"
#include "myteam/process.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"

namespace myorg {

using taskml::Json;

namespace {

int git(const fs::path& dir, std::vector<std::string> args) {
    myteam::ProcessOptions o;
    o.argv = {"git", "-C", dir.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.timeout_seconds = 120;
    return myteam::run_process(o, [](std::string_view) {}).exit_code;
}

void log_tick(const fs::path& org, const OrgState& st, const Tick& t) {
    Json j = Json::object();
    j.set("time", myteam::iso_timestamp());
    j.set("tick", st.ticks);
    j.set("milestone", st.milestone);
    j.set("waiting", t.waiting);
    j.set("waiting_for", t.waiting_for);
    j.set("rate_limited", t.rate_limited);
    j.set("reached_milestone", t.milestone);
    j.set("cost", t.cost);
    Json n = Json::array();
    for (const auto& x : t.notes) n.push_back(x);
    j.set("notes", n);
    Json p = Json::array();
    for (const auto& x : t.problems) p.push_back(x);
    j.set("problems", p);
    myteam::append_file(org / ".myorg" / "log" / "ticks.jsonl", j.dump() + "\n");
}

}  // namespace

Engine::Engine(fs::path org_dir, EngineOptions o) : org_(std::move(org_dir)), o_(std::move(o)) {}

std::vector<std::string> Engine::close_milestone(OrgState& st) {
    std::vector<std::string> did;
    const std::string n = std::to_string(st.milestone);
    OrgSpec spec = load_org_spec(org_);
    for (const Unit& u : spec.units) {
        const fs::path d = org_ / u.dir;
        if (!fs::exists(d / ".git")) continue;
        git(d, {"add", "-A"});
        if (git(d, {"diff", "--cached", "--quiet"}) != 0) {
            git(d, {"commit", "-q", "-m", "Milestone " + n + ": the unit's work at the crew's milestone"});
            did.push_back(u.handle + ": committed");
        }
    }
    git(org_, {"add", "-A"});
    if (git(org_, {"diff", "--cached", "--quiet"}) != 0) {
        git(org_, {"commit", "-q", "-m", "Milestone " + n + ": the plan, and every unit's revision"});
        did.push_back("org: committed, with every unit's revision");
    }
    if (git(org_, {"tag", "-f", "milestone-" + n}) == 0) did.push_back("tagged milestone-" + n);
    st.milestone += 1;
    return did;
}

Tick Engine::tick() {
    Tick t;
    OrgState st = load_org_state(org_);
    auto note = [&](std::string s) {
        if (o_.on_note) o_.on_note(s);
        t.notes.push_back(std::move(s));
    };
    // 1. The units' views, as ORG.md has them now (the coach may have changed it).
    SyncResult sr = sync_org(org_, st.milestone, o_.dry_run);
    for (const auto& c : sr.created) note("unit " + c);
    for (const auto& u : sr.updated) note("view: " + u);
    for (const auto& p : sr.problems) t.problems.push_back(p);
    if (!sr.problems.empty()) {
        t.waiting = true;
        t.waiting_for = "ORG.md has problems: " + sr.problems.front();
        return t;
    }

    // 2. A coach step on the org.
    myteam::Project project = myteam::Project::open(org_);
    auto team_dir = myteam::find_team_dir(project.config().team, org_);
    if (!team_dir) {
        t.waiting = true;
        t.waiting_for = "the org's team \"" + project.config().team + "\" is not found";
        return t;
    }
    myteam::Team team = myteam::load_team(*team_dir, project.team_overlay());
    const OrgConfig cfg = load_org_config(org_);
    myteam::RunnerFactory runners = o_.runners ? o_.runners : myteam::project_runners(project);
    myteam::Coach coach(project, team, runners);
    myteam::CoachOptions co;
    co.loop = o_.loop;
    if (co.loop.runner.empty()) co.loop.runner = cfg.runner;
    co.dry_run = o_.dry_run;
    myteam::CoachStep s = coach.step(co);
    for (const auto& n : s.notes) note(n);
    for (const auto& p : s.problems) t.problems.push_back(p);
    t.cost = s.cost;
    t.waiting = s.waiting;
    t.waiting_for = s.waiting_for;
    t.rate_limited = s.rate_limited;
    if (s.needs_human) {
        t.waiting = true;
        if (t.waiting_for.empty()) t.waiting_for = "the plan needs a person (myteam status)";
    }
    if (o_.dry_run) return t;

    // 3. What the units delivered to the customer this step, in the org's repository (the
    //    units' experts commit only in their own).
    if (auto units = commit_deliverables(org_); !units.empty())
        note("deliverables committed: " + myteam::join(units, ", "));

    // 4. A milestone: everything at it, committed and tagged.
    if (s.milestone) {
        t.milestone = true;
        for (const auto& d : close_milestone(st)) note(d);
        t.waiting = true;
        t.waiting_for = "milestone " + std::to_string(st.milestone - 1) + " reached: tell the coach what is next (myteam say)";
    }
    st.ticks++;
    save_org_state(org_, st);
    log_tick(org_, st, t);
    return t;
}

Json org_log(const fs::path& org, int max) {
    Json out = Json::object();
    Json ticks = Json::array();
    auto text = myteam::try_read_file(org / ".myorg" / "log" / "ticks.jsonl");
    if (text) {
        auto lines = myteam::split_lines(*text);
        int n = 0;
        for (auto it = lines.rbegin(); it != lines.rend() && n < max; ++it) {
            if (myteam::trim(*it).empty()) continue;
            try {
                ticks.push_back(Json::parse(*it));
                ++n;
            } catch (...) {
            }
        }
    }
    out.set("ticks", ticks);
    return out;
}

}  // namespace myorg
