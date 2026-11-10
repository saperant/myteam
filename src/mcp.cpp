#include "myteam/mcp.hpp"

#include <algorithm>
#include <filesystem>
#include <istream>
#include <ostream>

#include "myteam/blockers.hpp"
#include "myteam/commits.hpp"
#include "myteam/services.hpp"
#include "myteam/context.hpp"
#include "myteam/plan.hpp"
#include "myteam/skills.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Json;
using taskml::TaskStatus;

namespace {

std::string str_arg(const Json& args, std::string_view key) {
    const Json* v = args.find(key);
    if (!v) return {};
    if (v->is_string()) return std::string(trim(v->as_string()));
    if (v->is_number()) return std::to_string(static_cast<long long>(v->as_number()));
    return {};
}

std::string strip_caret(std::string s) {
    if (!s.empty() && s[0] == '^') s.erase(0, 1);
    return s;
}

Json text_result(const std::string& text, bool error = false) {
    Json c = Json::object();
    c.set("type", "text");
    c.set("text", text);
    Json r = Json::object();
    r.set("content", Json::array({c}));
    r.set("isError", error);
    return r;
}

Json error_result(const std::string& text) { return text_result(text, true); }

Json schema(std::initializer_list<std::pair<const char*, Json>> props, std::initializer_list<const char*> required) {
    Json p = Json::object();
    for (const auto& [k, v] : props) p.set(k, v);
    Json s = Json::object();
    s.set("type", "object");
    s.set("properties", p);
    Json req = Json::array();
    for (const char* r : required) req.push_back(r);
    s.set("required", req);
    s.set("additionalProperties", false);
    return s;
}

Json prop(const char* type, const char* description, std::initializer_list<const char*> values = {}) {
    Json p = Json::object();
    p.set("type", type);
    p.set("description", description);
    if (values.size()) {
        Json e = Json::array();
        for (const char* v : values) e.push_back(v);
        p.set("enum", e);
    }
    return p;
}

Json tool(const char* name, const char* description, Json input) {
    Json t = Json::object();
    t.set("name", name);
    t.set("description", description);
    t.set("inputSchema", std::move(input));
    // Reading tools change nothing; reporting tools only record for the end
    // of the run (nothing is destroyed, and the plan is not written yet).
    const std::string_view n(name);
    const bool reads = n == "plan_get_task" || n == "plan_list_tasks" || n == "plan_team" || n == "service_status" ||
                       n == "service_logs" || n == "skill_read";
    Json ann = Json::object();
    ann.set("readOnlyHint", reads);
    ann.set("destructiveHint", n == "service_reset");  // a service's data goes
    ann.set("idempotentHint", reads);
    ann.set("openWorldHint", false);
    t.set("annotations", ann);
    return t;
}

std::string one_liner(const TaskRef& r) {
    const auto& t = *r.task;
    std::string s = std::string("[") + taskml::status_marker(t.status) + "] " + t.description + " " + r.key();
    if (t.assignee) s += " @" + *t.assignee;
    if (t.priority) s += " #p" + std::to_string(*t.priority);
    return s;
}

// An escalation to the operator (a blocker or a finding) on the same subject as
// an open @operator task: the refusal names that task, so the unit waits for it
// instead of filing a second one the operator has to merge.
const TaskRef* open_operator_task(const Plan& plan, std::string_view text) {
    const std::string title = first_line(std::string(text), 300);
    for (const TaskRef& r : plan.tasks())
        if (is_operator_task(*r.task) && !is_finished(r.task->status) && same_subject(r.task->description, title))
            return &r;
    return nullptr;
}

std::string already_open(const TaskRef& r, bool blocker) {
    const std::string id = r.task->id ? "^" + *r.task->id : r.key();
    return "Not recorded: the operator already has this open: " + one_liner(r) + ". " +
           (blocker ? "If it blocks you, report plan_report_blocker on=\"" + id + "\" instead."
                    : "It is a task for the operator already; your task goes on.") +
           " If yours is a different matter, say in the first line how it differs.";
}

std::string handles(const Team& team) {
    std::string out;
    for (const Member& m : team.members) out += (out.empty() ? "@" : ", @") + m.handle;
    return out;
}

const char* criterion_word(taskml::CriterionStatus s) {
    switch (s) {
        case taskml::CriterionStatus::Verified: return "verified";
        case taskml::CriterionStatus::Failed: return "failed";
        default: return "pending";
    }
}

}  // namespace

