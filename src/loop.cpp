#include "myteam/loop.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include <taskml/json.hpp>

#include "myteam/blockers.hpp"
#include "myteam/contracts.hpp"
#include "myteam/memory.hpp"
#include "myteam/skills.hpp"
#include "myteam/changelog.hpp"
#include "myteam/checkpoint.hpp"
#include "myteam/feedback.hpp"
#include "myteam/guard.hpp"
#include "myteam/sandbox.hpp"
#include "myteam/commits.hpp"
#include "myteam/services.hpp"
#include "myteam/mcp.hpp"
#include "myteam/util.hpp"

namespace myteam {

using taskml::Json;
using taskml::TaskStatus;

RunnerFactory project_runners(const Project& project) {
    return [&project](const std::string& id) { return make_runner(id, project.runner_settings(id)); };
}

Loop::Loop(const Project& project, const Team& team, RunnerFactory runners)
    : project_(project), team_(team), runners_(std::move(runners)) {}

const Project& Loop::env_for(const std::string& unit) const {
    if (unit.empty()) return project_;
    auto it = envs_.find(unit);
    if (it != envs_.end()) return *it->second;
    std::unique_ptr<Project> p;
    try {
        if (fs::is_directory(project_.dir() / unit)) p = std::make_unique<Project>(Project::open(project_.dir() / unit));
    } catch (const std::exception&) {
    }
    if (!p) return project_;  // no such unit (yet): the project itself
    return *envs_.emplace(unit, std::move(p)).first->second;
}

std::string Loop::unit_of(const DispatchPlan& dp) const {
    if (dp.unit) return *dp.unit;
    return dp.member ? dp.member->unit : std::string();
}

std::vector<std::string> Loop::hidden_from_units() const {
    std::vector<std::string> out;
    for (const fs::path& p : {project_.plan_path(), project_.changelog_path(), project_.state_dir(), project_.dir() / ".myorg"})
        out.push_back(fs::weakly_canonical(p).string());
    return out;
}

std::vector<std::string> task_ids_named(std::string_view text) {
    auto id_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'; };
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '^' || (i > 0 && id_char(text[i - 1]))) continue;
        std::size_t j = i + 1;
        while (j < text.size() && id_char(text[j])) ++j;
        std::string id(text.substr(i + 1, j - i - 1));
        while (!id.empty() && (id.back() == '.' || id.back() == '-' || id.back() == '_')) id.pop_back();
        if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
        i = j - 1;
    }
    return out;
}

std::vector<std::string> note_tasks_commits_name(const Plan& plan, PlanEditor& ed, const std::vector<RunCommit>& commits,
                                                 std::string_view own_id, std::string_view author, std::string_view date) {
    std::vector<std::string> changes;
    for (const RunCommit& c : commits) {
        if (!c.ok || c.commit.empty()) continue;
        const std::string sha = c.commit.substr(0, 10);
        for (const std::string& id : task_ids_named(c.message.empty() ? c.summary : c.message)) {
            if (id == own_id) continue;
            const TaskRef* t = plan.by_id(id);
            if (!t || is_finished(t->task->status)) continue;
            const std::string mark = "Commit `" + sha + "`";
            if (std::any_of(t->task->notes.begin(), t->task->notes.end(),
                            [&](const std::string& n) { return starts_with(n, mark); }))
                continue;
            const std::string who = t->task->assignee ? "@" + normalize_handle(*t->task->assignee) : "its assignee";
            ed.add_note(*t, mark + " by @" + std::string(author) + (own_id.empty() ? "" : " on ^" + std::string(own_id)) +
                                " names this task " + std::string(date) + ": " + who +
                                ", close it or check its criteria (" + first_line(c.summary, 80) + ")");
            changes.push_back("commit `" + sha + "` names open ^" + id + ": noted for " + who);
        }
    }
    return changes;
}

void Loop::note(const LoopOptions& o, IterationResult& r, std::string text) const {
    if (o.on_note) o.on_note(text);
    r.notes.push_back(std::move(text));
}

// ---------------------------------------------------------------------------
// Choosing work
// ---------------------------------------------------------------------------

namespace {

// For the context: what the guard and the sandbox refuse, as they stand now
// (an old note or criterion may say otherwise).
std::string protected_text(const Project& p) {
    std::vector<std::string> on, off;
    for (const auto& g : protected_globs(p, p.config().guard)) (g.size() > 1 && g[0] == '!' ? off : on).push_back(g[0] == '!' ? g.substr(1) : g);
    std::string t = "The safeguards refuse writes to: " + join(on, ", ") + ".\n";
    if (!off.empty()) t += "Yours to write, although they look protected: " + join(off, ", ") + ".\n";
    if (const auto w = p.write_paths(); !w.empty())
        t += "Besides the project, writes go to: " + join(w, ", ") + " (and the temp directory).\n";
    t += "This is how it stands now: if a note or a criterion says otherwise, this wins.\n"
         "To change the plan, report it (status, criteria, handoffs, blockers): myteam writes the plan and the\n"
         "changelog from your report. A change to another protected file is the operator's: a blocker to the\n"
         "operator with the change.\n";
    if (!p.config().web.empty())
        t += "\nThe web: you may read pages from " + join(p.config().web, ", ") +
             " (WebFetch; in Claude Code through the web-fetch agent), and nothing else. These are hosts:\n"
             "a subdomain (docs.example.com of example.com) is not among them unless it is listed. There is no\n"
             "web search: start from those sites.\n";
    return t;
}
}  // namespace

std::string Loop::protected_paths_text(const Project& env) { return protected_text(env); }

