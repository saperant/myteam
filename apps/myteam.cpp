// myteam: a team of AI agents working through a Markdown plan.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>
#include <map>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <taskml/taskml.hpp>

#include "myteam/blockers.hpp"
#include "myteam/feedback.hpp"
#include "myteam/services.hpp"
#include "myteam/mcp.hpp"
#include "myteam/changelog.hpp"
#include "myteam/checkpoint.hpp"
#include "myteam/coach.hpp"
#include "myteam/memory.hpp"
#include "myteam/guard.hpp"
#include "myteam/inspect.hpp"
#include "myteam/context.hpp"
#include "myteam/loop.hpp"
#include "myteam/plan.hpp"
#include "myteam/project.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"

using namespace myteam;

namespace {

const char* kUsage = R"(usage: myteam <command> [options]

commands:
  init      create .myteam/ (config, project brief) and a PLAN.md      -t TEAM [--force]
  status    the plan, the crew's ready and waiting work, what needs you
  check     validate the plan and the team
  run       a step: ready tasks, the Gatekeeper's checks, the coach's decisions   [run options]
  say       tell the coach something (you are the customer); no text: the conversation   [TEXT]
  digest    what the coach reads at its next decision (its whole prompt)
  blockers  what is blocked, on what, and what needs you
  done      finish a task yourself (an @operator task), with a note    TASK [NOTE] [--declined]
            --declined: not done (refused, not needed); what waits for it is held for a decision
  unblock   drop dead blocker links, release finished blockers' tasks  [--dry-run]
  dedup     merge open twin tasks (same title and assignee): ^old -> ^kept  [--dry-run]
  feedback  the crew's feedback for myteam itself                       [--all] [--export [-o FILE]]
  team      list the members, roles, units and models
  teams     the teams there are to choose from                          [--json]
  context   print the clean context an agent gets for a task           TASK [--member H]
  runners   which agent CLIs are installed
  convert   print a TaskML plan as a Markdown plan                     FILE
  guard     check a command against the safeguards                      --check "COMMAND"  (as a hook: started by myteam run)
  undo      put back what a run changed; its task goes back to [ ]       RUN | ^task
  json      what a UI needs, as JSON (myrcp's plugin)                  WHAT [NAME] [--from N] [--max N]
            WHAT: overview, plan, team, runs, run NAME, events NAME, conversation,
                  coach, digest, council, skills, feedback, services
  services  the containers myteam runs for the team (services: in config)  [status | up | down [--purge] | logs NAME | restart NAME | reset NAME]
  mcp       the MCP server agents get (stdio; started by myteam run)    --member H --task ID [--run-dir DIR] [--commits]

options:
  -d, --directory DIR   the project (default: the current directory)
  -t, --team TEAM       team name or directory (default: config, then the plan's team:)

run options:
  -n, --iterations N    steps to run (default 1); stops early when the crew waits
  --max N               at most N task runs per round (direct)
  --task KEY            only this task (^id or line number), outside the coach's step
  --runner R            use runner R (claude, codex, vibe) for everyone
  -i, --interactive     show each prompt and ask before running it
  --dry-run             show what would run; change nothing
  -q, --quiet           no live agent output
  --report FILE         write what the step did as JSON (for myorg)
)";

struct Args {
    std::string command;
    std::string dir = ".";
    std::string team;
    std::vector<std::string> positional;
    std::vector<std::string> hide;  ///< guard: paths the agent may not read
    std::string member, task, runner, topic, thread, output, steward, run_dir, note, hold, report;
    int iterations = 1, max = 0, from = 0;
    bool json = false, commits = false, services = false, purge = false, force = false, all = false, serve = false, drafts = false, no_hugo = false, export_ = false, interactive = false, dry_run = false, quiet = false, later = false, no_po = false, declined = false;
};

std::vector<std::string> self_mcp_command();

[[noreturn]] void die(const std::string& msg, int code = 1) {
    std::cerr << "myteam: " << msg << "\n";
    std::exit(code);
}

Args parse_args(int argc, char** argv) {
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
        else if (s == "-t" || s == "--team") a.team = value(i);
        else if (s == "--member") a.member = value(i);
        else if (s == "--task") a.task = value(i);
        else if (s == "--runner") a.runner = value(i);
        else if (s == "--topic") a.topic = value(i);
        else if (s == "--thread") a.thread = value(i);
        else if (s == "--later") a.later = true;
        else if (s == "--commits") a.commits = true;
        else if (s == "--services") a.services = true;
        else if (s == "--purge") a.purge = true;
        else if (s == "--all") a.all = true;
        else if (s == "--serve") a.serve = true;
        else if (s == "--drafts") a.drafts = true;
        else if (s == "--declined") a.declined = true;
        else if (s == "--no-hugo") a.no_hugo = true;
        else if (s == "--export") a.export_ = true;
        else if (s == "-o" || s == "--output") a.output = value(i);
        else if (s == "--no-po") a.no_po = true;
        else if (s == "--steward") a.steward = value(i);
        else if (s == "--run-dir") a.run_dir = value(i);
        else if (s == "--hide") a.hide.push_back(value(i));
        else if (s == "--hold") a.hold = value(i);
        else if (s == "--report") a.report = value(i);
        else if (s == "--json") a.json = true;
        else if (s == "--note" || s == "--check") a.note = value(i);
        else if (s == "--no-steward") a.steward = "off";
        else if (s == "-n" || s == "--iterations") a.iterations = number(i);
        else if (s == "--max") a.max = number(i);
        else if (s == "--from") a.from = number(i);
        else if (s == "--force") a.force = true;
        else if (s == "-i" || s == "--interactive") a.interactive = true;
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

std::string team_name(const Args& a, const Project& p) {
    if (!a.team.empty()) return a.team;
    if (!p.config().team.empty()) return p.config().team;
    if (fs::exists(p.plan_path())) {
        Plan plan = Plan::load(p.plan_path());
        if (auto t = plan.directive("team"); !t.empty()) return t;
    }
    die("no team: give -t TEAM, or set team: in .myteam/config.yaml");
}

Team open_team(const Args& a, const Project& p) {
    std::string name = team_name(a, p);
    auto dir = find_team_dir(name, p.dir());
    if (!dir) {
        std::string where;
        for (const auto& d : team_search_path(p.dir())) where += "\n  " + d.string();
        die("team \"" + name + "\" not found in:" + where);
    }
    Team t = load_team(*dir, p.team_overlay());
    t.external = p.config().external;  // the org's other teams (myorg)
    return t;
}

Plan open_plan(const Project& p) {
    if (!fs::exists(p.plan_path())) die("no plan at " + p.plan_path().string() + " (myteam init creates one)");
    return Plan::load(p.plan_path());
}

void print_diagnostics(const Plan& plan) {
    for (const auto& d : plan.errors()) std::cerr << plan.path().filename().string() << ": " << d.format(plan.text()) << "\n";
}

bool tty() { return ::isatty(1); }
std::string dim(const std::string& s) { return tty() ? "\033[2m" + s + "\033[0m" : s; }
std::string bold(const std::string& s) { return tty() ? "\033[1m" + s + "\033[0m" : s; }

// ---------------------------------------------------------------------------

int cmd_init(const Args& a) {
    if (a.team.empty()) die("init needs -t TEAM", 2);
    Project p = Project::open(a.dir);
    if (!find_team_dir(a.team, p.dir())) die("team \"" + a.team + "\" not found");
    InitResult r = init_project(p.dir(), a.team, a.force);
    for (const auto& f : r.created) std::cout << "created " << fs::relative(f, p.dir()).string() << "\n";
    for (const auto& f : r.kept) std::cout << "kept    " << fs::relative(f, p.dir()).string() << " (exists; --force replaces)\n";
    return 0;
}

int cmd_status(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    Plan plan = open_plan(p);
    std::cout << bold(plan.directive("project", p.dir().filename().string())) << "  ·  team " << team.name
              << "  ·  " << p.config().methodology << "\n";
    if (!plan.directive("sprint").empty()) std::cout << "sprint: " << plan.directive("sprint") << "\n";
    int counts[6] = {};
    for (const TaskRef& t : plan.tasks()) ++counts[static_cast<int>(t.task->status)];
    std::cout << plan.tasks().size() << " tasks: " << counts[0] << " pending, " << counts[1] << " in progress, "
              << counts[5] << " in review, " << counts[3] << " blocked, " << counts[2] << " done\n";
    if (!plan.ok()) {
        std::cout << "\nThe plan does not validate:\n";
        print_diagnostics(plan);
    }
    auto ready = ready_assigned_tasks(plan, team);
    std::cout << "\n" << bold("Ready") << "\n";
    if (ready.empty()) std::cout << dim("  (none)") << "\n";
    for (const TaskRef* t : ready) {
        const Member* m = team.find(*t->task->assignee);
        std::cout << "  " << t->key() << "  " << t->task->description << "  "
                  << dim("@" + m->handle + " · " + m->models.for_priority(t->task->priority).str()) << "\n";
    }
    auto waiting = waiting_assigned_tasks(plan, team);
    if (!waiting.empty()) {
        std::cout << "\n" << bold("Waiting") << "\n";
        for (const TaskRef* t : waiting) {
            std::string why;
            for (const auto& d : plan.unmet_dependencies(*t)) why += " ^" + d;
            for (const auto& b : plan.active_blockers(*t)) why += " ^" + b;
            std::cout << "  " << t->key() << "  " << t->task->description << "  " << dim("on" + why) << "\n";
        }
    }
    {
        auto conv = load_conversation(p);
        if (!conv.empty())
            std::cout << "\n" << bold("Customer") << "  last: " << (conv.back().who == "customer" ? "you" : "the coach") << ", "
                      << conv.back().when << dim("  (myteam say)") << "\n";
        CoachState cs = load_coach_state(p);
        if (p.config().methodology == "coach") {
            char buf[48];
            std::snprintf(buf, sizeof buf, "$%.2f", cs.cost);
            std::cout << "\n" << bold("Coach") << "  " << cs.steps << " steps, " << cs.decisions << " decisions, "
                      << cs.milestones << " milestones, " << buf << (cs.news.empty() ? "" : ", " + std::to_string(cs.news.size()) + " news waiting")
                      << (cs.objection.empty() ? "" : ", the Gatekeeper objects") << dim("  (myteam digest)") << "\n";
        }
    }
    BlockerReview rv = review_blockers(plan);
    if (!rv.for_operator.empty()) {
        std::cout << "\n" << bold("Needs you") << dim("  (myteam done TASK \"note\" when it is done)") << "\n";
        for (const TaskRef* t : rv.for_operator) {
            std::cout << "  " << t->key() << "  " << t->task->description;
            for (const auto& n : t->task->notes)
                if (starts_with(n, "Needed by")) std::cout << "  " << dim(n);
            std::cout << "\n";
        }
    }
    if (!rv.blocked.empty()) {
        std::cout << "\n" << bold("Blocked") << dim("  (myteam blockers)") << "\n";
        for (const BlockedItem& b : rv.blocked) {
            std::string on;
            for (const TaskRef* o : b.open) on += " ^" + *o->task->id;
            for (const auto& m : b.missing) on += " ^" + m + "(missing)";
            std::cout << "  " << b.task->key() << "  " << b.task->task->description << "  "
                      << dim(b.unlinked ? "no blocker task: the coach decides" : "on" + on) << "\n";
        }
    }
    {
        int open = 0, reports = 0;
        for (const FeedbackItem& f : load_feedback(p))
            if (f.status == "new") ++open, reports += f.reports;
        if (open)
            std::cout << "\n" << bold("Feedback for myteam") << "  " << open << " new issue" << (open == 1 ? "" : "s")
                      << " (" << reports << " report" << (reports == 1 ? "" : "s") << ")"
                      << dim("  (myteam feedback)") << "\n";
    }
    return plan.ok() ? 0 : 1;
}

int cmd_check(const Args& a) {
    Project p = Project::open(a.dir);
    int bad = 0;
    try {
        Team team = open_team(a, p);
        std::cout << "team " << team.id << ": " << team.members.size() << " members, ok\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        ++bad;
    }
    Plan plan = open_plan(p);
    print_diagnostics(plan);
    for (const auto& w : plan.warnings())
        std::cerr << plan.path().filename().string() << ": " << w.format(plan.text()) << "\n";
    if (plan.ok()) std::cout << "plan " << fs::relative(plan.path(), p.dir()).string() << ": " << plan.tasks().size() << " tasks, ok\n";
    else ++bad;
    return bad ? 1 : 0;
}

int cmd_team(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    std::cout << bold(team.name) << dim("  " + team.dir.string()) << "\n";
    if (!team.overlay.empty()) std::cout << dim("  project overrides: " + team.overlay.string()) << "\n";
    for (const Member& m : team.members) {
        std::string tag = m.handle == team.coach ? " (coach)" : m.handle == team.gatekeeper ? " (Gatekeeper)"
                          : m.handle == team.product_owner && team.coach.empty() ? " (product owner)" : "";
        if (!m.unit.empty()) tag += " in " + m.unit + "/";
        std::cout << "  @" << m.handle << "  " << m.name << ", " << m.role << tag << "\n      "
                  << dim(m.models.standard.str() + (m.models.premium ? " · premium " + m.models.premium->str() : "") +
                         (m.models.economy ? " · economy " + m.models.economy->str() : ""))
                  << "\n";
    }
    return 0;
}

int cmd_context(const Args& a) {
    if (a.positional.empty()) die("context needs a task (^id or line number)", 2);
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    Plan plan = open_plan(p);
    const TaskRef* t = plan.find(a.positional[0]);
    if (!t) die("no task " + a.positional[0] + " in the plan");
    const Member* m = nullptr;
    if (!a.member.empty()) m = team.find(a.member);
    else if (t->task->assignee) m = team.find(*t->task->assignee);
    if (!m) die("the task has no member of the team as assignee; give --member");
    if (a.member.empty()) {
        // Exactly what `run` gives it: its unit's brief, its memory, the skills.
        Loop loop(p, team, project_runners(p));
        LoopOptions o;
        o.only_task = t->key();
        o.mcp_command = self_mcp_command();
        for (const DispatchPlan& dp : loop.preview(o)) {
            std::cout << dp.context.render();
            std::cerr << dim("\n~" + std::to_string(dp.context.estimated_tokens()) + " tokens for @" + m->handle + " · " +
                             dp.model.str() + (m->unit.empty() ? "" : " · in " + m->unit + "/"))
                      << "\n";
            return 0;
        }
    }
    Loop loop(p, team, project_runners(p));
    const Project& env = loop.env_for(m->unit);  // the member's unit: its brief and rules
    ContextInputs in{.team = team, .member = *m, .plan = plan, .brief = env.brief(), .tools = false, .commits = false,
                     .services = "", .protected_paths = "", .notes = {}, .skills = "", .memory = memory_section(p, m->handle)};
    AgentContext ctx = build_task_context(in, *t);
    std::cout << ctx.render();
    std::cerr << dim("\n~" + std::to_string(ctx.estimated_tokens()) + " tokens for @" + m->handle + " · " +
                     m->models.for_priority(t->task->priority).str())
              << "\n";
    return 0;
}

int cmd_teams(const Args& a) {
    const auto catalog = team_catalog(a.dir);
    if (a.json) {
        taskml::Json arr = taskml::Json::array();
        for (const CatalogEntry& c : catalog) {
            taskml::Json j = taskml::Json::object();
            j.set("name", c.name);
            j.set("dir", c.dir.string());
            if (!c.error.empty()) {
                j.set("error", c.error);
                arr.push_back(j);
                continue;
            }
            j.set("title", c.team.name);
            j.set("summary", c.team.summary);
            j.set("topology", c.team.topology);
            j.set("domain", c.team.domain);
            taskml::Json del = taskml::Json::array();
            for (const auto& d : c.team.delivers) del.push_back(d);
            j.set("delivers", del);
            taskml::Json ms = taskml::Json::array();
            for (const Member& m : c.team.members) {
                taskml::Json x = taskml::Json::object();
                x.set("handle", m.handle);
                x.set("name", m.name);
                x.set("role", m.role);
                x.set("summary", m.summary);
                ms.push_back(x);
            }
            j.set("members", ms);
            j.set("product_owner", c.team.product_owner);
            arr.push_back(j);
        }
        std::cout << arr.dump(2) << "\n";
        return 0;
    }
    for (const CatalogEntry& c : catalog) {
        if (!c.error.empty()) {
            std::cout << bold(c.name) << "  " << dim("does not load: " + c.error) << "\n";
            continue;
        }
        std::cout << bold(c.name) << "  " << c.team.name << " · " << c.team.members.size() << " members"
                  << (c.team.topology.empty() ? "" : " · " + c.team.topology)
                  << (c.team.domain.empty() ? "" : " · " + c.team.domain) << "\n";
        if (!c.team.summary.empty()) std::cout << "  " << c.team.summary << "\n";
        if (!c.team.delivers.empty()) std::cout << dim("  delivers: " + join(c.team.delivers, ", ")) << "\n";
        std::cout << dim("  " + c.dir.string()) << "\n";
    }
    return 0;
}

int cmd_runners(const Args& a) {
    Project p = Project::open(a.dir);
    for (const std::string& id : runner_ids()) {
        auto r = make_runner(id, p.runner_settings(id));
        std::string why = r->start_problem();
        if (!r->available()) std::cout << "  " << id << "  not installed\n";
        else if (!why.empty()) std::cout << "  " << id << "  " << why << "\n";
        else std::cout << "  " << id << "  " << r->executable().string() << "\n";
    }
    return 0;
}

int cmd_convert(const Args& a) {
    if (a.positional.empty()) die("convert needs a file", 2);
    std::string text = read_file(a.positional[0]);
    taskml::ParseResult r = taskml::parse(text);
    for (const auto& e : r.errors) std::cerr << e.format(text) << "\n";
    std::cout << taskml::to_markdown(r.document);
    return r.ok() ? 0 : 1;
}

void print_event(const DispatchPlan& dp, const AgentEvent& e) {
    std::string who = "@" + dp.member->handle;
    switch (e.kind) {
        case AgentEvent::Kind::Started: std::cout << dim(who + " starts (" + e.text + ")") << "\n"; break;
        case AgentEvent::Kind::Text: {
            std::string t(trim(e.text));
            if (t.size() > 400) t = t.substr(0, 400) + "…";
            std::cout << who << ": " << t << "\n";
            break;
        }
        case AgentEvent::Kind::ToolCall: std::cout << dim("  " + e.tool + " " + e.text) << "\n"; break;
        case AgentEvent::Kind::Error: std::cout << "  error: " << e.text << "\n"; break;
        default: break;
    }
    std::cout.flush();
}

bool confirm(const DispatchPlan& dp) {
    std::cout << "\n" << bold("── prompt for @" + dp.member->handle + " (" + dp.model.str() + ") ──") << "\n"
              << dp.context.render() << bold("── ~" + std::to_string(dp.context.estimated_tokens()) + " tokens ──")
              << "\nRun it? [Y/n] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    return answer.empty() || answer[0] == 'y' || answer[0] == 'Y';
}


// ---------------------------------------------------------------------------
// The customer and the product owner
// ---------------------------------------------------------------------------


int cmd_blockers(const Args& a) {
    Project p = Project::open(a.dir);
    Plan plan = open_plan(p);
    BlockerReview rv = review_blockers(plan);
    if (rv.blocked.empty() && rv.for_operator.empty() && rv.customer_questions.empty()) {
        std::cout << "Nothing is blocked.\n";
        return 0;
    }
    for (const BlockedItem& b : rv.blocked) {
        const auto& t = *b.task->task;
        std::cout << bold(b.task->key() + "  " + t.description) << dim(t.assignee ? "  @" + *t.assignee : "") << "\n";
        for (const auto& r : b.reasons) std::cout << "  " << r << "\n";
        for (const TaskRef* o : b.open) {
            std::string who = o->task->assignee ? "@" + *o->task->assignee : "unassigned";
            if (is_operator_task(*o->task)) who = "you (@operator)";
            else if (is_customer_question(*o->task)) who = "the customer, through the coach";
            std::cout << "  waits for ^" << *o->task->id << " " << status_label(o->task->status) << ": "
                      << o->task->description << dim("  " + who) << "\n";
        }
        for (const auto& m : b.missing) std::cout << "  waits for ^" << m << ", which is not in the plan\n";
        if (b.unlinked) std::cout << "  " << dim("no blocker task: nothing will unblock it by itself") << "\n";
        if (!b.unlinked && b.open.empty() && b.missing.empty())
            std::cout << "  " << dim("every blocker is done: the next round unblocks it") << "\n";
        std::cout << "\n";
    }
    if (!rv.customer_questions.empty()) {
        std::cout << bold("Questions for the customer") << "\n";
        for (const TaskRef* q : rv.customer_questions)
            std::cout << "  " << q->key() << "  " << q->task->description << "  "
                      << dim("the coach answers it, or asks you (myteam say)")
                      << "\n";
        std::cout << "\n";
    }
    if (!rv.for_operator.empty()) {
        std::cout << bold("Needs you") << "\n";
        for (const TaskRef* t : rv.for_operator) std::cout << "  " << t->key() << "  " << t->task->description << "\n";
        std::cout << dim("When one is done: myteam done TASK \"what you did / where it is\"") << "\n";
    }
    return 0;
}

int cmd_feedback(const Args& a) {
    Project p = Project::open(a.dir);
    if (a.export_) {
        std::string md = export_feedback(p, a.all);
        if (a.output.empty()) {
            std::cout << md;
        } else {
            write_file_atomic(a.output, md);
            std::cout << "wrote " << a.output << "\n";
        }
        return 0;
    }
    std::vector<FeedbackItem> items = load_feedback(p);
    int shown = 0;
    for (const FeedbackItem& f : items) {
        if (!a.all && f.status != "new" && f.status != "filed") continue;
        ++shown;
        std::string who;
        for (const auto& r : f.reporters) who += " @" + r;
        std::cout << "  ^" << f.id << "  " << dim("[" + f.area + "]") << " " << f.title << "  "
                  << dim("x" + std::to_string(f.reports) + who + (f.status == "new" ? "" : ", " + f.status) +
                         (f.since_fix ? ", reported again " + std::to_string(f.since_fix) + "x since" : ""))
                  << "\n";
    }
    if (!shown) std::cout << "No feedback for myteam yet.\n";
    else
        std::cout << dim("In " + p.feedback_path().string() +
                         ": [~] filed, [x] fixed, [-] declined. myteam feedback --export for a digest.")
                  << "\n";
    return 0;
}

int cmd_done(const Args& a) {
    if (a.positional.empty()) die("done needs a task (^id or line number)", 2);
    Project p = Project::open(a.dir);
    Plan plan = open_plan(p);
    const TaskRef* t = plan.find(a.positional[0]);
    if (!t) die("no task " + a.positional[0] + " in the plan");
    std::string note;
    for (std::size_t i = 1; i < a.positional.size(); ++i) note += (note.empty() ? "" : " ") + a.positional[i];
    PlanEditor ed(plan);
    // --declined: not done, and what waits for it does not go on as if it were.
    const auto status = a.declined ? taskml::TaskStatus::Cancelled : taskml::TaskStatus::Completed;
    if (a.declined) note = "Declined (@operator " + iso_timestamp().substr(0, 10) + ")" + (note.empty() ? "" : ": " + note);
    ed.set_status(*t, status);
    if (!note.empty()) ed.add_note(*t, note);
    CommitResult c = ed.commit();
    if (!c.ok) die("plan not updated: " + c.error);
    std::cout << t->key() << " " << status_label(status) << "  " << t->task->description << "\n";
    // Release what it blocked right away.
    Plan after = Plan::load(p.plan_path());
    PlanEditor ued(after);
    std::string id = t->task->id.value_or("");
    std::vector<std::string> dropped;
    auto un = unblock_resolved(after, ued, id.empty() ? std::map<std::string, std::string>{}
                                                      : std::map<std::string, std::string>{{id, note.empty() ? "@operator" : "@operator: " + note}},
                               &dropped);
    if (ued.changed() && ued.commit().ok) {
        if (!dropped.empty()) append_changelog(p.changelog_path(), dropped_links_entry(dropped));
        for (const auto& d : dropped) std::cout << "dropped blocker link " << d << "\n";
        for (const auto& u : un) std::cout << "unblocked " << u.key << "  " << u.description << "\n";
    }
    return 0;
}

// The sweep the loop does at each step, by hand: dead blocker links go, each one on record.
int cmd_unblock(const Args& a) {
    Project p = Project::open(a.dir);
    Plan plan = open_plan(p);
    PlanEditor ed(plan);
    std::vector<std::string> dropped;
    auto un = unblock_resolved(plan, ed, {}, &dropped);
    for (const auto& d : dropped) std::cout << "dropped blocker link " << d << "\n";
    for (const auto& u : un) std::cout << "unblocked " << u.key << "  " << u.description << "\n";
    if (!ed.changed()) {
        std::cout << dim("no dead blocker links") << "\n";
        return 0;
    }
    if (a.dry_run) {
        std::cout << dim("dry run: the plan is not changed") << "\n";
        return 0;
    }
    CommitResult c = ed.commit();
    if (!c.ok) die("plan not updated: " + c.error);
    if (!dropped.empty()) append_changelog(p.changelog_path(), dropped_links_entry(dropped));
    return 0;
}

// The twin cleanup the loop does at each step, by hand: one line per merge, old -> kept.
int cmd_dedup(const Args& a) {
    Project p = Project::open(a.dir);
    Plan plan = open_plan(p);
    PlanEditor ed(plan);
    const auto merged = merge_open_twins(plan, ed);
    for (const auto& m : merged) std::cout << twin_cleanup_line(m) << "\n";
    if (merged.empty()) {
        std::cout << dim("no open twins") << "\n";
        return 0;
    }
    if (a.dry_run) {
        std::cout << dim("dry run: the plan is not changed") << "\n";
        return 0;
    }
    CommitResult c = ed.commit();
    if (!c.ok) die("plan not updated: " + c.error);
    append_changelog(p.changelog_path(), twin_cleanup_entry(merged));
    // The blocker links to a merged twin move to the kept task now, not at the next step.
    Plan after = Plan::load(p.plan_path());
    PlanEditor ued(after);
    std::vector<std::string> dropped;
    unblock_resolved(after, ued, {}, &dropped);
    if (ued.changed() && ued.commit().ok)
        for (const auto& d : dropped) std::cout << "dropped blocker link " << d << "\n";
    return 0;
}

// The team, or an exception (not exit) when it cannot be found.
int cmd_json(const Args& a) {
    if (a.positional.empty())
        die("json needs WHAT: overview, plan, team, runs, run NAME, events NAME, conversation, coach, digest, council, "
            "skills, feedback, services", 2);
    Project p = Project::open(a.dir);
    const std::string what = a.positional[0];
    const std::string name = a.positional.size() > 1 ? a.positional[1] : "";
    taskml::Json out;
    try {
        if (what == "plan") out = inspect::plan(p);
        else if (what == "runs") out = inspect::runs(p, a.max ? a.max : 200);
        else if (what == "run" || what == "events") {
            if (name.empty()) die("json " + what + " needs a run name", 2);
            out = what == "run" ? inspect::run(p, name) : inspect::events(p, name, static_cast<std::size_t>(a.from));
        } else if (what == "conversation") out = inspect::conversation(p);
        else if (what == "council") out = inspect::council(p);
        else if (what == "skills") out = inspect::skills(p);
        else if (what == "feedback") out = inspect::feedback(p);
        else if (what == "services") out = services_json(p);
        else if (what == "overview" || what == "team" || what == "coach" || what == "digest") {
            Team team = open_team(a, p);
            out = what == "overview" ? inspect::overview(p, team)
                  : what == "team"   ? inspect::team(p, team)
                  : what == "coach"  ? inspect::coach(p, team)
                                     : inspect::digest(p, team);
        } else {
            die("json: unknown WHAT " + what, 2);
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

int cmd_services(const Args& a) {
    Project p = Project::open(a.dir);
    if (p.config().services.empty()) {
        std::cout << "No services (services: in .myteam/config.yaml).\n";
        return 0;
    }
    const std::string what = a.positional.empty() ? "status" : a.positional[0];
    const std::string name = a.positional.size() > 1 ? a.positional[1] : "";
    ServicesResult r;
    if (what == "up") r = ensure_services(p);
    else if (what == "down") r = stop_services(p, a.purge);
    else {
        if ((what == "logs" || what == "restart" || what == "reset") && name.empty())
            die("services " + what + " needs a service name", 2);
        ServiceAnswer ans = service_action(p, what, name, a.max ? a.max : 80);
        std::cout << ans.text << (ans.text.empty() || ans.text.back() == '\n' ? "" : "\n");
        return ans.ok ? 0 : 1;
    }
    for (const auto& n : r.notes) std::cout << "  " << n << "\n";
    for (const auto& pr : r.problems) std::cout << "  problem: " << pr << "\n";
    if (what == "up") std::cout << service_action(p, "status", "").text;
    return r.problems.empty() ? 0 : 1;
}

int cmd_mcp(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    std::string task = a.task;
    if (!task.empty() && task[0] == '^') task.erase(0, 1);
    fs::path report = a.run_dir.empty() ? fs::path() : fs::path(a.run_dir) / kMcpReportFile;
    McpServer server({p, team, normalize_handle(a.member), task, report, a.commits && !report.empty(),
                      a.services && !report.empty()});
    server.serve(std::cin, std::cout);
    return 0;
}

std::string self_exe() {
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : self.string();
}

/// How agents start the MCP server: this executable.
std::vector<std::string> self_mcp_command() {
    std::string self = self_exe();
    if (self.empty()) return {};
    return {self, "mcp"};
}

/// The guard hook: this executable.
std::vector<std::string> self_guard_command() {
    std::string self = self_exe();
    if (self.empty()) return {};
    return {self, "guard"};
}

int cmd_guard(const Args& a) {
    Project p = Project::open(a.dir);
    if (!a.note.empty() || !a.positional.empty()) {
        // myteam guard --check "rm -rf /": what the guard would say.
        GuardPolicy policy = guard_policy(p);
        GuardContext ctx = guard_context(p, policy, fs::current_path());
        std::string cmd = !a.note.empty() ? a.note : a.positional[0];
        GuardDecision d = check_command(ctx, cmd);
        std::cout << (d.allow ? "allowed" : "refused: " + d.reason) << "\n";
        return d.allow ? 0 : 2;
    }
    return run_guard_hook(p, a.run_dir, std::cin, std::cerr, a.hide);
}

int cmd_undo(const Args& a) {
    if (a.positional.empty()) die("undo needs a run (a directory name under .myteam/runs) or ^task", 2);
    Project p = Project::open(a.dir);
    UndoResult u = undo_run(p, a.positional[0]);
    for (const auto& r : u.restored) std::cout << "  restored " << r << "\n";
    for (const auto& r : u.skipped) std::cout << "  kept " << r << dim("  (changed since the run)") << "\n";
    for (const auto& pr : u.problems) std::cout << "  problem: " << pr << "\n";
    for (const auto& c : u.commits)
        std::cout << "  the run committed " << c.substr(0, 10) << dim("  (undo leaves commits: git revert " + c.substr(0, 10) + ")") << "\n";
    if (u.run.empty()) return 1;
    std::cout << "undid run " << u.run << (u.task.empty() ? "" : " (" + u.task + " is back to [ ])") << "\n";
    append_changelog(p.changelog_path(), "## " + iso_timestamp() + " · Undone: run " + u.run + "\n\n- " +
                                             std::to_string(u.restored.size()) + " files restored, " +
                                             std::to_string(u.skipped.size()) + " kept (changed since)\n");
    return u.problems.empty() ? 0 : 1;
}

int print_round(const IterationResult& r, const Args& a) {
    int failures = 0;
    for (const DispatchOutcome& d : r.dispatches) {
        if (a.dry_run) {
            std::cout << d.plan.task_key << "  " << d.plan.description << "  → @" << d.plan.member->handle << " ("
                      << d.plan.model.str() << ", ~" << d.plan.context.estimated_tokens() << " tokens)\n";
            if (d.plan.reason != "assigned") std::cout << dim("  " + d.plan.reason) << "\n";
            continue;
        }
        if (d.skipped) {
            std::cout << dim("skipped " + d.plan.task_key) << "\n";
            continue;
        }
        std::string result = d.report.status ? status_label(*d.report.status) : (d.run.ok ? "no status" : "failed");
        std::cout << bold(d.plan.task_key + " " + result) << "  " << d.plan.description << "\n";
        for (const auto& c : d.plan_changes) std::cout << "  plan: " << c << "\n";
        for (const auto& pr : d.problems) std::cout << "  problem: " << pr << "\n";
        if (!d.run.ok) ++failures;
    }
    for (const auto& w : r.waiting) std::cout << dim("waiting: " + w) << "\n";
    return failures;
}

LoopOptions loop_options(const Args& a) {
    LoopOptions o;
    o.dry_run = a.dry_run;
    o.only_task = a.task;
    o.runner = a.runner;
    o.mcp_command = self_mcp_command();
    o.guard_command = self_guard_command();
    o.max_dispatches = a.interactive && a.max == 0 ? 1 : a.max;
    o.on_note = [](const std::string& n) { std::cout << dim("· " + n) << "\n"; };
    if (!a.quiet && !a.dry_run) o.on_event = print_event;
    if (a.interactive) o.confirm = confirm;
    return o;
}

void print_coach_step(const CoachStep& s) {
    for (const DispatchOutcome& d : s.runs) {
        if (!d.executed) continue;
        std::string result = d.plan.talk ? (d.run.ok ? "answered" : "failed")
                             : d.report.status ? status_label(*d.report.status)
                                               : (d.run.ok ? "ran" : "failed");
        std::cout << bold(d.plan.task_key + " " + result) << "  " << d.plan.description << "  "
                  << dim("@" + d.plan.member->handle + " · " + Loop::usage_text(d.run.usage)) << "\n";
        for (const auto& c : d.plan_changes) std::cout << "  plan: " << c << "\n";
    }
    for (const auto& n : s.notes) std::cout << dim("· " + n) << "\n";
    for (const auto& p : s.problems) std::cout << "  problem: " << p << "\n";
    if (s.milestone) std::cout << bold("Milestone: the coach says the goal is met (myteam say to answer)") << "\n";
    if (s.waiting) std::cout << dim("waiting: " + s.waiting_for) << "\n";
    char buf[48];
    std::snprintf(buf, sizeof buf, "$%.2f", s.cost);
    std::cout << dim(std::string("step cost ") + buf) << "\n";
}

/// --report FILE: what the step did, for a program driving myteam.
void write_coach_report(const Args& a, const CoachStep& s) {
    if (a.report.empty()) return;
    taskml::Json j = taskml::Json::object();
    j.set("runs", static_cast<int>(s.runs.size()));
    int failed = 0;
    for (const auto& d : s.runs) failed += d.executed && !d.run.ok;
    j.set("failed", failed);
    j.set("decided", s.decided);
    j.set("milestone", s.milestone);
    j.set("waiting", s.waiting);
    j.set("waiting_for", s.waiting_for);
    j.set("rate_limited", s.rate_limited);
    j.set("needs_human", s.needs_human);
    j.set("cost", s.cost);
    taskml::Json notes = taskml::Json::array();
    for (const auto& n : s.notes) notes.push_back(n);
    j.set("notes", notes);
    taskml::Json problems = taskml::Json::array();
    for (const auto& p : s.problems) problems.push_back(p);
    j.set("problems", problems);
    write_file_atomic(a.report, j.dump(2) + "\n");
}

int cmd_run(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    open_plan(p);
    const std::string& method = p.config().methodology;
    if (method != "direct" && method != "coach")
        std::cerr << "myteam: methodology \"" << method << "\" is not implemented (coach, direct); running a direct task round\n";
    int failures = 0;
    if (method == "coach" && a.task.empty()) {
        Coach coach(p, team, project_runners(p));
        CoachOptions co;
        co.loop = loop_options(a);
        co.dry_run = a.dry_run;
        for (int i = 0; i < a.iterations; ++i) {
            if (a.iterations > 1) std::cout << bold("step " + std::to_string(i + 1) + "/" + std::to_string(a.iterations)) << "\n";
            CoachStep s = coach.step(co);
            print_coach_step(s);
            write_coach_report(a, s);
            for (const auto& d : s.runs) failures += d.executed && !d.run.ok && d.run.failure != Failure::RateLimit;
            if (s.needs_human) return 1;
            if (s.waiting || s.milestone || a.dry_run) break;
        }
        return failures ? 1 : 0;
    }
    Loop loop(p, team, project_runners(p));
    LoopOptions o = loop_options(a);
    if (!team.coach.empty()) o.skip_members = {team.coach, team.gatekeeper};
    for (int i = 0; i < a.iterations; ++i) {
        if (a.iterations > 1) std::cout << bold("round " + std::to_string(i + 1) + "/" + std::to_string(a.iterations)) << "\n";
        IterationResult r = loop.iterate(o);
        failures += print_round(r, a);
        if (r.needs_human) return 1;
        if (r.dispatches.empty() || a.dry_run) break;
    }
    return failures ? 1 : 0;
}

/// The customer's side: a message to the coach, or the conversation so far.
int cmd_say(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    const Member* coach = team.find(team.coach);
    if (!coach) coach = &team.product_owner_member();
    if (a.positional.empty()) {
        auto conv = load_conversation(p);
        if (conv.empty()) {
            std::cout << "No conversation yet: myteam say \"what you want\"\n";
            return 0;
        }
        const std::size_t from = conv.size() > 12 ? conv.size() - 12 : 0;
        for (std::size_t i = from; i < conv.size(); ++i)
            std::cout << bold(conv[i].who == "customer" ? "You" : coach->name) << dim("  " + conv[i].when) << "\n"
                      << conv[i].text << "\n\n";
        return 0;
    }
    std::string message;
    for (const auto& w : a.positional) message += (message.empty() ? "" : " ") + w;
    append_conversation(p, "customer", "", message);
    std::cout << "Told " << coach->name << " (@" << coach->handle << "); the coach reads it at its next decision (myteam run).\n";
    return 0;
}

/// What the coach reads.
int cmd_digest(const Args& a) {
    Project p = Project::open(a.dir);
    Team team = open_team(a, p);
    Coach coach(p, team, project_runners(p));
    const std::string prompt = coach.coach_prompt();
    std::cout << prompt << dim("~" + std::to_string(estimate_tokens(prompt)) + " tokens") << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    try {
        if (a.command == "init") return cmd_init(a);
        if (a.command == "status") return cmd_status(a);
        if (a.command == "check") return cmd_check(a);
        if (a.command == "team") return cmd_team(a);
        if (a.command == "context") return cmd_context(a);
        if (a.command == "run") return cmd_run(a);
        if (a.command == "runners") return cmd_runners(a);
        if (a.command == "teams") return cmd_teams(a);
        if (a.command == "convert") return cmd_convert(a);
        if (a.command == "blockers") return cmd_blockers(a);
        if (a.command == "mcp") return cmd_mcp(a);
        if (a.command == "services") return cmd_services(a);
        if (a.command == "guard") return cmd_guard(a);
        if (a.command == "undo") return cmd_undo(a);
        if (a.command == "say") return cmd_say(a);
        if (a.command == "json") return cmd_json(a);
        if (a.command == "digest") return cmd_digest(a);
        if (a.command == "done") return cmd_done(a);
        if (a.command == "unblock") return cmd_unblock(a);
        if (a.command == "dedup") return cmd_dedup(a);
        if (a.command == "feedback") return cmd_feedback(a);
        if (a.command == "help" || a.command == "-h" || a.command == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (a.command == "version" || a.command == "--version") {
            std::cout << "myteam " << MYTEAM_VERSION << " (taskml " << taskml::library_version() << ")\n";
            return 0;
        }
    } catch (const std::exception& e) {
        die(e.what());
    }
    die("unknown command \"" + a.command + "\"\n" + kUsage, 2);
}