McpServer::McpServer(McpSession session) : s_(std::move(session)) {
    if (!s_.report_path.empty() && fs::exists(s_.report_path)) {
        try {
            report_ = report_from_json(Json::parse(read_file(s_.report_path)));
        } catch (const std::exception&) {
        }
    }
}

void McpServer::save() const {
    if (s_.report_path.empty()) return;
    fs::create_directories(s_.report_path.parent_path());
    write_file_atomic(s_.report_path, report_to_json(report_).dump(2) + "\n");
}

Json McpServer::tools() const {
    Json list = Json::array();
    list.push_back(tool("plan_get_task",
                        "Your task (or another, by ^id) as the plan has it now: notes, numbered acceptance "
                        "criteria, dependencies, blockers.",
                        schema({{"task", prop("string", "A task id like ^api; default: your task")}}, {})));
    list.push_back(tool("plan_team", "The team: handles, names, roles; who to hand off to.", schema({}, {})));
    list.push_back(tool("plan_report_status",
                        "Report the status of your task and what you did. Call it when you finish (or stop). "
                        "myteam records it in the plan when your run ends.",
                        schema({{"status", prop("string", "done: complete; review: done, needs review; in_progress: "
                                                          "more work remains; blocked: you cannot continue "
                                                          "(also report each blocker)",
                                                {"done", "review", "in_progress", "blocked"})},
                                {"summary", prop("string", "What you did, key decisions, files changed")},
                                {"reviewer", prop("string", "For review: who reviews it, a teammate's handle, another "
                                                            "team's or operator; not you")}},
                               {"status", "summary"})));
    list.push_back(tool("plan_handoff",
                        "Hand part of the work on: a teammate's task, after yours; or a task for another team of the "
                        "org (the org carries it to them) or the operator, which neither waits for yours nor blocks it.",
                        schema({{"to", prop("string", "A teammate's handle, another team's handle, or operator")},
                                {"instructions", prop("string", "First line: the task. Then what to deliver, where. Then one line "
                                                                "per acceptance criterion: accept: how it is known to be done")}},
                               {"to", "instructions"})));
    list.push_back(tool("plan_report_blocker",
                        "Report one thing that blocks your task. Give exactly one of on or to. The first line "
                        "of text becomes the task that unblocks you.",
                        schema({{"on", prop("string", "A task in the plan you wait for (^id)")},
                                {"to", prop("string", "Who can unblock you: a teammate's handle, customer (the "
                                                      "product owner asks them), or operator (access, credentials, "
                                                      "tools, services)")},
                                {"text", prop("string", "What is missing, and what would unblock you")}},
                               {"text"})));
    list.push_back(tool("plan_check_criterion", "Record that you checked an acceptance criterion of your task.",
                        schema({{"number", prop("integer", "The criterion's number in plan_get_task")},
                                {"status", prop("string", "The result; skipped: it could not be checked yet (it stays open, never a pass)",
                                               {"verified", "failed", "skipped"})},
                                {"evidence", prop("string", "How you checked it (a command, a test, a file); "
                                                            "for skipped, why")}},
                               {"number", "status"})));
    list.push_back(tool("plan_report_finding",
                        "Tell the operator what is missing outside the team (a tool's gap, a missing feature) when "
                        "it does not stop your task: it becomes a task for the operator, and your task is not blocked. "
                        "Start it with \"To @team:\" for one of the org's other teams (a tool's support team): it "
                        "becomes a task for them instead.",
                        schema({{"text", prop("string", "What is missing, where, and what it costs you")}}, {"text"})));
    list.push_back(tool("skill_read",
                        "Read one of the team's skills in full (the Skills section lists them): how the team learned "
                        "to do a job.",
                        schema({{"name", prop("string", "The skill's name as listed (its slug)")}}, {"name"})));
    list.push_back(tool("plan_report_lesson",
                        "Record what you learned that would have saved you time (a tool's quirk, a check that works, "
                        "a mistake not to repeat). It goes into your memory for your next tasks; the coach may make it "
                        "a skill for the whole crew.",
                        schema({{"text", prop("string", "The lesson, in one or two sentences")}}, {"text"})));
    list.push_back(tool("plan_request_web_access",
                        "Ask the operator to open a web site to the team (your web access is the team's list of "
                        "sites): a vendor's manuals, a tool's docs. Your task goes on; the site is open from "
                        "your next run once approved.",
                        schema({{"site", prop("string", "The site's host name (manuals.example.com)")},
                                {"why", prop("string", "The document you need there, and why")}},
                               {"site", "why"})));
    list.push_back(tool("plan_report_org_issue",
                        "Tell the developers of myteam (the system running this team) what in the way the team "
                        "works got in your way. Not about the product.",
                        schema({{"area", prop("string", "Where", {"context", "plan", "handoffs", "tools",
                                                                  "permissions", "process", "team", "other"})},
                                {"text", prop("string", "What got in the way, and what would have helped")}},
                               {"area", "text"})));
    if (s_.services) {
        list.push_back(tool("service_status", "The project's services (database, proxy, ...): what runs, and how it is.",
                            schema({}, {})));
        list.push_back(tool("service_logs", "The last lines a service wrote.",
                            schema({{"name", prop("string", "The service")},
                                    {"lines", prop("integer", "How many (default 80, at most 500)")}},
                                   {"name"})));
        list.push_back(tool("service_restart",
                            "Restart a service, e.g. after you changed the project file it reads (its config).",
                            schema({{"name", prop("string", "The service")}}, {"name"})));
        list.push_back(tool("service_reset", "Remove a service's data and start it fresh (an empty database).",
                            schema({{"name", prop("string", "The service")}}, {"name"})));
    }
    if (s_.commits)
        list.push_back(tool("git_commit",
                            "Commit your work on the current branch (git commit and git add do not work here). "
                            "Commits what your run changed and has not committed yet: all of it, or the paths "
                            "you name. myteam checks it first (no protected files, deleted tests, skipped tests "
                            "or secrets) and answers with the commit or what to fix.",
                            schema({{"message", prop("string", "The commit message: a summary line, then the why")},
                                    {"paths", Json::parse(R"({"type":"array","items":{"type":"string"},"description":)"
                                                          R"("Files or directories to commit; default: all your changes"})")}},
                                   {"message"})));
    Json r = Json::object();
    r.set("tools", list);
    return r;
}