Loop::Selection Loop::select(const Plan& plan, const LoopOptions& o) const {
    Selection sel;
    auto skipped = [&](const TaskRef* t) {
        return t->task->assignee && o.skip_members.count(normalize_handle(*t->task->assignee));
    };
    for (const TaskRef* w : waiting_assigned_tasks(plan, team_)) {
        if (skipped(w)) continue;
        std::string why;
        for (const auto& d : plan.unmet_dependencies(*w)) why += (why.empty() ? "depends on ^" : ", ^") + d;
        for (const auto& b : plan.active_blockers(*w)) why += (why.empty() ? "blocked by ^" : ", blocked by ^") + b;
        sel.waiting.push_back(task_one_liner(*w->task) + ": " + why);
    }
    if (!o.only_task.empty()) {
        const TaskRef* t = plan.find(o.only_task);
        if (t && t->task->assignee && team_.find(*t->task->assignee)) sel.keys.push_back(t->key());
        return sel;
    }
    auto tagged = [&](const TaskRef* t) {
        return o.only_tag.empty() ||
               std::find(t->task->tags.begin(), t->task->tags.end(), o.only_tag) != t->task->tags.end();
    };
    std::vector<const TaskRef*> ready;
    for (const TaskRef* t : ready_assigned_tasks(plan, team_))
        if (tagged(t) && !skipped(t)) ready.push_back(t);
    if (o.max_dispatches > 0 && static_cast<int>(ready.size()) > o.max_dispatches) {
        // Fewer slots than ready tasks: the most urgent first, and each member one turn before
        // anyone gets a second (in plan order, one member's many tasks could take every slot).
        // Among equal priority, whoever ran a task least recently goes first (four members with
        // p0 work and three slots: in plan order the fourth would never run).
        std::map<std::string, fs::file_time_type> last_run;  // member -> its newest task run's record
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(project_.runs_dir(), ec)) {
            // run.json: what the loop recorded (every run has one; meta.json is the runner's).
            auto rec = try_read_file(e.path() / "run.json");
            if (!rec) continue;
            try {
                Json j = Json::parse(*rec);
                const Json* who = j.find("member");
                const Json* what = j.find("task");
                if (!who || !who->is_string() || !what || !what->is_string() || !starts_with(what->as_string(), "^")) continue;
                const auto when = fs::last_write_time(e.path() / "run.json", ec);
                auto [it, fresh] = last_run.emplace(who->as_string(), when);
                if (!fresh && when > it->second) it->second = when;
            } catch (const std::exception&) {
            }
        }
        auto ran = [&](const TaskRef* t) {
            auto it = last_run.find(normalize_handle(*t->task->assignee));
            return it == last_run.end() ? fs::file_time_type::min() : it->second;
        };
        std::stable_sort(ready.begin(), ready.end(), [&](const TaskRef* a, const TaskRef* b) {
            const int pa = a->task->priority.value_or(2), pb = b->task->priority.value_or(2);
            if (pa != pb) return pa < pb;
            return ran(a) < ran(b);
        });
        std::vector<const TaskRef*> picked, rest;
        std::set<std::string> served;
        for (const TaskRef* t : ready) {
            const std::string who = normalize_handle(*t->task->assignee);
            (served.insert(who).second ? picked : rest).push_back(t);
        }
        picked.insert(picked.end(), rest.begin(), rest.end());
        ready = std::move(picked);
        ready.resize(static_cast<std::size_t>(o.max_dispatches));
    }
    for (const TaskRef* t : ready) sel.keys.push_back(t->key());
    return sel;
}

DispatchPlan Loop::plan_dispatch(const Plan& plan, const TaskRef& t, const LoopOptions& o, std::string reason) const {
    DispatchPlan dp;
    dp.task_key = t.key();
    dp.description = t.task->description;
    dp.member = t.task->assignee ? team_.find(*t.task->assignee) : nullptr;
    if (!dp.member) dp.member = &team_.product_owner_member();
    dp.model = dp.member->models.for_priority(t.task->priority);
    if (!o.runner.empty() && dp.model.runner != o.runner) dp.model = ModelRef{o.runner, ""};
    else dp.model = usable(dp.model, dp.member->models.standard, o);
    dp.reason = std::move(reason);
    dp.tools = tools_for(dp.model, o);
    const Project& env = env_for(unit_of(dp));
    dp.commits = commits_for(dp.tools, env);
    dp.context = build_task_context(
        {team_, *dp.member, plan, env.brief(), dp.tools, dp.commits, services_brief(env, dp.tools), protected_text(env),
         env.config().notes, skills_section(load_skills(project_), *t.task, dp.member->handle),
         memory_section(project_, dp.member->handle)}, t);
    return dp;
}

bool Loop::tools_for(const ModelRef& model, const LoopOptions& o) const {
    if (o.mcp_command.empty() || !project_.config().mcp) return false;
    auto r = runners_(model.runner);
    return r && r->supports_mcp();
}


ModelRef Loop::usable(const ModelRef& wanted, const ModelRef& standard, const LoopOptions& o) const {
    return usable_model(wanted, standard, runners_, told_, [&](const std::string& n) {
        if (o.on_note) o.on_note(n);
    });
}

bool Loop::commits_for(bool tools, const Project& env) const {
    const auto& cfg = env.config();
    if (!tools || cfg.commits != "tool" || !cfg.guard.checkpoints) return false;
    auto it = git_repo_.find(env.dir().string());
    if (it == git_repo_.end()) it = git_repo_.emplace(env.dir().string(), is_git_repo(env)).first;
    return it->second;
}

