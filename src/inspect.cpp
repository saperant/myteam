#include "myteam/inspect.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <filesystem>

#include "myteam/blockers.hpp"
#include "myteam/coach.hpp"
#include "myteam/feedback.hpp"
#include "myteam/memory.hpp"
#include "myteam/plan.hpp"
#include "myteam/runner.hpp"
#include "myteam/services.hpp"
#include "myteam/skills.hpp"
#include "myteam/util.hpp"

namespace myteam::inspect {

namespace fs = std::filesystem;
using taskml::Json;
using taskml::TaskStatus;

namespace {

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

std::optional<Json> read_json(const fs::path& p) {
    auto text = try_read_file(p);
    if (!text) return std::nullopt;
    try {
        return Json::parse(*text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string str(const Json* j, std::string_view k) {
    const Json* v = j ? j->find(k) : nullptr;
    return v && v->is_string() ? v->as_string() : std::string();
}
double num(const Json* j, std::string_view k) {
    const Json* v = j ? j->find(k) : nullptr;
    return v && v->is_number() ? v->as_number() : 0.0;
}
bool flag(const Json* j, std::string_view k) {
    const Json* v = j ? j->find(k) : nullptr;
    return v && v->is_bool() && v->as_bool();
}

const char* status_word(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending: return "pending";
        case TaskStatus::InProgress: return "in_progress";
        case TaskStatus::Completed: return "done";
        case TaskStatus::Blocked: return "blocked";
        case TaskStatus::Cancelled: return "cancelled";
        case TaskStatus::Review: return "review";
    }
    return "pending";
}

const char* criterion_word(taskml::CriterionStatus s) {
    switch (s) {
        case taskml::CriterionStatus::Verified: return "verified";
        case taskml::CriterionStatus::Failed: return "failed";
        default: return "pending";
    }
}

// "20261106-225223-..." -> "2026-11-06T22:52:23Z" (file_timestamp() is UTC).
std::string started_from_name(const std::string& name) {
    if (name.size() < 15 || name[8] != '-') return "";
    for (int i : {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14})
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return "";
    return name.substr(0, 4) + "-" + name.substr(4, 2) + "-" + name.substr(6, 2) + "T" + name.substr(9, 2) + ":" +
           name.substr(11, 2) + ":" + name.substr(13, 2) + "Z";
}

bool alive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

/// The kind of a run, from what it was for.
std::string kind_of(const std::string& what) {
    if (what == "coach") return "coach";
    if (what == "gatekeeper") return "review";
    if (starts_with(what, "gate:")) return "check";
    if (starts_with(what, "council:")) return "council";
    if (what == "repair") return "repair";
    if (starts_with(what, "^")) return "task";
    if (what == "steward" || starts_with(what, "ceremony:") || starts_with(what, "customer:")) return "scrum";
    return what.empty() ? "other" : "task";
}

Json run_summary(const fs::path& dir) {
    Json j = Json::object();
    const std::string name = dir.filename().string();
    j.set("name", name);
    j.set("dir", dir.string());
    auto meta = read_json(dir / "meta.json");
    auto record = read_json(dir / "run.json");
    auto guard = read_json(dir / "guard.json");
    const Json* m = meta ? &*meta : nullptr;
    const Json* r = record ? &*record : nullptr;
    std::string what = str(m, "what");
    if (what.empty()) what = str(r, "task");
    j.set("what", what);
    j.set("kind", kind_of(what));
    j.set("description", str(r, "description"));
    std::string member = str(m, "member");
    if (member.empty()) member = str(r, "member");
    j.set("member", member);
    std::string model = str(r, "model");
    if (model.empty() && m) model = str(m, "runner") + (str(m, "model").empty() ? "" : ":" + str(m, "model"));
    j.set("model", model);
    std::string started = str(m, "started");
    if (started.empty()) started = started_from_name(name);
    j.set("started", started);
    j.set("finished", str(m, "finished"));
    // running (the runner's process is alive and it has not finished), interrupted, done.
    std::string state = "done";
    if (m && str(m, "finished").empty() && !r) state = alive(static_cast<int>(num(m, "pid"))) ? "running" : "interrupted";
    if (!m && !r && !fs::exists(dir / "result.md")) state = "unknown";
    j.set("state", state);
    bool ok = r ? flag(r, "ok") : m ? flag(m, "ok") : !trim(try_read_file(dir / "result.md").value_or("")).empty();
    j.set("ok", ok);
    std::string failure = str(r, "failure");
    if (failure.empty()) failure = str(m, "failure");
    j.set("failure", failure.empty() ? "none" : failure);
    double cost = r ? num(r->find("usage"), "cost_usd") : num(m, "cost_usd");
    if (cost == 0 && m) cost = num(m, "cost_usd");
    j.set("cost_usd", cost);
    j.set("turns", static_cast<int>(r ? num(r->find("usage"), "turns") : num(m, "turns")));
    const Json* rep = r ? r->find("report") : nullptr;
    j.set("status", str(rep, "status"));
    j.set("summary", first_line(str(rep, "summary"), 200));
    int commits = 0;
    if (const Json* cs = r ? r->find("commits") : nullptr; cs && cs->is_array()) commits = static_cast<int>(cs->as_array().size());
    j.set("commits", commits);
    int guard_n = 0;
    if (const Json* g = guard ? guard->find("guard") : nullptr; g && g->is_array()) guard_n = static_cast<int>(g->as_array().size());
    j.set("guard", guard_n);
    j.set("hold", guard ? flag(&*guard, "hold") : false);
    j.set("has_stream", fs::exists(dir / "stream.jsonl"));
    return j;
}

std::vector<fs::path> run_dirs(const Project& p) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.runs_dir(), ec))
        if (e.is_directory()) out.push_back(e.path());
    std::sort(out.rbegin(), out.rend());
    return out;
}

Json task_json(const Plan& plan, const TaskRef& r) {
    const auto& t = *r.task;
    Json j = Json::object();
    j.set("key", r.key());
    j.set("id", t.id.value_or(""));
    j.set("line", t.line);
    j.set("depth", r.depth);
    j.set("section", r.sections.empty() ? "" : r.sections.back());
    j.set("status", status_word(t.status));
    j.set("marker", std::string(1, taskml::status_marker(t.status)));
    j.set("description", t.description);
    j.set("assignee", t.assignee.value_or(""));
    j.set("priority", t.priority ? Json(*t.priority) : Json(nullptr));
    j.set("estimate", t.estimate ? "~" + *t.estimate : "");
    j.set("due", t.due.value_or(""));
    j.set("tags", strings(t.tags));
    j.set("depends", strings(t.depends_on));
    j.set("blocked_by", strings(t.blocked_by));
    Json unmet = Json::array();
    for (const auto& d : plan.unmet_dependencies(r)) unmet.push_back(d);
    j.set("waiting_on", unmet);
    Json active = Json::array();
    for (const auto& b : plan.active_blockers(r)) active.push_back(b);
    j.set("blockers", active);
    Json cs = Json::array();
    for (const auto& c : t.criteria) {
        Json cj = Json::object();
        cj.set("text", c.description);
        cj.set("status", criterion_word(c.status));
        cj.set("evidence", c.evidence.value_or(""));
        cj.set("line", c.line);
        cs.push_back(cj);
    }
    j.set("criteria", cs);
    j.set("notes", strings(t.notes));
    j.set("subtasks", static_cast<int>(t.subtasks.size()));
    return j;
}

std::string role_kind(const Team& t, const Member& m) {
    if (m.handle == t.coach) return "coach";
    if (m.handle == t.gatekeeper) return "gatekeeper";
    return "expert";
}

}  // namespace