Json McpServer::call(std::string_view name, const Json& args) {
    Plan plan = Plan::load(s_.project.plan_path());
    const TaskRef* mine = s_.task.empty() ? nullptr : plan.by_id(s_.task);

    if (name == "plan_get_task") {
        std::string key = str_arg(args, "task");
        if (key.empty()) key = str_arg(args, "id");  // what agents often call it
        const TaskRef* r = key.empty() ? mine : plan.find(key);
        if (!r && !key.empty()) r = plan.by_id(strip_caret(key));
        if (!r) return error_result(key.empty() ? "you have no task in this session" : "no task " + key + " in the plan");
        return text_result(describe_task(plan, *r));
    }
    if (name == "plan_team") {
        std::string out;
        for (const Member& m : s_.team.members) {
            out += "- @" + m.handle + " - " + m.name + ", " + m.role;
            if (!m.summary.empty()) out += ": " + m.summary;
            out += "\n";
        }
        out += "- @operator - the person running the team (access, credentials, tools)\n";
        out += "- the customer - only through the product owner, @" + s_.team.product_owner_member().handle + "\n";
        return text_result(out);
    }

    if (s_.services && starts_with(name, "service_")) {
        std::string action(name.substr(8));
        if (action != "status" && action != "logs" && action != "restart" && action != "reset")
            return error_result("no tool " + std::string(name));
        const Json* ln = args.find("lines");
        ServiceAnswer a = request_service(s_.report_path.parent_path(), action, str_arg(args, "name"),
                                          ln && ln->is_number() ? static_cast<int>(ln->as_number()) : 80);
        return a.ok ? text_result(a.text) : error_result(a.text);
    }
    if (name == "git_commit" && s_.commits) {
        CommitRequest req;
        req.message = str_arg(args, "message");
        if (const Json* ps = args.find("paths"); ps && ps->is_array())
            for (const Json& p : ps->as_array())
                if (p.is_string()) req.paths.push_back(p.as_string());
        if (std::string(trim(req.message)).empty()) return error_result("the message is empty");
        RunCommit c = request_commit(s_.report_path.parent_path(), req);
        if (!c.ok) return error_result("Not committed: " + c.error);
        std::string out = "Committed " + c.commit.substr(0, 10) + " on " + c.branch + ": " + c.summary + "\n";
        for (const auto& f : c.files) out += "  " + f + "\n";
        for (const auto& f : c.left_out) out += "  left out: " + f + "\n";
        if (!c.hooks.empty()) out += "Hooks: " + join(c.hooks, ", ") + "\n";
        for (const auto& f : c.hook_changed) out += "  fixed by a hook (also in your work tree): " + f + "\n";
        return text_result(out);
    }

    // Reporting: checked here, recorded for the end of the run.
    if (name == "plan_report_status") {
        std::string st = str_arg(args, "status");
        auto status = report_status_from_string(st);
        if (!status || *status == TaskStatus::Cancelled)
            return error_result("status must be one of done, review, in_progress, blocked");
        std::string reviewer = normalize_handle(str_arg(args, "reviewer"));
        if (*status == TaskStatus::Review)
            if (std::string why = reviewer_problem(s_.team, s_.member, reviewer); !why.empty())
                return error_result("Not recorded: " + why + ".");
        report_.reviewer = *status == TaskStatus::Review ? reviewer : "";
        report_.status = status;
        report_.status_text = st;
        std::string summary = str_arg(args, "summary");
        if (!summary.empty()) report_.summary = summary;
        save();
        std::string note = "Recorded: " + st + (report_.reviewer.empty() ? "" : ", reviewer @" + report_.reviewer) +
                           ". myteam updates the plan when your run ends.";
        if (*status == TaskStatus::Blocked && report_.blockers.empty())
            note += " Report what blocks you with plan_report_blocker.";
        return text_result(note);
    }
    if (name == "plan_handoff") {
        std::string to = handoff_target(s_.team, str_arg(args, "to"));
        std::string instructions = str_arg(args, "instructions");
        if (to.empty()) {
            std::string others;
            for (const auto& [h, what] : s_.team.external) others += ", @" + h;
            return error_result("no teammate @" + normalize_handle(str_arg(args, "to")) + ". The team: " +
                                handles(s_.team) + (others.empty() ? "" : "; other teams of the org: " + others.substr(2)) +
                                "; or @" + std::string(kOperator));
        }
        if (to == s_.member) return error_result("you cannot hand off to yourself");
        if (instructions.empty()) return error_result("instructions are empty");
        {
            bool criteria = false;
            for (const auto& l : split_lines(instructions)) criteria = criteria || criterion_line(l).has_value();
            if (!criteria)
                return error_result("give at least one acceptance criterion, a line \"accept: how it is known to be "
                                    "done\", so @" + to + " can tell when the task is finished");
        }
        // The same job, already theirs and open: say so (myteam would only add a note).
        for (const TaskRef& r : plan.tasks())
            if (r.task->assignee && normalize_handle(*r.task->assignee) == to &&
                (is_open(r.task->status) || r.task->status == TaskStatus::Blocked) &&
                same_subject(r.task->description, first_line(instructions, 300))) {
                report_.handoffs.push_back({to, instructions});
                save();
                return text_result("@" + to + " already has this job: " + one_liner(r) +
                                   ". Your handoff is added to it as a note, not as a second task.");
            }
        report_.handoffs.push_back({to, instructions});
        save();
        const bool waits = handoff_waits_for_sender(s_.team, to);
        return text_result("Recorded: a task for @" + to + ": " + first_line(instructions, 120) +
                           (waits ? "" : (to == kOperator ? ". It does not wait for your task, and yours does not wait for it."
                                                          : ". The org carries it to that team; it does not wait for your "
                                                            "task, and yours does not wait for it.")));
    }
    if (name == "plan_report_blocker") {
        std::string on = strip_caret(str_arg(args, "on"));
        std::string to = normalize_handle(str_arg(args, "to"));
        std::string text = str_arg(args, "text");
        if (text.empty()) return error_result("say what is missing in text");
        if (!on.empty() == !to.empty()) return error_result("give exactly one of on (a task) or to (who)");
        BlockerReport b;
        b.text = text;
        if (!on.empty()) {
            const TaskRef* t = plan.by_id(on);
            if (!t) return error_result("no task ^" + on + " in the plan; use to= for work nobody has planned yet");
            if (mine && t->task == mine->task) return error_result("a task cannot block itself");
            b.kind = BlockerReport::Kind::Task;
            b.on = on;
        } else if (to == "customer") {
            b.kind = BlockerReport::Kind::Customer;
        } else if (to == kOperator) {
            b.kind = BlockerReport::Kind::Operator;
            if (const TaskRef* open = open_operator_task(plan, text)) return error_result(already_open(*open, true));
            for (const BlockerReport& x : report_.blockers)
                if (x.kind == BlockerReport::Kind::Operator && same_subject(x.text, text)) {
                    if (first_line(x.text) == first_line(text)) return text_result("Already recorded.");
                    return error_result("Not recorded: you reported this to the operator already in this run (\"" +
                                        first_line(x.text, 120) + "\"). If yours is a different matter, say in "
                                        "the first line how it differs.");
                }
        } else if (const Member* m = s_.team.find(to)) {
            b.kind = BlockerReport::Kind::Member;
            b.to = m->handle;
        } else if (s_.team.is_external(to)) {
            b.kind = BlockerReport::Kind::Member;  // another team of the org
            b.to = to;
        } else {
            std::string others;
            for (const auto& [h, what] : s_.team.external) others += ", " + h;
            return error_result("no teammate @" + to + "; use a handle (" + handles(s_.team) + "), " +
                                (others.empty() ? "" : "another team (" + others.substr(2) + "), ") +
                                "customer or operator");
        }
        report_.blockers.push_back(std::move(b));
        save();
        return text_result("Recorded the blocker. Report status blocked with plan_report_status if you cannot go on.");
    }
    if (name == "plan_check_criterion") {
        if (!mine) return error_result("you have no task in this session");
        const Json* nv = args.find("number");
        int n = 0;
        if (nv && nv->is_number()) n = static_cast<int>(nv->as_number());
        else if (nv && nv->is_string()) n = std::atoi(nv->as_string().c_str());
        const int count = static_cast<int>(mine->task->criteria.size());
        if (n < 1 || n > count)
            return error_result(count == 0 ? "your task has no acceptance criteria"
                                           : "number must be 1 to " + std::to_string(count));
        const std::string status = str_arg(args, "status");
        const bool skipped = is_skipped_status(status);
        auto st = criterion_status_from_report(status);
        if (!st || (*st == taskml::CriterionStatus::Pending && !skipped))
            return error_result("status must be verified, failed or skipped");
        if (skipped && trim(str_arg(args, "evidence")).empty())
            return error_result("say in evidence why it is skipped (what is missing, who delivers it)");
        CriterionReport c{n, *st, skipped ? skipped_evidence(str_arg(args, "evidence")) : str_arg(args, "evidence")};
        // A later check of the same criterion replaces the earlier one.
        auto& cs = report_.criteria;
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const CriterionReport& x) { return x.number == n; }), cs.end());
        cs.push_back(std::move(c));
        save();
        return text_result("Recorded: criterion " + std::to_string(n) + " (" +
                           mine->task->criteria[static_cast<std::size_t>(n - 1)].description + ") " +
                           (skipped ? "skipped: it stays open" : criterion_word(*st)) + ".");
    }
    if (name == "skill_read") {
        const std::string want(trim(str_arg(args, "name")));
        for (const Skill& s : load_skills(s_.project))
            if (s.slug == want || s.name == want) return text_result("# " + s.name + "\n\n" + s.description + "\n\n" + s.body);
        std::string names;
        for (const Skill& s : load_skills(s_.project)) names += (names.empty() ? "" : ", ") + s.slug;
        return error_result("no skill \"" + want + "\"" + (names.empty() ? " (the team has none yet)" : "; there are: " + names));
    }
    if (name == "plan_report_lesson") {
        const std::string text(trim(str_arg(args, "text")));
        if (text.empty()) return error_result("text is empty");
        if (std::find(report_.lessons.begin(), report_.lessons.end(), text) == report_.lessons.end()) {
            report_.lessons.push_back(text);
            save();
        }
        return text_result("Recorded: it goes into your memory when your run ends.");
    }
    if (name == "plan_request_web_access") {
        const std::string site = web_host(str_arg(args, "site"));
        if (site.empty()) return error_result("site is not a host name (manuals.example.com)");
        for (const auto& [s, why] : report_.web_access)
            if (s == site) return text_result("Already asked in this run.");
        report_.web_access.emplace_back(site, std::string(trim(str_arg(args, "why"))));
        save();
        return text_result("Asked: the operator approves " + site + " for the team. Your task goes on.");
    }
    if (name == "plan_report_finding") {
        if (!mine) return error_result("you have no task in this session");
        std::string text(trim(str_arg(args, "text")));
        if (text.empty()) return error_result("text is empty");
        if (const TaskRef* open = open_operator_task(plan, text)) return error_result(already_open(*open, false));
        auto& fs_ = report_.findings;
        for (const std::string& x : fs_)
            if (same_subject(x, text)) {
                if (first_line(x) == first_line(text)) return text_result("Already recorded. Your task goes on.");
                return error_result("Not recorded: you reported this finding already in this run (\"" +
                                    first_line(x, 120) + "\"). If yours is a different matter, say in the first "
                                    "line how it differs.");
            }
        fs_.push_back(text);
        save();
        return text_result("Recorded: a task for the operator. Your task goes on; it is not blocked.");
    }
    if (name == "plan_report_org_issue") {
        std::string text = str_arg(args, "text");
        if (text.empty()) return error_result("text is empty");
        report_.org_issues.push_back({org_issue_area(str_arg(args, "area")), text});
        save();
        return text_result("Recorded for the developers of myteam. Thank you.");
    }
    return error_result("unknown tool " + std::string(name));
}

