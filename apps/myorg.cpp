// myorg: one crew across an organisation's units.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "myorg/org.hpp"
#include "myteam/coach.hpp"
#include "myteam/project.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"

using namespace myorg;

namespace {

const char* kUsage = R"(usage: myorg <command> [options]

commands:
  init      make an org here: git, the crew (myteam, methodology coach), ORG.md, interfaces/   -t TEAM [--name N] [--force]
  add       add a unit to ORG.md and set it up                    UNIT [-t TEAM] [--type T] [--purpose TEXT] [--dir D]
  link      an interaction between two units in ORG.md            A B --mode M [--provider U] [--via PATH] [--until N]
            M: collaboration, x-as-a-service, facilitating; --until N: it lasts through milestone N
  sync      make the org match ORG.md: new units, what each unit's experts see   [--dry-run]
  status    the milestone, the crew's work, what needs you, the cost so far
  run       work: sync, a coach step, a milestone's commit; again until it needs you   [-n TICKS] [--dry-run] [-q]
  log       the ticks myorg ran                                                       [-n MAX]
  needs     what needs you now: operator tasks, the coach's messages, unreleased tool fixes, a waiting or stopped org
  watch     wait until something new needs you, say it, run notify, exit (run it in the background)
                                                                       [--interval S] [--stale MIN] [--follow] [--all]
  released  the tool unit's accepted fixes are released (after you merged them)        UNIT
  json      what a UI needs (myrcp's myorg plugin)                                    overview | log [-n MAX]

options:
  -d, --directory DIR   the org (default: the current directory)

You are the customer: tell the coach what you want with myteam say -d ORG "...".
While myorg run works, keep myorg watch running: the crew goes on while something waits for you.
)";

struct Args {
    std::string command;
    std::string dir = ".";
    std::vector<std::string> positional;
    std::string name, team, type, purpose, unit_dir, mode, provider, via;
    int ticks = 0, until = 0, interval = 60, stale = 0;
    bool force = false, dry_run = false, quiet = false, follow = false, all = false;
};

[[noreturn]] void die(const std::string& m, int code = 1) {
    std::cerr << "myorg: " << m << "\n";
    std::exit(code);
}

Args parse(int argc, char** argv) {
    Args a;
    if (argc < 2) {
        std::cerr << kUsage;
        std::exit(2);
    }
    a.command = argv[1];
    auto value = [&](int& i) -> std::string {
        if (i + 1 >= argc) die(std::string("missing value for ") + argv[i], 2);
        return argv[++i];
    };
    auto number = [&](int& i) {
        std::string v = value(i);
        try {
            return std::stoi(v);
        } catch (...) {
            die("not a number: " + v, 2);
        }
    };
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-d" || s == "--directory") a.dir = value(i);
        else if (s == "--name") a.name = value(i);
        else if (s == "-t" || s == "--team") a.team = value(i);
        else if (s == "--type") a.type = value(i);
        else if (s == "--purpose") a.purpose = value(i);
        else if (s == "--dir") a.unit_dir = value(i);
        else if (s == "--mode") a.mode = value(i);
        else if (s == "--provider") a.provider = value(i);
        else if (s == "--via") a.via = value(i);
        else if (s == "--until") a.until = number(i);
        else if (s == "-n") a.ticks = number(i);
        else if (s == "--interval") a.interval = number(i);
        else if (s == "--stale") a.stale = number(i);
        else if (s == "--follow") a.follow = true;
        else if (s == "--all") a.all = true;
        else if (s == "--force") a.force = true;
        else if (s == "--dry-run") a.dry_run = true;
        else if (s == "-q" || s == "--quiet") a.quiet = true;
        else if (s == "-h" || s == "--help") {
            std::cout << kUsage;
            std::exit(0);
        } else if (!s.empty() && s[0] == '-') die("unknown option " + s, 2);
        else a.positional.push_back(s);
    }
    return a;
}