Json plan(const Project& project) {
    Json j = Json::object();
    j.set("path", project.plan_path().string());
    if (!fs::exists(project.plan_path())) {
        j.set("tasks", Json::array());
        j.set("errors", Json::array());
        return j;
    }
    Plan p = Plan::load(project.plan_path());
    j.set("project", p.directive("project", project.dir().filename().string()));
    j.set("goal", p.directive("goal"));
    Json tasks = Json::array();
    for (const TaskRef& r : p.tasks()) tasks.push_back(task_json(p, r));
    j.set("tasks", tasks);
    Json errs = Json::array();
    for (const auto& d : p.errors()) {
        Json e = Json::object();
        e.set("line", d.line);
        e.set("message", d.message);
        errs.push_back(e);
    }
    j.set("errors", errs);
    return j;
}

Json runs(const Project& project, int max) {
    Json list = Json::array();
    int n = 0;
    for (const fs::path& d : run_dirs(project)) {
        if (max > 0 && n++ >= max) break;
        list.push_back(run_summary(d));
    }
    Json j = Json::object();
    j.set("runs", list);
    return j;
}

Json run(const Project& project, const std::string& name) {
    const fs::path dir = project.runs_dir() / name;
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos || !fs::is_directory(dir))
        throw std::runtime_error("no run " + name);
    Json j = run_summary(dir);
    if (auto r = read_json(dir / "run.json")) j.set("record", *r);
    if (auto g = read_json(dir / "guard.json")) j.set("guard_record", *g);
    if (auto m = read_json(dir / "meta.json")) j.set("meta", *m);
    j.set("result", try_read_file(dir / "result.md").value_or(""));
    std::string err = try_read_file(dir / "stderr.log").value_or("");
    if (err.size() > 4000) err = err.substr(err.size() - 4000);
    j.set("stderr", err);
    j.set("prompt_path", (dir / "prompt.md").string());
    return j;
}