std::optional<Json> McpServer::handle(const Json& msg) {
    const Json* id = msg.find("id");
    const std::string method = msg.find("method") && msg.find("method")->is_string() ? msg.find("method")->as_string() : "";
    if (!id || id->is_null()) return std::nullopt;  // a notification (initialized, cancelled, ...)
    Json resp = Json::object();
    resp.set("jsonrpc", "2.0");
    resp.set("id", *id);
    const Json empty = Json::object();
    const Json* params = msg.find("params");
    if (!params) params = &empty;

    if (method == "initialize") {
        Json r = Json::object();
        std::string version = "2025-06-18";
        if (const Json* v = params->find("protocolVersion"); v && v->is_string()) version = v->as_string();
        r.set("protocolVersion", version);
        Json caps = Json::object();
        caps.set("tools", Json::object());
        r.set("capabilities", caps);
        Json info = Json::object();
        info.set("name", "myteam");
        info.set("version", MYTEAM_VERSION);
        r.set("serverInfo", info);
        r.set("instructions",
              "Tools for your task in the team's plan. Read with plan_get_task; report with plan_report_status, "
              "plan_handoff, plan_report_blocker and plan_check_criterion. They check what you give them; "
              "the plan is updated when your run ends.");
        resp.set("result", r);
    } else if (method == "ping") {
        resp.set("result", Json::object());
    } else if (method == "tools/list") {
        resp.set("result", tools());
    } else if (method == "tools/call") {
        std::string name = str_arg(*params, "name");
        const Json* args = params->find("arguments");
        try {
            resp.set("result", call(name, args && args->is_object() ? *args : empty));
        } catch (const std::exception& e) {
            resp.set("result", error_result(std::string("failed: ") + e.what()));
        }
    } else if (method == "resources/list" || method == "prompts/list") {
        Json r = Json::object();
        r.set(method == "resources/list" ? "resources" : "prompts", Json::array());
        resp.set("result", r);
    } else {
        Json e = Json::object();
        e.set("code", -32601);
        e.set("message", "method not found: " + method);
        resp.set("error", e);
    }
    return resp;
}