bool tty() { return ::isatty(1); }
std::string dim(const std::string& s) { return tty() ? "\033[2m" + s + "\033[0m" : s; }
std::string bold(const std::string& s) { return tty() ? "\033[1m" + s + "\033[0m" : s; }

fs::path org_dir(const Args& a) { return fs::weakly_canonical(fs::path(a.dir)); }

/// The myteam executable agents' MCP server and guard hook run: next to this one, else $PATH.
std::string myteam_exe() {
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        fs::path next = self.parent_path() / "myteam";
        if (fs::exists(next)) return next.string();
    }
    if (auto p = myteam::find_executable("myteam")) return p->string();
    return "";
}

void print_sync(const SyncResult& r) {
    for (const auto& c : r.created) std::cout << "new unit " << c << "\n";
    for (const auto& c : r.updated) std::cout << dim("updated " + c) << "\n";
    for (const auto& c : r.notes) std::cout << dim(c) << "\n";
    for (const auto& p : r.problems) std::cout << "problem: " << p << "\n";
}

int cmd_init(const Args& a) {
    const fs::path d = org_dir(a);
    InitOptions o;
    o.name = a.name;
    o.team = a.team;
    o.force = a.force;
    for (const auto& l : init_org(d, o)) std::cout << "made " << l << "\n";
    std::cout << "\nNext: the units (myorg add api --purpose \"...\"), the crew's experts (.myteam/team/members/,\n"
                 "each with a unit:), then tell the coach what you want: myteam say -d " << d.string() << " \"...\"\n";
    return 0;
}

void save_spec(const fs::path& org, const OrgSpec& s) {
    auto problems = check_org_spec(s);
    if (!problems.empty()) die(myteam::join(problems, "\n"));
    myteam::write_file_atomic(org / "ORG.md", render_org_spec(s));
}

int cmd_add(const Args& a) {
    if (a.positional.size() != 1) die("add needs UNIT", 2);
    const fs::path d = org_dir(a);
    OrgSpec s = load_org_spec(d);
    const std::string h = myteam::normalize_handle(a.positional[0]);
    if (s.find(h)) die("ORG.md has a unit " + h + " already");
    Unit u;
    u.handle = h;
    u.team = a.team;
    u.type = a.type;
    u.purpose = a.purpose;
    u.dir = a.unit_dir.empty() ? h : a.unit_dir;
    s.units.push_back(u);
    save_spec(d, s);
    std::cout << "ORG.md: unit " << h << "\n";
    print_sync(sync_org(d, load_org_state(d).milestone, a.dry_run));
    return 0;
}

int cmd_link(const Args& a) {
    if (a.positional.size() != 2 || a.mode.empty()) die("link needs A B --mode M", 2);
    const fs::path d = org_dir(a);
    OrgSpec s = load_org_spec(d);
    Interaction x;
    x.between = {myteam::normalize_handle(a.positional[0]), myteam::normalize_handle(a.positional[1])};
    x.mode = a.mode;
    x.provider = myteam::normalize_handle(a.provider);
    if (!a.via.empty()) x.via = {a.via};
    x.until = a.until;
    s.interactions.erase(std::remove_if(s.interactions.begin(), s.interactions.end(),
                                        [&](const Interaction& o) { return o.involves(x.between[0]) && o.involves(x.between[1]); }),
                         s.interactions.end());
    s.interactions.push_back(x);
    save_spec(d, s);
    std::cout << "ORG.md: " << x.between[0] << " and " << x.between[1] << ": " << x.mode << "\n";
    print_sync(sync_org(d, load_org_state(d).milestone, a.dry_run));
    return 0;
}

int cmd_sync(const Args& a) {
    const fs::path d = org_dir(a);
    SyncResult r = sync_org(d, load_org_state(d).milestone, a.dry_run);
    print_sync(r);
    if (r.created.empty() && r.updated.empty() && r.problems.empty()) std::cout << "the org matches ORG.md\n";
    return r.problems.empty() ? 0 : 1;
}