Json events(const Project& project, const std::string& name, std::size_t from) {
    const fs::path dir = project.runs_dir() / name;
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos || !fs::is_directory(dir))
        throw std::runtime_error("no run " + name);
    auto meta = read_json(dir / "meta.json");
    std::string runner = meta ? str(&*meta, "runner") : "";
    if (runner.empty()) {
        auto r = read_json(dir / "run.json");
        std::string model = r ? str(&*r, "model") : "";
        runner = model.substr(0, model.find(':'));
    }
    if (runner.empty()) runner = "claude";
    Json list = Json::array();
    std::size_t count = 0;
    auto add = [&](const AgentEvent& e) {
        if (count++ < from) return;
        const char* kind = "text";
        switch (e.kind) {
            case AgentEvent::Kind::Started: kind = "started"; break;
            case AgentEvent::Kind::Text: kind = "text"; break;
            case AgentEvent::Kind::ToolCall: kind = "tool_call"; break;
            case AgentEvent::Kind::ToolResult: kind = "tool_result"; break;
            case AgentEvent::Kind::Usage: kind = "usage"; break;
            case AgentEvent::Kind::Error: kind = "error"; break;
            case AgentEvent::Kind::Finished: kind = "finished"; break;
        }
        Json ej = Json::object();
        ej.set("kind", kind);
        ej.set("tool", e.tool);
        std::string text = e.text;
        if (text.size() > 20000) text = text.substr(0, 20000) + "\n…";
        ej.set("text", text);
        list.push_back(ej);
    };
    if (auto r = make_runner(runner, project.runner_settings(runner))) {
        StreamState state;
        for (const auto& line : split_lines(try_read_file(dir / "stream.jsonl").value_or("")))
            if (!trim(line).empty()) {
                try {
                    r->parse_line(line, state, add);
                } catch (const std::exception&) {
                    // A line still being written: the next read gets it.
                }
            }
    }
    Json j = Json::object();
    j.set("run", name);
    j.set("runner", runner);
    j.set("from", static_cast<long long>(from));
    j.set("count", static_cast<long long>(count));
    j.set("events", list);
    return j;
}

Json team(const Project& project, const Team& t) {
    std::optional<Plan> p;
    if (fs::exists(project.plan_path())) p = Plan::load(project.plan_path());
    Json all = runs(project, 0);
    Json members = Json::array();
    for (const Member& m : t.members) {
        Json j = Json::object();
        j.set("handle", m.handle);
        j.set("name", m.name);
        j.set("role", m.role);
        j.set("kind", role_kind(t, m));
        j.set("unit", m.unit);
        j.set("summary", m.summary);
        j.set("skills", strings(m.skills));
        j.set("prompt", m.prompt);
        j.set("file", m.source.string());
        Json models = Json::object();
        models.set("standard", m.models.standard.str());
        models.set("premium", m.models.premium ? m.models.premium->str() : "");
        models.set("economy", m.models.economy ? m.models.economy->str() : "");
        j.set("models", models);
        const Memory mem = load_memory(project, m.handle);
        Json mj = Json::object();
        mj.set("file", memory_path(project, m.handle).string());
        mj.set("coaching", mem.coaching);
        mj.set("notes", mem.other);
        mj.set("lessons", strings(mem.lessons));
        j.set("memory", mj);
        Json now = Json::array();
        int open = 0, review = 0, done = 0;
        if (p)
            for (const TaskRef& r : p->tasks()) {
                if (!r.task->assignee || normalize_handle(*r.task->assignee) != m.handle) continue;
                if (r.task->status == TaskStatus::InProgress) now.push_back(r.key());
                if (is_open(r.task->status) || r.task->status == TaskStatus::Blocked) ++open;
                if (r.task->status == TaskStatus::Review) ++review;
                if (r.task->status == TaskStatus::Completed) ++done;
            }
        j.set("in_progress", now);
        j.set("open", open);
        j.set("review", review);
        j.set("done", done);
        double cost = 0;
        int nruns = 0;
        Json last = nullptr, running = nullptr;
        for (const Json& r : all.find("runs")->as_array()) {
            if (str(&r, "member") != m.handle) continue;
            ++nruns;
            cost += num(&r, "cost_usd");
            if (last.is_null()) last = r;
            if (running.is_null() && str(&r, "state") == "running") running = r;
        }
        j.set("runs", nruns);
        j.set("cost_usd", cost);
        j.set("last_run", last);
        j.set("running", running);
        members.push_back(j);
    }
    Json j = Json::object();
    j.set("name", t.name);
    j.set("coach", t.coach);
    j.set("gatekeeper", t.gatekeeper);
    j.set("product_owner", t.product_owner_member().handle);
    j.set("members", members);
    return j;
}