void McpServer::serve(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        std::optional<Json> resp;
        try {
            resp = handle(Json::parse(line));
        } catch (const std::exception& e) {
            Json r = Json::object();
            r.set("jsonrpc", "2.0");
            r.set("id", nullptr);
            Json err = Json::object();
            err.set("code", -32700);
            err.set("message", std::string("parse error: ") + e.what());
            r.set("error", err);
            resp = r;
        }
        if (resp) out << resp->dump() << "\n" << std::flush;
    }
}

// ---------------------------------------------------------------------------
// The report file
// ---------------------------------------------------------------------------

Json report_to_json(const AgentReport& r) {
    Json j = Json::object();
    j.set("status", r.status_text);
    if (!r.reviewer.empty()) j.set("reviewer", r.reviewer);
    j.set("summary", r.summary);
    Json hs = Json::array();
    for (const auto& h : r.handoffs) {
        Json x = Json::object();
        x.set("to", h.to);
        x.set("instructions", h.instructions);
        hs.push_back(x);
    }
    j.set("handoffs", hs);
    Json bs = Json::array();
    for (const auto& b : r.blockers) {
        Json x = Json::object();
        const char* kind = b.kind == BlockerReport::Kind::Task       ? "task"
                           : b.kind == BlockerReport::Kind::Member   ? "member"
                           : b.kind == BlockerReport::Kind::Customer ? "customer"
                                                                     : "operator";
        x.set("kind", kind);
        x.set("on", b.on);
        x.set("to", b.to);
        x.set("text", b.text);
        bs.push_back(x);
    }
    j.set("blockers", bs);
    Json cs = Json::array();
    for (const auto& c : r.criteria) {
        Json x = Json::object();
        x.set("number", c.number);
        x.set("status", criterion_word(c.status));
        x.set("evidence", c.evidence);
        cs.push_back(x);
    }
    j.set("criteria", cs);
    Json os = Json::array();
    for (const auto& o : r.org_issues) {
        Json x = Json::object();
        x.set("area", o.area);
        x.set("text", o.text);
        os.push_back(x);
    }
    j.set("org_issues", os);
    Json fs_ = Json::array();
    for (const auto& f : r.findings) fs_.push_back(Json(f));
    j.set("findings", fs_);
    Json ls = Json::array();
    for (const auto& l : r.lessons) ls.push_back(Json(l));
    j.set("lessons", ls);
    Json wa = Json::array();
    for (const auto& [site, why] : r.web_access) {
        Json x = Json::object();
        x.set("site", site);
        x.set("why", why);
        wa.push_back(x);
    }
    j.set("web_access", wa);
    if (!r.plan_edits.empty()) j.set("plan_edits", r.plan_edits);
    return j;
}