int cmd_status(const Args& a) {
    const fs::path d = org_dir(a);
    OrgSpec spec = load_org_spec(d);
    OrgState st = load_org_state(d);
    myteam::Project p = myteam::Project::open(d);
    myteam::CoachState cs = myteam::load_coach_state(p);
    char cost[48];
    std::snprintf(cost, sizeof cost, "$%.2f", cs.cost);
    std::cout << bold(spec.name.empty() ? d.filename().string() : spec.name) << "  ·  toward milestone " << st.milestone
              << "  ·  " << st.ticks << " ticks, " << cs.decisions << " coach decisions, " << cost << "\n";
    std::cout << "\n" << bold("Units") << "\n";
    for (const Unit& u : spec.units) {
        std::string line = "  " + u.handle + "  " + u.dir + "/";
        if (!u.purpose.empty()) line += "  " + dim(myteam::first_line(u.purpose, 90));
        std::cout << line << "\n";
    }
    std::cout << "\n" << bold("Interactions") << "\n";
    for (const Interaction& x : spec.interactions) {
        const std::string m = x.mode_in(st.milestone);
        std::cout << "  " << x.between[0] << " — " << x.between[1] << "  " << (m.empty() ? std::string("ended") : m)
                  << (x.provider.empty() ? "" : " (provider " + x.provider + ")")
                  << (x.until ? dim("  until milestone " + std::to_string(x.until)) : "") << "\n";
    }
    std::cout << "\n" << dim("The crew's work: myteam status -d " + d.string() + "; what the coach reads: myteam digest") << "\n";
    return 0;
}

int cmd_run(const Args& a) {
    const fs::path d = org_dir(a);
    const OrgConfig cfg = load_org_config(d);
    EngineOptions eo;
    eo.dry_run = a.dry_run;
    eo.on_note = [](const std::string& n) { std::cout << dim("· " + n) << "\n" << std::flush; };
    const std::string exe = myteam_exe();
    if (!exe.empty()) {
        eo.loop.mcp_command = {exe, "mcp"};
        eo.loop.guard_command = {exe, "guard"};
    } else {
        std::cerr << "myorg: no myteam executable found: agents run without the plan tools and the guard\n";
    }
    if (!a.quiet && !a.dry_run)
        eo.loop.on_event = [](const myteam::DispatchPlan& dp, const myteam::AgentEvent& e) {
            if (e.kind == myteam::AgentEvent::Kind::ToolCall)
                std::cout << dim("  @" + dp.member->handle + " " + e.tool + " " + myteam::first_line(e.text, 100)) << "\n";
            else if (e.kind == myteam::AgentEvent::Kind::Error)
                std::cout << "  @" << dp.member->handle << " error: " << e.text << "\n";
            std::cout.flush();
        };
    Engine engine(d, eo);
    const int max = a.ticks > 0 ? a.ticks : cfg.max_steps;
    double total = 0;
    for (int i = 0; i < max; ++i) {
        Tick t = engine.tick();
        total += t.cost;
        for (const auto& p : t.problems) std::cout << "problem: " << p << "\n";
        char buf[64];
        std::snprintf(buf, sizeof buf, "tick %d: $%.2f (this run $%.2f)", i + 1, t.cost, total);
        std::cout << dim(buf) << "\n";
        // What needs you, said once each, and the notify command for it.
        if (!a.dry_run) {
            WatchState ws = load_watch_state(d);
            auto fresh = new_needs(org_needs(d), ws.notified);
            save_watch_state(d, ws);
            for (const Need& n : fresh) std::cout << bold("NEEDS YOU: ") << n.text << "\n";
            for (const auto& p : notify_needs(d, fresh)) std::cout << "problem: " << p << "\n";
            std::cout.flush();
        }
        if (a.dry_run) break;
        if (t.rate_limited && cfg.rate_limit_wait > 0 && i + 1 < max) {
            std::cout << dim("rate limited: waiting " + std::to_string(cfg.rate_limit_wait) + " s") << "\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(cfg.rate_limit_wait));
            continue;
        }
        if (t.waiting) {
            std::cout << bold("waiting: ") << t.waiting_for << "\n";
            return 0;
        }
        if (fs::exists(d / ".myorg" / "stop")) {
            fs::remove(d / ".myorg" / "stop");
            std::cout << "stopped (.myorg/stop)\n";
            return 0;
        }
    }
    return 0;
}