Json conversation(const Project& project) {
    auto conv = load_conversation(project);
    const CoachState st = load_coach_state(project);
    Json list = Json::array();
    int last_coach = -1;
    for (std::size_t i = 0; i < conv.size(); ++i) {
        Json m = Json::object();
        m.set("who", conv[i].who);
        m.set("when", conv[i].when);
        m.set("text", conv[i].text);
        m.set("seen", static_cast<int>(i) < st.customer_seen);
        list.push_back(m);
        if (conv[i].who == "coach") last_coach = static_cast<int>(i);
    }
    Json j = Json::object();
    j.set("path", conversation_path(project).string());
    j.set("messages", list);
    // The coach wrote last: the customer owes the reply.
    j.set("waiting_for_you", !conv.empty() && conv.back().who == "coach");
    int unseen = 0;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, st.customer_seen)); i < conv.size(); ++i)
        unseen += conv[i].who == "customer";
    j.set("unseen_by_coach", unseen);
    (void)last_coach;
    return j;
}

Json coach(const Project& project, const Team& t) {
    const CoachState st = load_coach_state(project);
    Json j = Json::object();
    j.set("coach", t.coach);
    j.set("gatekeeper", t.gatekeeper);
    j.set("news", strings(st.news));
    j.set("objection", st.objection);
    j.set("held", st.held);
    j.set("idle", st.idle);
    j.set("steps", st.steps);
    j.set("decisions", st.decisions);
    j.set("milestones", st.milestones);
    j.set("cost_usd", st.cost);
    j.set("last_decision", st.last_decision);
    j.set("notes", t.coach.empty() ? std::string() : load_memory(project, t.coach).other);
    const auto& c = project.config().coach;
    Json s = Json::object();
    s.set("tasks_per_step", c.tasks_per_step);
    s.set("acceptances_per_step", c.acceptances_per_step);
    s.set("max_rejections", c.max_rejections);
    j.set("settings", s);
    return j;
}

Json digest(const Project& project, const Team& t) {
    Coach c(project, t, project_runners(project));
    const std::string text = c.coach_prompt();
    Json j = Json::object();
    j.set("text", text);
    j.set("tokens", static_cast<long long>(estimate_tokens(text)));
    return j;
}

Json council(const Project& project) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(project.state_dir() / "council", ec))
        if (e.path().extension() == ".md") files.push_back(e.path());
    std::sort(files.rbegin(), files.rend());
    Json list = Json::array();
    for (const auto& f : files) {
        Json j = Json::object();
        const std::string name = f.stem().string();
        j.set("name", name);
        j.set("path", f.string());
        j.set("started", started_from_name(name));
        std::string topic, question, section, body;
        Json views = Json::array();
        auto flush = [&] {
            if (section == "question") question = std::string(trim(body));
            else if (starts_with(section, "@")) {
                Json v = Json::object();
                v.set("who", section.substr(1));
                v.set("text", std::string(trim(body)));
                views.push_back(v);
            }
            body.clear();
        };
        for (const auto& l : split_lines(try_read_file(f).value_or(""))) {
            if (starts_with(l, "# Council: ")) topic = l.substr(11);
            else if (starts_with(l, "## ")) {
                flush();
                section = l.substr(3) == "The question" ? "question" : l.substr(3);
            } else if (!section.empty())
                body += l + "\n";
        }
        flush();
        j.set("topic", topic.empty() ? name : topic);
        j.set("question", question);
        j.set("views", views);
        list.push_back(j);
    }
    Json j = Json::object();
    j.set("council", list);
    return j;
}