AgentReport report_from_json(const Json& j) {
    AgentReport r;
    auto s = [](const Json& o, const char* k) {
        const Json* v = o.find(k);
        return v && v->is_string() ? v->as_string() : std::string();
    };
    auto arr = [](const Json& o, const char* k) -> const Json::Array* {
        const Json* v = o.find(k);
        return v && v->is_array() ? &v->as_array() : nullptr;
    };
    r.status_text = s(j, "status");
    if (!r.status_text.empty()) r.status = report_status_from_string(r.status_text);
    r.reviewer = s(j, "reviewer");
    r.summary = s(j, "summary");
    if (auto* a = arr(j, "handoffs"))
        for (const Json& x : *a) r.handoffs.push_back({s(x, "to"), s(x, "instructions")});
    if (auto* a = arr(j, "blockers"))
        for (const Json& x : *a) {
            BlockerReport b;
            std::string kind = s(x, "kind");
            b.kind = kind == "task"       ? BlockerReport::Kind::Task
                     : kind == "member"   ? BlockerReport::Kind::Member
                     : kind == "customer" ? BlockerReport::Kind::Customer
                                          : BlockerReport::Kind::Operator;
            b.on = s(x, "on");
            b.to = s(x, "to");
            b.text = s(x, "text");
            r.blockers.push_back(std::move(b));
        }
    if (auto* a = arr(j, "criteria"))
        for (const Json& x : *a) {
            CriterionReport c;
            const Json* n = x.find("number");
            c.number = n && n->is_number() ? static_cast<int>(n->as_number()) : 0;
            c.status = criterion_status_from_report(s(x, "status")).value_or(taskml::CriterionStatus::Pending);
            c.evidence = s(x, "evidence");
            r.criteria.push_back(std::move(c));
        }
    if (auto* a = arr(j, "org_issues"))
        for (const Json& x : *a) r.org_issues.push_back({s(x, "area"), s(x, "text")});
    if (auto* a = arr(j, "findings"))
        for (const Json& x : *a)
            if (x.is_string() && !x.as_string().empty()) r.findings.push_back(x.as_string());
    if (auto* a = arr(j, "lessons"))
        for (const Json& x : *a)
            if (x.is_string() && !x.as_string().empty()) r.lessons.push_back(x.as_string());
    r.plan_edits = s(j, "plan_edits");
    if (auto* a = arr(j, "web_access"))
        for (const Json& x : *a)
            if (std::string site = web_host(s(x, "site")); !site.empty()) r.web_access.emplace_back(site, s(x, "why"));
    return r;
}