std::vector<DispatchPlan> Loop::preview(const LoopOptions& o) {
    std::vector<DispatchPlan> out;
    Plan plan = Plan::load(project_.plan_path());
    Selection sel = select(plan, o);
    for (const std::string& key : sel.keys) {
        const TaskRef* t = plan.find(key);
        if (!t) continue;
        out.push_back(plan_dispatch(plan, *t, o, "assigned"));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

fs::path Loop::new_run_dir(const DispatchPlan& dp) const {
    fs::path run_dir = project_.runs_dir() / (file_timestamp() + "-" + dp.member->handle + "-" + slug(dp.description, 32));
    for (int n = 2; fs::exists(run_dir); ++n) run_dir += "-" + std::to_string(n);
    return run_dir;
}

RunResult Loop::run_agent(const DispatchPlan& dp, const LoopOptions& o, fs::path& run_dir, bool keep_dir) {
    if (!keep_dir || run_dir.empty()) run_dir = new_run_dir(dp);
    std::unique_ptr<AgentRunner> runner = runners_(dp.model.runner);
    if (!runner) {
        RunResult r;
        r.error = "unknown runner \"" + dp.model.runner + "\" (known: claude, codex, vibe)";
        r.failure = Failure::NotAvailable;
        return r;
    }
    const std::string unit = unit_of(dp);
    const Project& env = env_for(unit);
    RunRequest req;
    req.member = dp.member->handle;
    req.what = dp.task_key;
    req.model = dp.model.model;
    req.prompt = dp.context.render();
    req.run_dir = run_dir;
    req.timeout_seconds = project_.config().limits.timeout_seconds;
    req.max_turns = dp.max_turns > 0 ? dp.max_turns : project_.config().limits.max_turns;
    req.max_cost_usd = project_.config().limits.max_cost_usd;
    EventSink sink;
    if (o.on_event) sink = [&](const AgentEvent& e) { o.on_event(dp, e); };
    if (dp.talk) {
        // One answer: everything it needs is in the prompt.
        req.talk = true;
        req.system_prompt = dp.system_prompt;
        req.workdir = project_.dir();
        req.permission = Permission::ReadOnly;
        req.commands = {};
        return runner->run(req, sink);
    }
    req.env = service_agent_env(env);
    req.web_domains = env.config().web;
    req.read_paths = env.read_paths();
    req.write_paths = env.write_paths();
    if (dp.resume_session) {
        req.resume_session = dp.resume_session;
        req.prompt = "Your previous run on this task stopped at a limit. Continue where you stopped. When you "
                     "finish, end your final message with the report blocks as before (<task_status>, "
                     "<work_summary>, and <blocker> or <handoff> when needed).\n";
    }
    req.workdir = env.dir();
    req.permission = dp.permission.value_or(env.config().permission);
    req.deny_paths = {project_.customer_rel()};
    // A unit's agent gets its task in the prompt: the org's plan, changelog and state are not
    // for it to read.
    if (&env != &project_) req.deny_read = hidden_from_units();
    req.commands = env.config().commands;
    req.read_only_shell = req.permission == Permission::ReadOnly;
    try {
        req.sandbox = sandbox_argv(env, dp.model.runner, run_dir);
    } catch (const std::exception& e) {
        RunResult r;
        r.error = e.what();
        r.failure = Failure::NotAvailable;
        return r;
    }
    if (!o.guard_command.empty() && env.config().guard.enabled) {
        req.guard_hook = o.guard_command;
        req.guard_hook.insert(req.guard_hook.end(), {"--directory", env.dir().string(), "--run-dir", run_dir.string()});
        for (const std::string& h : req.deny_read) req.guard_hook.insert(req.guard_hook.end(), {"--hide", h});
    }
    if (dp.tools && runner->supports_mcp()) {
        McpServerSpec spec;
        spec.command = o.mcp_command;
        std::string task = dp.task_key;
        if (!task.empty() && task[0] == '^') task.erase(0, 1);
        spec.command.insert(spec.command.end(), {"--directory", project_.dir().string(), "--team", team_.dir.string(),
                                                 "--member", dp.member->handle, "--task", task, "--run-dir",
                                                 run_dir.string()});
        if (dp.commits) spec.command.push_back("--commits");
        if (!env.config().services.empty()) spec.command.push_back("--services");
        req.mcp = std::move(spec);
    }
    return runner->run(req, sink);
}

RunResult Loop::run(const DispatchPlan& dp, const LoopOptions& o, fs::path& run_dir) {
    return run_agent(dp, o, run_dir);
}

namespace {

Json run_record(const DispatchOutcome& out) {
    Json j = Json::object();
    j.set("task", out.plan.task_key);
    j.set("description", out.plan.description);
    j.set("member", out.plan.member ? out.plan.member->handle : "");
    j.set("model", out.plan.model.str());
    j.set("reason", out.plan.reason);
    j.set("context_tokens", static_cast<long long>(out.plan.context.estimated_tokens()));
    const RunResult& r = out.run;
    j.set("ok", r.ok);
    j.set("exit_code", r.exit_code);
    j.set("timed_out", r.timed_out);
    j.set("session_id", r.session_id);
    j.set("reported_model", r.model);
    j.set("stop_reason", r.stop_reason);
    j.set("error", r.error);
    j.set("failure", out.failure);
    Json u = Json::object();
    u.set("input_tokens", r.usage.input_tokens);
    u.set("output_tokens", r.usage.output_tokens);
    u.set("cached_input_tokens", r.usage.cached_input_tokens);
    u.set("cost_usd", r.usage.cost_usd);
    u.set("turns", r.usage.turns);
    j.set("usage", u);
    Json tools = Json::array();
    for (const ToolCall& t : r.tool_calls) {
        Json c = Json::object();
        c.set("name", t.name);
        c.set("detail", t.detail);
        c.set("failed", t.failed);
        tools.push_back(c);
    }
    j.set("tool_calls", tools);
    Json commits = Json::array();
    for (const auto& c : out.commits) commits.push_back(commit_to_json(c));
    j.set("commits", commits);
    Json rep = Json::object();
    rep.set("status", out.report.status_text);
    rep.set("summary", out.report.summary);
    Json hs = Json::array();
    for (const auto& h : out.report.handoffs) {
        Json x = Json::object();
        x.set("to", h.to);
        x.set("instructions", h.instructions);
        hs.push_back(x);
    }
    rep.set("handoffs", hs);
    j.set("report", rep);
    Json changes = Json::array();
    for (const auto& c : out.plan_changes) changes.push_back(c);
    j.set("plan_changes", changes);
    Json problems = Json::array();
    for (const auto& p : out.problems) problems.push_back(p);
    j.set("problems", problems);
    Json org = Json::array();
    for (const auto& x : out.report.org_issues) {
        Json i = Json::object();
        i.set("area", x.area);
        i.set("text", x.text);
        org.push_back(i);
    }
    j.set("org_issues", org);
    Json guard = Json::array();
    for (const auto& g : out.guard) guard.push_back(g);
    j.set("guard", guard);
    return j;
}

std::string tokens(long long n) {
    if (n < 1000) return std::to_string(n);
    char buf[32];
    std::snprintf(buf, sizeof buf, n < 10000 ? "%.1fk" : "%.0fk", static_cast<double>(n) / 1000.0);
    return buf;
}

std::string usage_line(const Usage& u) {  // Loop::usage_text
    std::string out = std::to_string(u.turns) + " turns, " + tokens(u.input_tokens) + " in / " +
                      tokens(u.output_tokens) + " out tokens";
    if (u.cost_usd > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, ", $%.2f", u.cost_usd);
        out += buf;
    }
    return out;
}

}  // namespace

std::string Loop::usage_text(const Usage& u) { return usage_line(u); }

void Loop::record(const DispatchOutcome& out, const std::string& changelog) {
    if (!out.run_dir.empty()) write_file_atomic(out.run_dir / "run.json", run_record(out).dump(2) + "\n");
    if (!changelog.empty()) append_changelog(project_.changelog_path(), changelog);
}

namespace {
void note_fallback(const LoopOptions& o, const DispatchPlan& dp, const RunResult& failed) {
    if (o.on_note)
        o.on_note(dp.model.runner + " cannot run" + (failed.error.empty() ? "" : " (" + first_line(failed.error, 240) + ")") +
                  "; @" + dp.member->handle + " uses " + dp.member->models.standard.str());
}
}  // namespace

DispatchOutcome Loop::execute(DispatchPlan dp, const LoopOptions& o) {
    DispatchOutcome out;
    out.plan = std::move(dp);
    out.executed = true;
    // The run works in its unit: checkpoints, commits and the safeguards are the unit's.
    const Project& env = env_for(unit_of(out.plan));
    // Layer 2: a checkpoint before the run.
    const GuardPolicy& policy = env.config().guard;
    std::optional<Snapshot> before;
    if (policy.checkpoints && !o.dry_run) {
        std::string err;
        before = take_snapshot(env, "myteam: before " + out.plan.task_key + " by @" + out.plan.member->handle, &err);
        if (!before) out.problems.push_back("no checkpoint before the run: " + err);
    }
    // Agents' commits: answered from here while the agent runs.
    if (!before || !before->git_dir.empty()) out.plan.commits = false;
    auto attempt = [&] {
        out.run_dir = new_run_dir(out.plan);
        std::unique_ptr<CommitBroker> broker;
        // It answers the agent's commits and service requests, from outside the sandbox.
        const bool services = out.plan.tools && !env.config().services.empty();
        if (out.plan.commits || services) {
            CommitIdentity who{out.plan.member->handle, out.plan.member->name, "", out.plan.task_key,
                               out.run_dir.filename().string()};
            broker = std::make_unique<CommitBroker>(env, out.plan.commits ? before : std::nullopt, who, out.run_dir);
            broker->start();
        }
        RunResult r = run_agent(out.plan, o, out.run_dir, true);
        if (broker) {
            broker->stop();
            for (auto& c : broker->commits()) out.commits.push_back(std::move(c));
        }
        return r;
    };
    out.run = attempt();
    // The premium or economy runner is not installed: the standard model will do.
    if (out.run.failure == Failure::NotAvailable && o.runner.empty() &&
        !(out.plan.model == out.plan.member->models.standard)) {
        note_fallback(o, out.plan, out.run);
        out.plan.model = out.plan.member->models.standard;
        out.run = attempt();
    }
    out.report = parse_report(out.run.final_text);
    // What the agent reported through the tools (checked as it went).
    merge_reports(out.report, load_mcp_report(out.run_dir));
    // Layer 1: what the guard refused during the run.
    auto blocks = load_guard_blocks(out.run_dir);
    for (const auto& b : blocks) out.guard.push_back("refused " + b.tool + " `" + first_line(b.what, 100) + "`: " + b.reason);
    // Layer 2: what the run changed.
    if (before) {
        std::string err;
        auto after = take_snapshot(env, "myteam: after " + out.plan.task_key + " by @" + out.plan.member->handle, &err);
        if (after) {
            // In the sandbox the agent could not write the plan, .myteam/ and the like:
            // a change there is someone else's (the operator, the customer), kept.
            bool sandboxed = false;
            try {
                sandboxed = !sandbox_argv(env, out.plan.model.runner, out.run_dir).empty();
            } catch (const std::exception&) {
            }
            ChangeReview cr = review_changes(env, policy, *before, *after, true, sandboxed);
            for (const auto& r : cr.reverted) out.guard.push_back("reverted " + r);
            for (const auto& f : cr.flags) out.guard.push_back("flagged " + f);
            out.guard_hold = cr.hold;
            // What the run left, after the reverts: what `myteam undo` puts back.
            if (!cr.reverted.empty())
                if (auto left = take_snapshot(env, "myteam: after the guard, " + out.plan.task_key, &err)) after = left;
            Json g = Json::object();
            Json b = Json::object();
            b.set("commit", before->commit);
            b.set("tree", before->tree);
            Json a = Json::object();
            a.set("commit", after->commit);
            a.set("tree", after->tree);
            g.set("git_dir", before->git_dir);
            g.set("before", b);
            g.set("after", a);
            g.set("files", static_cast<int>(cr.files.size()));
            g.set("lines", cr.lines);
            Json gl = Json::array();
            for (const auto& x : out.guard) gl.push_back(x);
            g.set("guard", gl);
            g.set("hold", cr.hold);
            Json cs = Json::array();
            for (const auto& c : out.commits) cs.push_back(c.commit);
            g.set("commits", cs);
            write_file_atomic(out.run_dir / "guard.json", g.dump(2) + "\n");
        } else {
            out.problems.push_back("no checkpoint after the run: " + err);
        }
    }
    apply_report(out, o);
    record_org_issues(out);
    write_file_atomic(out.run_dir / "run.json", run_record(out).dump(2) + "\n");
    append_changelog(project_.changelog_path(), changelog_entry(out));
    return out;
}

std::vector<Json> Loop::records(const std::string& task_key) const {
    std::vector<std::pair<std::string, Json>> found;
    std::error_code ec;
    if (!fs::is_directory(project_.runs_dir(), ec)) return {};
    for (const auto& e : fs::directory_iterator(project_.runs_dir(), ec)) {
        auto text = try_read_file(e.path() / "run.json");
        if (!text) continue;
        try {
            Json j = Json::parse(*text);
            const Json* t = j.find("task");
            if (t && t->is_string() && t->as_string() == task_key) found.emplace_back(e.path().filename().string(), j);
        } catch (const taskml::JsonParseError&) {
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Json> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

int Loop::consecutive_failures(const std::string& task_key) const {
    int n = 0;
    for (const Json& j : records(task_key)) {
        const Json* f = j.find("failure");
        std::string failure = f && f->is_string() ? f->as_string() : "none";
        if (failure == "crash" || failure == "no-report") ++n;
        else if (failure == "none" || failure == "limit") break;
        // not-available, auth, rate-limit: not the task's fault; look further back
    }
    return n;
}

std::optional<std::string> Loop::limit_session(const DispatchPlan& dp) const {
    for (const Json& j : records(dp.task_key)) {
        auto get = [&](const char* k) {
            const Json* v = j.find(k);
            return v && v->is_string() ? v->as_string() : std::string();
        };
        if (get("failure") != "limit") return std::nullopt;  // only the latest run counts
        if (get("member") != dp.member->handle || ModelRef::parse(get("model")).runner != dp.model.runner)
            return std::nullopt;
        std::string session = get("session_id");
        if (session.empty()) return std::nullopt;
        return session;
    }
    return std::nullopt;
}

void Loop::unblock(IterationResult& res, const LoopOptions& o, const std::map<std::string, std::string>& why) {
    // A contract's version changed: the open tasks that name it hear of it (and the next stand-up).
    try {
        for (const ContractChange& c : note_contract_changes(project_))
            note(o, res, "contract interfaces/" + c.contract + " " + c.from + " -> " + c.to + ": noted on " +
                             std::to_string(c.tasks.size()) + " open task(s)");
    } catch (const std::exception& e) {
        note(o, res, std::string("could not check the contracts' versions: ") + e.what());
    }
    // Twins first (the first step after an upgrade merges the old ones): each merge on record,
    // and the blocker links to a merged twin move to the kept task below.
    {
        Plan plan = Plan::load(project_.plan_path());
        PlanEditor ed(plan);
        const auto merged = plan.ok() ? merge_open_twins(plan, ed) : std::vector<TwinCleanup>{};
        if (!merged.empty()) {
            CommitResult c = ed.commit();
            if (!c.ok) {
                note(o, res, "could not merge twin tasks: " + c.error);
            } else {
                append_changelog(project_.changelog_path(), twin_cleanup_entry(merged));
                for (const auto& m : merged) note(o, res, "merged twin " + twin_cleanup_line(m));
            }
        }
    }
    Plan plan = Plan::load(project_.plan_path());
    PlanEditor ed(plan);
    std::vector<std::string> dropped;
    auto un = unblock_resolved(plan, ed, why, &dropped);
    if (!ed.changed()) return;
    CommitResult c = ed.commit();
    if (!c.ok) {
        note(o, res, "could not unblock tasks: " + c.error);
        return;
    }
    // Dead links taken off (the first run after an upgrade sweeps the old ones): on record.
    if (!dropped.empty()) append_changelog(project_.changelog_path(), dropped_links_entry(dropped));
    for (const auto& d : dropped) note(o, res, "dropped blocker link " + d);
    for (const Unblocked& u : un) {
        std::string ids;
        for (const auto& id : u.resolved_by) ids += " ^" + id;
        std::string text = u.key + " " + u.description + " (blockers done:" + ids + ")";
        note(o, res, "unblocked " + text);
        res.unblocked.push_back(text);
    }
}

void Loop::record_org_issues(DispatchOutcome& out) {
    if (out.report.org_issues.empty()) return;
    std::string task;
    if (Plan plan = Plan::load(project_.plan_path()); const TaskRef* r = plan.find(out.plan.task_key))
        task = r->task->id.value_or("");
    FeedbackResult fr = record_feedback(project_, out.report.org_issues, {out.plan.member->handle, task, ""});
    if (!fr.error.empty()) out.problems.push_back(fr.error);
    for (const auto& id : fr.added) out.feedback.push_back("new ^" + id);
    for (const auto& id : fr.merged) out.feedback.push_back("reported again ^" + id);
}

void Loop::apply_report(DispatchOutcome& out, const LoopOptions& o) {
    Plan plan = Plan::load(project_.plan_path());
    const TaskRef* ref = plan.find(out.plan.task_key);
    if (!ref) {
        out.problems.push_back("the task " + out.plan.task_key + " is no longer in the plan");
        return;
    }
    const std::string id = ref->task->id.value_or("");
    const Member& who = *out.plan.member;
    const std::string date = iso_timestamp().substr(0, 10);
    PlanEditor ed(plan);
    const AgentReport& rep = out.report;
    const bool blocked = (rep.status && *rep.status == TaskStatus::Blocked) || !rep.blockers.empty();
    // With a Gatekeeper, done means "for the Gatekeeper to check": it accepts or sends it back.
    const bool gated = !team_.gatekeeper.empty() && who.handle != team_.gatekeeper && who.handle != team_.coach &&
                       rep.status && (*rep.status == TaskStatus::Completed || *rep.status == TaskStatus::Review) &&
                       std::find(ref->task->tags.begin(), ref->task->tags.end(), "no-gate") == ref->task->tags.end();

    auto block_on_operator = [&](const std::string& why) {
        BlockerReport b;
        b.kind = BlockerReport::Kind::Operator;
        b.text = why;
        BlockerEdits be = record_blockers(plan, ed, *ref, who, team_, {b}, "");
        out.plan_changes.insert(out.plan_changes.end(), be.changes.begin(), be.changes.end());
    };

    if (out.run.ok && blocked) {
        BlockerEdits be = record_blockers(plan, ed, *ref, who, team_, rep.blockers, rep.summary);
        out.plan_changes.insert(out.plan_changes.end(), be.changes.begin(), be.changes.end());
        out.problems.insert(out.problems.end(), be.problems.begin(), be.problems.end());
    } else if (out.run.ok && rep.status && out.guard_hold &&
               (*rep.status == TaskStatus::Completed || *rep.status == TaskStatus::Review)) {
        // Reported done, but the safeguards undid or flagged part of it.
        ed.set_status(*ref, TaskStatus::InProgress);
        std::string why;
        for (const auto& g : out.guard)
            if (starts_with(g, "reverted") || starts_with(g, "flagged")) why += (why.empty() ? "" : "; ") + g;
        ed.add_note(*ref, "Guard " + date + ": not done, " + first_line(why, 300));
        out.plan_changes.push_back("status → " + status_label(TaskStatus::InProgress) + " (the guard held it)");
    } else if (out.run.ok && gated) {
        ed.set_status(*ref, TaskStatus::Review);
        ed.add_note(*ref, "Done by @" + who.handle + " " + date + ", for the Gatekeeper: " +
                              (rep.summary.empty() ? std::string("(no summary)") : first_line(rep.summary, 600)));
        out.plan_changes.push_back("status → " + status_label(TaskStatus::Review) + ", for the Gatekeeper @" +
                                   team_.gatekeeper);
    } else if (out.run.ok && rep.status && *rep.status == TaskStatus::Review &&
               !reviewer_problem(team_, who.handle, rep.reviewer).empty()) {
        // Review is not a waiting state: with no reviewer (or the author as one) nobody picks it up.
        const std::string why = reviewer_problem(team_, who.handle, rep.reviewer);
        ed.set_status(*ref, TaskStatus::InProgress);
        ed.add_note(*ref, "Not in review " + date + ": " + why);
        out.plan_changes.push_back("status → " + status_label(TaskStatus::InProgress) + " (review names no reviewer)");
        out.problems.push_back("review refused: " + why);
    } else if (out.run.ok && rep.status) {
        ed.set_status(*ref, *rep.status);
        if (*rep.status == TaskStatus::Review) ed.add_note(*ref, review_note(rep.reviewer, who.handle, date));
        out.plan_changes.push_back("status → " + status_label(*rep.status) +
                                   (*rep.status == TaskStatus::Review ? ", reviewer @" + rep.reviewer : ""));
    } else if (out.run.ok) {
        // Finished, but said nothing we can use.
        out.failure = "no-report";
        out.problems.push_back(rep.status_text.empty() ? "no <task_status> in the agent's report"
                                                       : "unknown task status \"" + rep.status_text + "\"");
        int n = consecutive_failures(out.plan.task_key) + 1;
        if (n >= o.max_failures)
            block_on_operator("Check why @" + who.handle + " reports no status for ^" + id + " (" +
                              std::to_string(n) + " runs in a row)");
    } else {
        out.failure = to_string(out.run.failure);
        const std::string err = first_line(out.run.error, 160);
        switch (out.run.failure) {
            case Failure::NotAvailable:
            case Failure::Auth:
            case Failure::RateLimit:
                // Not the task's problem: put it back as it was and skip the runner.
                if (out.plan.previous_status != TaskStatus::InProgress) ed.set_status(*ref, out.plan.previous_status);
                out.stop_runner = true;
                out.problems.push_back(std::string(out.run.failure == Failure::RateLimit ? "rate limited" :
                                                   out.run.failure == Failure::Auth      ? "not logged in" :
                                                                                           "cannot run") +
                                       " (" + out.plan.model.runner + "): " + err);
                break;
            case Failure::Limit:
                ed.add_note(*ref, "Run by @" + who.handle + " stopped at a limit " + date + " (" + err +
                                      "); it continues next round");
                out.plan_changes.push_back("note: stopped at a limit; continues next round");
                break;
            default: {
                int n = consecutive_failures(out.plan.task_key) + 1;
                out.problems.push_back("run failed: " + out.run.error);
                if (n >= o.max_failures) {
                    block_on_operator("Find out why runs of ^" + id + " fail (" + std::to_string(n) +
                                      " in a row): " + err);
                } else {
                    ed.add_note(*ref, "Run by @" + who.handle + " failed " + date + ": " + err);
                    out.plan_changes.push_back("note: the run failed");
                }
                break;
            }
        }
    }

    if (out.run.ok)
        for (const CriterionReport& c : rep.criteria) {
            if (c.number < 1 || static_cast<std::size_t>(c.number) > ref->task->criteria.size()) {
                out.problems.push_back("no criterion " + std::to_string(c.number) + " in the task (it has " +
                                       std::to_string(ref->task->criteria.size()) + ")");
                continue;
            }
            std::optional<std::string> ev;
            if (!c.evidence.empty()) ev = first_line(c.evidence, 240);
            ed.set_criterion(*ref, static_cast<std::size_t>(c.number - 1), c.status, ev);
            out.plan_changes.push_back("criterion " + std::to_string(c.number) + " → " +
                                       taskml::criterion_marker_ascii(c.status));
        }

    std::vector<std::pair<std::string, std::string>> handed;  // (member, title) made in this report
    for (const HandoffRequest& h : rep.handoffs) {
        const std::string to_handle = handoff_target(team_, h.to);
        if (to_handle.empty()) {
            out.problems.push_back("handoff to unknown member @" + h.to);
            continue;
        }
        const bool waits = handoff_waits_for_sender(team_, to_handle);
        NewTask nt;
        auto [title, more] = title_and_rest(first_line(h.instructions, 1000));
        // The same job, already theirs and open: one task, with a note, not two.
        const TaskRef* same = nullptr;
        for (const TaskRef& r : plan.tasks())
            if (r.task->assignee && normalize_handle(*r.task->assignee) == to_handle &&
                (is_open(r.task->status) || r.task->status == TaskStatus::Blocked) && r.task != ref->task &&
                same_subject(r.task->description, title)) {
                same = &r;
                break;
            }
        if (same) {
            ed.add_note(*same, "Also handed off by @" + who.handle + (id.empty() ? "" : " from ^" + id) + ": " +
                                   first_line(h.instructions, 300));
            out.plan_changes.push_back("handoff to @" + to_handle + ": the same job as " + same->key() + " (noted there)");
            continue;
        }
        if (std::any_of(handed.begin(), handed.end(),
                        [&](const auto& x) { return x.first == to_handle && same_subject(x.second, title); }))
            continue;  // twice in one report
        handed.emplace_back(to_handle, title);
        nt.description = title;
        if (!more.empty()) nt.notes.push_back(more);
        nt.assignee = to_handle;
        nt.priority = ref->task->priority;
        nt.tags = inherited_tags(*ref->task);
        if (!id.empty() && waits) nt.depends_on.push_back(id);
        // All of it (a handoff that cites a list must carry the list), up to a
        // point; beyond it, where the rest is.
        auto lines = split_lines(h.instructions);
        std::size_t i = 1;
        for (; i < lines.size() && nt.notes.size() < 40; ++i) {
            if (auto c = criterion_line(lines[i])) nt.criteria.push_back(*c);
            else if (!trim(lines[i]).empty()) nt.notes.emplace_back(trim(lines[i]));
        }
        if (i < lines.size()) {
            std::error_code ec;
            nt.notes.push_back("... " + std::to_string(lines.size() - i) + " more lines: the handoff in " +
                               fs::relative(out.run_dir / "result.md", project_.dir(), ec).string());
        }
        nt.notes.push_back("Handed off by @" + who.handle + (waits || id.empty() ? "" : " from ^" + id + ", which does not wait for it"));
        std::string new_id = ed.add_sibling(*ref, nt);
        out.plan_changes.push_back("handoff ^" + new_id + " → @" + to_handle);
        if (nt.criteria.empty())
            out.problems.push_back("handoff ^" + new_id + " has no acceptance criteria (accept: lines): @" + to_handle +
                                   " cannot tell when it is done");
    }
    if (out.run.ok) {
        std::vector<std::string> added;
        for (const std::string& f : rep.findings) {
            const std::string fid = record_finding(plan, ed, *ref, who, f, &added, &team_);
            const std::string to = finding_address(f, &team_);
            if (!fid.empty()) out.plan_changes.push_back("finding ^" + fid + " → @" + (to.empty() ? std::string(kOperator) : to));
        }
        // A site the member needs: the operator's to add to the web list (it does not block).
        for (const auto& [site, why] : rep.web_access) {
            const std::string fid = record_finding(plan, ed, *ref, who, "operator: web access to " + site + ": " + why, &added, &team_);
            if (!fid.empty()) out.plan_changes.push_back("web access " + site + " ^" + fid + " → @" + std::string(kOperator));
        }
    }
    // What the member learned: its memory, for its next tasks (the coach sees it too).
    if (out.run.ok && !rep.lessons.empty()) {
        out.lessons = add_lessons(project_, who.handle, id, rep.lessons, date);
        for (const auto& l : out.lessons) out.plan_changes.push_back("lesson for @" + who.handle + ": " + first_line(l, 100));
    }
    // What the run committed may finish other open tasks: their assignees are asked.
    for (auto& c : note_tasks_commits_name(plan, ed, out.commits, id, who.handle, date))
        out.plan_changes.push_back(std::move(c));
    if (!ed.changed()) return;
    CommitResult c = ed.commit();
    if (!c.ok) {
        out.problems.push_back("plan not updated: " + c.error);
        for (const auto& d : c.diagnostics) out.problems.push_back("  " + d.format());
        out.plan_changes.clear();
        return;
    }
    // Finishing this task may unblock others: tell them how it was resolved.
    if (out.run.ok && rep.status && is_finished(*rep.status) && !id.empty()) {
        IterationResult scratch;
        std::string why = "@" + who.handle + (rep.summary.empty() ? "" : ": " + first_line(rep.summary, 160));
        unblock(scratch, o, {{id, why}});
        for (const auto& u : scratch.unblocked) out.plan_changes.push_back("unblocked " + u);
    }
}

std::string Loop::changelog_entry(const DispatchOutcome& out) const {
    const DispatchPlan& dp = out.plan;
    std::string e = "## " + iso_timestamp() + " · " + dp.description + " (" + dp.task_key + ")\n\n";
    e += "- Team: " + team_.name + "; member: " + dp.member->name + " (@" + dp.member->handle + "), " + dp.model.str() + "\n";
    if (dp.reason != "assigned") e += "- Allocation: " + dp.reason + "\n";
    std::string result;
    if (out.report.status) result = status_label(*out.report.status);
    else if (!out.run.ok) result = "failed: " + first_line(out.run.error, 200);
    else result = "no status reported";
    e += "- Result: " + result + "\n";
    for (const auto& c : out.plan_changes) e += "- Plan: " + c + "\n";
    for (const auto& p : out.problems) e += "- Problem: " + p + "\n";
    for (const auto& f : out.feedback) e += "- Feedback for myteam: " + f + "\n";
    for (const auto& g : out.guard) e += "- Guard: " + g + "\n";
    for (const auto& c : out.commits)
        e += "- Commit: `" + c.commit.substr(0, 10) + "` on " + c.branch + ": " + c.summary + " (" +
             std::to_string(c.files.size()) + " files)\n";
    e += "- Usage: " + usage_line(out.run.usage) + "\n";
    std::error_code ec;
    e += "- Run: `" + fs::relative(out.run_dir, project_.dir(), ec).string() + "`\n";
    if (!out.report.summary.empty()) e += "\n" + out.report.summary + "\n";
    return e;
}

bool Loop::repair(IterationResult& res, const LoopOptions& o) {
    for (int attempt = 1; attempt <= o.max_repairs; ++attempt) {
        Plan plan = Plan::load(project_.plan_path());
        if (plan.ok()) return true;
        ++res.repair_attempts;
        const Member* who = team_.find(team_.repairer);
        if (!who) who = &team_.product_owner_member();
        DispatchPlan dp;
        dp.task_key = "repair";
        dp.description = "Repair the plan";
        dp.member = who;
        dp.model = who->models.standard;
        if (!o.runner.empty() && dp.model.runner != o.runner) dp.model = ModelRef{o.runner, ""};
        dp.reason = "the plan does not validate (" + std::to_string(plan.errors().size()) + " errors)";
        dp.context = build_repair_context({team_, *who, plan, project_.brief(), false, false, "", "", {}, "", ""}, plan.errors());
        note(o, res, "plan repair " + std::to_string(attempt) + "/" + std::to_string(o.max_repairs) + " by @" + who->handle);
        DispatchOutcome out;
        out.plan = dp;
        out.executed = true;
        out.run = run_agent(dp, o, out.run_dir);
        out.report = parse_report(out.run.final_text);
        if (!out.run.ok) out.problems.push_back("run failed: " + out.run.error);
        write_file_atomic(out.run_dir / "run.json", run_record(out).dump(2) + "\n");
        append_changelog(project_.changelog_path(), changelog_entry(out));
        res.dispatches.push_back(std::move(out));
    }
    Plan plan = Plan::load(project_.plan_path());
    if (plan.ok()) return true;
    res.needs_human = true;
    std::string msg = "the plan still does not validate after " + std::to_string(o.max_repairs) +
                      " repair attempts; a human is needed";
    note(o, res, msg);
    std::string e = "## " + iso_timestamp() + " · Human needed\n\n- " + msg + "\n";
    for (const auto& d : plan.errors()) e += "- " + d.format() + "\n";
    append_changelog(project_.changelog_path(), e);
    return false;
}

IterationResult Loop::iterate(const LoopOptions& o) {
    // The project's services run before the team works.
    if (!o.dry_run && !project_.config().services.empty()) {
        ServicesResult sr = ensure_services(project_);
        for (const auto& n : sr.notes)
            if (o.on_note) o.on_note(n);
        for (const auto& p : sr.problems)
            if (o.on_note) o.on_note("problem: " + p);
    }
    return task_round(o);
}

IterationResult Loop::task_round(const LoopOptions& o) {
    IterationResult res;
    {
        Plan plan = Plan::load(project_.plan_path());
        if (!plan.ok()) {
            if (o.dry_run) {
                note(o, res, "the plan does not validate; a real run would send it to repair first");
                res.needs_human = true;
                return res;
            }
            if (!repair(res, o)) return res;
        }
    }

    // Blockers that were finished since the last round (by an agent, or by
    // hand) release their tasks.
    if (!o.dry_run) unblock(res, o);

    Plan plan = Plan::load(project_.plan_path());
    Selection sel = select(plan, o);
    res.waiting = sel.waiting;
    if (sel.keys.empty()) {
        note(o, res, o.only_task.empty() ? "no task is ready for the team"
                                         : "task " + o.only_task + " is not assigned to a member of the team");
        return res;
    }

    if (o.dry_run) {
        for (DispatchPlan& dp : preview(o)) {
            DispatchOutcome out;
            out.plan = std::move(dp);
            res.dispatches.push_back(std::move(out));
        }
        return res;
    }

    // Give each task an id to track it by, and record the allocation.
    std::vector<std::string> ids;
    {
        PlanEditor ed(plan);
        for (const std::string& key : sel.keys) {
            const TaskRef* t = plan.find(key);
            if (!t) continue;
            ids.push_back(ed.ensure_id(*t));
        }
        CommitResult c = ed.commit();
        if (!c.ok) {
            note(o, res, "cannot prepare the plan: " + c.error);
            res.needs_human = true;
            return res;
        }
    }

    std::set<std::string> stopped;  // runners that cannot work this round
    for (const std::string& id : ids) {
        Plan current = Plan::load(project_.plan_path());
        const TaskRef* t = current.by_id(id);
        if (!t || !is_open(t->task->status)) {
            note(o, res, "^" + id + " is no longer open; skipped");
            continue;
        }
        DispatchPlan dp = plan_dispatch(current, *t, o, "assigned");
        if (stopped.count(dp.model.runner)) {
            note(o, res, "^" + id + " skipped: " + dp.model.runner + " cannot run now");
            continue;
        }
        dp.previous_status = t->task->status;
        dp.resume_session = limit_session(dp);
        if (o.confirm && !o.confirm(dp)) {
            DispatchOutcome out;
            out.plan = std::move(dp);
            out.skipped = true;
            res.dispatches.push_back(std::move(out));
            continue;
        }
        if (t->task->status == TaskStatus::Pending) {
            PlanEditor ed(current);
            ed.set_status(*t, TaskStatus::InProgress);
            ed.commit();
        }
        DispatchOutcome out = execute(std::move(dp), o);
        if (out.stop_runner) {
            stopped.insert(out.plan.model.runner);
            if (out.run.failure != Failure::RateLimit) res.needs_human = true;
        }
        res.dispatches.push_back(std::move(out));
    }

    if (!Plan::load(project_.plan_path()).ok()) repair(res, o);
    // What still waits, as the round leaves the plan.
    Plan end = Plan::load(project_.plan_path());
    LoopOptions w;
    w.skip_members = o.skip_members;
    res.waiting = select(end, w).waiting;
    return res;
}

}  // namespace myteam