/// What needs you now (myorg needs), or only what is new to the watch (myorg watch).
int cmd_needs(const Args& a) {
    const fs::path d = org_dir(a);
    auto now = org_needs(d, a.stale);
    if (now.empty()) std::cout << "Nothing needs you.\n";
    for (const Need& n : now) std::cout << n.kind << "  " << n.text << "\n";
    return 0;
}

/// Waits until something new needs you, prints it (one line each: KIND  text), runs the notify
/// command, and exits 0: run it in the background, and its exit is the notification. --follow
/// goes on (one line per new need) for a watcher reading lines; --all says what needs you already
/// on the first look too.
int cmd_watch(const Args& a) {
    const fs::path d = org_dir(a);
    const int stale = a.stale > 0 ? a.stale : 120;
    bool first = true;
    while (true) {
        WatchState ws = load_watch_state(d);
        std::vector<Need> fresh;
        try {
            fresh = new_needs(org_needs(d, stale), ws.watched);
        } catch (const std::exception& e) {
            fresh = {{"error", "error:" + std::string(e.what()), std::string("myorg watch: ") + e.what()}};
        }
        save_watch_state(d, ws);
        if (first && !a.all) fresh.erase(std::remove_if(fresh.begin(), fresh.end(), [](const Need& n) { return n.kind == "stopped"; }), fresh.end());
        first = false;
        if (!fresh.empty()) {
            for (const Need& n : fresh) std::cout << n.kind << "  " << n.text << "\n";
            std::cout.flush();
            for (const auto& p : notify_needs(d, fresh)) std::cerr << "myorg watch: " << p << "\n";
            if (!a.follow) return 0;
        }
        if (fs::exists(d / ".myorg" / "stop-watch")) {
            fs::remove(d / ".myorg" / "stop-watch");
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::seconds(std::max(5, a.interval)));
    }
}

int cmd_released(const Args& a) {
    if (a.positional.empty()) die("released needs UNIT", 2);
    const fs::path d = org_dir(a);
    auto ids = mark_released(d, myteam::normalize_handle(a.positional[0]));
    std::cout << a.positional[0] << ": " << ids.size() << " accepted task(s) marked released";
    for (const auto& id : ids) std::cout << " ^" << id;
    std::cout << "\n";
    return 0;
}

int cmd_log(const Args& a) {
    auto j = org_log(org_dir(a), a.ticks > 0 ? a.ticks : 20);
    const auto* ticks = j.find("ticks");
    if (!ticks || !ticks->is_array() || ticks->as_array().empty()) {
        std::cout << "No ticks yet.\n";
        return 0;
    }
    for (const auto& t : ticks->as_array()) std::cout << t.dump() << "\n";
    return 0;
}

taskml::Json strings_json(const std::vector<std::string>& v) {
    taskml::Json a = taskml::Json::array();
    for (const auto& x : v) a.push_back(x);
    return a;
}