AgentReport load_mcp_report(const fs::path& run_dir) {
    auto text = try_read_file(run_dir / kMcpReportFile);
    if (!text) return {};
    try {
        return report_from_json(Json::parse(*text));
    } catch (const std::exception&) {
        return {};
    }
}

void merge_reports(AgentReport& r, const AgentReport& t) {
    if (t.status) {
        r.status = t.status;
        r.status_text = t.status_text;
        if (!t.reviewer.empty() || *t.status != taskml::TaskStatus::Review) r.reviewer = t.reviewer;
    }
    if (!t.summary.empty()) r.summary = t.summary;
    for (const auto& h : t.handoffs) {
        bool dup = std::any_of(r.handoffs.begin(), r.handoffs.end(), [&](const HandoffRequest& x) {
            return x.to == h.to && first_line(x.instructions) == first_line(h.instructions);
        });
        if (!dup) r.handoffs.push_back(h);
    }
    for (const auto& b : t.blockers) {
        bool dup = std::any_of(r.blockers.begin(), r.blockers.end(), [&](const BlockerReport& x) {
            if (x.kind != b.kind || x.on != b.on || x.to != b.to) return false;
            // The same escalation in the tool and in the final message, worded twice.
            return first_line(x.text) == first_line(b.text) ||
                   (b.kind == BlockerReport::Kind::Operator && same_subject(x.text, b.text));
        });
        if (!dup) r.blockers.push_back(b);
    }
    for (const auto& l : t.lessons)
        if (std::find(r.lessons.begin(), r.lessons.end(), l) == r.lessons.end()) r.lessons.push_back(l);
    for (const auto& c : t.criteria) {
        auto& cs = r.criteria;
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const CriterionReport& x) { return x.number == c.number; }),
                 cs.end());
        cs.push_back(c);
    }
    for (const auto& o : t.org_issues) {
        bool dup = std::any_of(r.org_issues.begin(), r.org_issues.end(),
                               [&](const OrgIssue& x) { return first_line(x.text) == first_line(o.text); });
        if (!dup) r.org_issues.push_back(o);
    }
    for (const auto& f : t.findings)
        if (std::none_of(r.findings.begin(), r.findings.end(),
                         [&](const std::string& x) { return first_line(x) == first_line(f) || same_subject(x, f); }))
            r.findings.push_back(f);
    if (!t.plan_edits.empty() && r.plan_edits.find(t.plan_edits) == std::string::npos)
        r.plan_edits += (r.plan_edits.empty() ? "" : "\n") + t.plan_edits;
    for (const auto& w : t.web_access)
        if (std::none_of(r.web_access.begin(), r.web_access.end(), [&](const auto& x) { return x.first == w.first; }))
            r.web_access.push_back(w);
}

}  // namespace myteam