Json skills(const Project& project) {
    Json list = Json::array();
    for (const Skill& s : load_skills(project)) {
        Json j = Json::object();
        j.set("slug", s.slug);
        j.set("name", s.name);
        j.set("description", s.description);
        j.set("triggers", strings(s.triggers));
        j.set("members", strings(s.members));
        j.set("source", s.source);
        j.set("file", s.file.string());
        j.set("body", s.body);
        list.push_back(j);
    }
    Json j = Json::object();
    j.set("path", skills_dir(project).string());
    j.set("skills", list);
    return j;
}

Json feedback(const Project& project) {
    Json list = Json::array();
    for (const FeedbackItem& f : load_feedback(project)) {
        Json j = Json::object();
        j.set("id", f.id);
        j.set("area", f.area);
        j.set("title", f.title);
        j.set("status", f.status);
        j.set("reports", f.reports);
        j.set("reporters", strings(f.reporters));
        j.set("notes", strings(f.notes));
        list.push_back(j);
    }
    Json j = Json::object();
    j.set("path", project.feedback_path().string());
    j.set("feedback", list);
    return j;
}

Json overview(const Project& project, const Team& t) {
    Json j = Json::object();
    j.set("dir", project.dir().string());
    j.set("team", t.name);
    j.set("methodology", project.config().methodology);
    Json counts = Json::object();
    for (const char* k : {"pending", "in_progress", "review", "blocked", "done", "cancelled"}) counts.set(k, 0);
    std::string project_name = project.dir().filename().string();
    std::string goal;
    Json needs = Json::array();  // what needs you, one line each
    auto need = [&](std::string kind, std::string text, std::string ref) {
        Json n = Json::object();
        n.set("kind", std::move(kind));
        n.set("text", std::move(text));
        n.set("ref", std::move(ref));
        needs.push_back(n);
    };
    int ready = 0, errors = 0, unassigned = 0, review = 0;
    if (fs::exists(project.plan_path())) {
        Plan p = Plan::load(project.plan_path());
        project_name = p.directive("project", project_name);
        goal = p.directive("goal");
        for (const TaskRef& r : p.tasks()) {
            const char* w = status_word(r.task->status);
            counts.set(w, static_cast<int>(num(&counts, w)) + 1);
            if (!r.task->assignee && !is_finished(r.task->status)) ++unassigned;
            if (r.task->status == TaskStatus::Review) ++review;
        }
        for (const TaskRef* r : ready_assigned_tasks(p, t))
            if (!r->task->assignee || (normalize_handle(*r->task->assignee) != t.coach &&
                                       normalize_handle(*r->task->assignee) != t.gatekeeper))
                ++ready;
        errors = static_cast<int>(p.errors().size());
        BlockerReview rv = review_blockers(p);
        for (const TaskRef* r : rv.for_operator) need("operator", r->task->description, r->key());
        if (errors) need("plan", std::to_string(errors) + " error(s) in the plan", "");
    }
    j.set("project", project_name);
    j.set("goal", goal);
    j.set("counts", counts);
    j.set("ready", ready);
    j.set("unassigned", unassigned);
    j.set("in_review", review);
    j.set("plan_errors", errors);
    auto conv = load_conversation(project);
    if (!conv.empty() && conv.back().who == "coach")
        need("customer", "the coach wrote to you: " + first_line(conv.back().text, 160), "");
    const CoachState st = load_coach_state(project);
    Json cj = Json::object();
    cj.set("steps", st.steps);
    cj.set("decisions", st.decisions);
    cj.set("milestones", st.milestones);
    cj.set("cost_usd", st.cost);
    cj.set("news", static_cast<int>(st.news.size()));
    cj.set("objection", st.objection);
    cj.set("last_decision", st.last_decision);
    cj.set("idle", st.idle);
    j.set("coach", cj);
    Json latest = runs(project, 12);
    Json running = Json::array();
    double total = 0;
    for (const fs::path& d : run_dirs(project)) {
        Json r = run_summary(d);
        total += num(&r, "cost_usd");
        if (!fs::exists(d / "result.md") && str(&r, "state") == "running") running.push_back(r);
    }
    for (const Json& r : latest.find("runs")->as_array())
        if (flag(&r, "hold")) need("guard", "run held by the safeguards: " + str(&r, "what"), str(&r, "name"));
    j.set("running", running);
    j.set("latest", *latest.find("runs"));
    j.set("cost_usd", total);
    j.set("needs_you", needs);
    if (!project.config().services.empty()) j.set("services", *services_json(project).find("services"));
    int open_feedback = 0;
    for (const FeedbackItem& f : load_feedback(project)) open_feedback += f.status == "new";
    j.set("feedback_new", open_feedback);
    return j;
}

}  // namespace myteam::inspect