/// What myrcp's myorg plugin reads: the org, its units and what each sees, the interactions
/// as they are now, the milestone, the crew's state.
int cmd_json(const Args& a) {
    const std::string what = a.positional.empty() ? "overview" : a.positional[0];
    const fs::path d = org_dir(a);
    taskml::Json out = taskml::Json::object();
    try {
        if (what == "log") {
            out = org_log(d, a.ticks > 0 ? a.ticks : 200);
        } else if (what == "overview") {
            OrgSpec spec = load_org_spec(d);
            OrgState st = load_org_state(d);
            out.set("dir", d.string());
            out.set("name", spec.name.empty() ? d.filename().string() : spec.name);
            out.set("milestone", st.milestone);
            out.set("ticks", st.ticks);
            out.set("rules", strings_json(spec.rules));
            myteam::Project p = myteam::Project::open(d);
            std::optional<myteam::Team> team;
            if (auto td = myteam::find_team_dir(p.config().team, d)) {
                try {
                    team = myteam::load_team(*td, p.team_overlay());
                } catch (const std::exception&) {
                }
            }
            taskml::Json units = taskml::Json::array();
            for (const Unit& u : spec.units) {
                taskml::Json j = taskml::Json::object();
                j.set("handle", u.handle);
                j.set("dir", u.dir);
                j.set("type", u.type);
                j.set("purpose", u.purpose);
                j.set("owns", strings_json(u.owns));
                j.set("reads", strings_json(u.reads));
                j.set("writes", strings_json(u.writes));
                j.set("rules", strings_json(u.rules));
                j.set("repository", fs::exists(d / u.dir / ".git"));
                UnitView v = unit_view(spec, u.handle, st.milestone, d);
                taskml::Json view = taskml::Json::object();
                view.set("hide", strings_json(v.hide));
                view.set("readable", strings_json(v.readable));
                view.set("writable", strings_json(v.writable));
                view.set("notes", strings_json(v.notes));
                j.set("view", view);
                taskml::Json experts = taskml::Json::array();
                if (team)
                    for (const auto& m : team->members)
                        if (m.unit == u.dir || m.unit == u.handle) experts.push_back(m.handle);
                j.set("experts", experts);
                units.push_back(j);
            }
            out.set("units", units);
            taskml::Json xs = taskml::Json::array();
            for (const Interaction& x : spec.interactions) {
                taskml::Json j = taskml::Json::object();
                j.set("between", strings_json(x.between));
                j.set("mode", x.mode);
                j.set("now", x.mode_in(st.milestone));
                j.set("provider", x.provider);
                j.set("via", strings_json(x.via));
                j.set("until", x.until);
                j.set("then", x.then);
                j.set("note", x.note);
                xs.push_back(j);
            }
            out.set("interactions", xs);
            // What the units deliver to the customer (shapes are additive).
            taskml::Json dl = taskml::Json::object();
            dl.set("dir", spec.deliverables);
            taskml::Json per = taskml::Json::object();
            for (const auto& [unit, files] : list_deliverables(d, spec)) per.set(unit, strings_json(files));
            dl.set("units", per);
            out.set("deliverables", dl);
            myteam::CoachState cs = myteam::load_coach_state(p);
            taskml::Json c = taskml::Json::object();
            c.set("decisions", cs.decisions);
            c.set("steps", cs.steps);
            c.set("cost_usd", cs.cost);
            c.set("objection", cs.objection);
            c.set("news", static_cast<int>(cs.news.size()));
            out.set("coach", c);
            out.set("stop_requested", fs::exists(d / ".myorg" / "stop"));
        } else {
            die("json: overview or log", 2);
        }
    } catch (const std::exception& e) {
        taskml::Json err = taskml::Json::object();
        err.set("error", e.what());
        std::cout << err.dump() << "\n";
        return 1;
    }
    std::cout << out.dump() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    try {
        if (a.command == "help" || a.command == "-h" || a.command == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (a.command == "version" || a.command == "--version") {
            std::cout << "myorg " << MYTEAM_VERSION << "\n";
            return 0;
        }
        if (a.command == "init") return cmd_init(a);
        if (a.command == "add") return cmd_add(a);
        if (a.command == "link") return cmd_link(a);
        if (a.command == "sync") return cmd_sync(a);
        if (a.command == "status") return cmd_status(a);
        if (a.command == "run") return cmd_run(a);
        if (a.command == "log") return cmd_log(a);
        if (a.command == "needs") return cmd_needs(a);
        if (a.command == "watch") return cmd_watch(a);
        if (a.command == "released") return cmd_released(a);
        if (a.command == "json") return cmd_json(a);
        die("unknown command " + a.command + " (myorg help)", 2);
    } catch (const std::exception& e) {
        die(e.what());
    }
}
