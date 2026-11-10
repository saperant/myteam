#include "myteam/context.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "myteam/util.hpp"

namespace myteam {

using taskml::Task;

std::string AgentContext::render() const {
    std::string out;
    for (const ContextSection& s : sections) {
        if (s.body.empty()) continue;
        if (!out.empty()) out += "\n";
        out += s.title.empty() ? s.body : "# " + s.title + "\n\n" + s.body;
        if (out.back() != '\n') out += '\n';
    }
    return out;
}

std::size_t AgentContext::estimated_tokens() const { return estimate_tokens(render()); }

const ContextSection* AgentContext::section(std::string_view id) const {
    for (const ContextSection& s : sections)
        if (s.id == id) return &s;
    return nullptr;
}

namespace {

std::string role_section(const ContextInputs& in) {
    const Member& m = in.member;
    std::string out = "You are " + m.name + " (@" + m.handle + "), the " + m.role + " of team " + in.team.name + ".";
    if (!m.skills.empty()) {
        out += " Your skills: ";
        for (std::size_t i = 0; i < m.skills.size(); ++i) out += (i ? ", " : "") + m.skills[i];
        out += ".";
    }
    out += "\n";
    if (!m.prompt.empty()) out += "\n" + m.prompt + "\n";
    return out;
}

std::string meta_line(const Task& t) {
    std::vector<std::string> parts;
    parts.push_back("status: " + status_label(t.status));
    if (t.priority) parts.push_back("priority: p" + std::to_string(*t.priority));
    if (t.estimate) parts.push_back("estimate: " + *t.estimate);
    if (t.due) parts.push_back("due: " + *t.due);
    if (t.assignee) parts.push_back("assignee: @" + *t.assignee);
    if (!t.tags.empty()) {
        std::string tags;
        for (const auto& tag : t.tags) tags += (tags.empty() ? "#" : " #") + tag;
        parts.push_back("tags: " + tags);
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? " · " : "") + parts[i];
    return out;
}

void list_ids(std::string& out, const Plan& plan, const std::vector<std::string>& ids, const char* title) {
    if (ids.empty()) return;
    out += std::string("\n") + title + ":\n";
    for (const std::string& id : ids) {
        const TaskRef* r = plan.by_id(id);
        if (!r)
            out += "- ^" + id + " (not in the plan)\n";
        else
            out += "- " + task_one_liner(*r->task) + "\n";
    }
}

/// The org's other teams this project works with (myorg), and how to ask them.
std::string other_teams(const Team& team) {
    if (team.external.empty()) return "";
    std::string out = "\nOther teams of the org (not your teammates):\n";
    for (const auto& [h, what] : team.external) out += "- @" + h + (what.empty() ? "" : " — " + what) + "\n";
    const std::string first = team.external.begin()->first;
    out += "When your task needs something from one of them (a decision, a part of their design, a fix), say\n"
           "so with a blocker, <blocker to=\"" + first + "\">what you need, and why</blocker> (or plan_report_blocker\n"
           "with to=\"" + first + "\"). The org carries it to that team's sprint, and your task goes on when they\n"
           "deliver. Work for them that your task does not wait for (planning it, say) is a handoff,\n"
           "<handoff to=\"" + first + "\"> (or plan_handoff): the org carries it, and your task is not blocked.\n"
           "Read what they publish (interfaces, their repository where you can see it) before asking.\n";
    return out;
}

std::string teammates_rules(const ContextInputs& in);

std::string teammates_section(const ContextInputs& in) {
    if (!in.team.coach.empty()) return crew_section(in.team, in.member.handle);
    return teammates_rules(in) + other_teams(in.team);
}

std::string teammates_rules(const ContextInputs& in) {
    const Member& m = in.member;
    if (m.handoffs.empty()) {
        std::string out = "No handoff rules for your role. ";
        if (in.team.product_owner != m.handle) {
            const Member& f = in.team.product_owner_member();
            out += "When the task needs someone else, hand off to " + f.name + " (@" + f.handle + ", " + f.role +
                   "), who coordinates the team.\n";
        } else {
            out += "Hand off to whoever the work needs.\n";
        }
        return out;
    }
    std::string out = "Hand work off when:\n";
    std::set<std::string> listed;
    for (const HandoffRule& r : m.handoffs) {
        const Member* to = in.team.find(r.to);
        if (!to) continue;
        out += "- " + r.when + " → " + to->name + " (@" + to->handle + ")";
        if (!r.note.empty()) out += ": " + r.note;
        out += "\n";
        listed.insert(to->handle);
    }
    out += "\nWho they are:\n";
    for (const std::string& h : listed) {
        const Member* to = in.team.find(h);
        out += "- @" + to->handle + " — " + to->name + ", " + to->role;
        if (!to->summary.empty()) out += ": " + to->summary;
        out += "\n";
    }
    // The rules say who usually gets your work; anyone in the team can get a handoff.
    out += "\nThese are the usual ones: any teammate can get a handoff when the task needs them (plan_team, or\n"
           "the team's handles in the plan, says who is who).\n";
    return out;
}

const char* kReporting =
    "Do not edit the plan file (PLAN.md) yourself: myteam records your status,\n"
    "summary, handoffs and blockers in it. You may split your work: hand off the\n"
    "parts that belong to other roles instead of doing them.\n"
    "\n"
    "The brief is short: everyone reads it every time. Write long material\n"
    "(requirements, a design, security rules) to a file in docs/, link it from the\n"
    "brief, and refer to it from the tasks that need it.\n"
    "\n"
    "End your final message with these blocks:\n"
    "\n"
    "<task_status>done</task_status>\n"
    "    one of: done (the task is complete), review (done; add reviewer=\"handle\",\n"
    "    not you), in_progress (more work remains), blocked (you cannot continue)\n"
    "<work_summary>\n"
    "What you did, key decisions, files changed.\n"
    "</work_summary>\n"
    "\n"
    "For each handoff:\n"
    "\n"
    "<handoff to=\"handle\">\n"
    "What they should do, and the file or artifact they should deliver.\n"
    "accept: how they and the reviewer know it is done (one line each)\n"
    "</handoff>\n"
    "\n"
    "For each acceptance criterion, by its number in the task:\n"
    "\n"
    "<criterion n=\"1\" status=\"verified\">How you checked it.</criterion>\n"
    "    status: verified, failed or skipped (not checkable yet: say why)\n"
    "\n"
    "When you are blocked, add one block per blocker. Say exactly what is missing\n"
    "and what would unblock you; the first line becomes the task that unblocks you:\n"
    "\n"
    "<blocker on=\"^task-id\">What you need from that task.</blocker>\n"
    "    you wait for a task already in the plan\n"
    "<blocker to=\"handle\">The work or decision you need from a teammate.</blocker>\n"
    "<blocker to=\"customer\">The question only the customer can answer.</blocker>\n"
    "    the product owner takes it to the customer\n"
    "<blocker to=\"operator\">What is missing outside the team: access, credentials,\n"
    "a tool, a service.</blocker>\n"
    "    the person running the team provides it\n"
    "<finding>A gap outside the team (in a tool) you can work without.</finding>\n"
    "<web_access site=\"host\">A site not on the team's list, and what you need there.</web_access>\n"
    "    both go to the operator; they do not block your task\n"
    "\n"
    "When the way the team works got in your way, say so (optional): it goes to the\n"
    "developers of myteam, the system running this team, not into the plan. One\n"
    "block per issue:\n"
    "\n"
    "<org_issue area=\"context\">What got in the way, and what would have helped.</org_issue>\n"
    "    area: context, plan, handoffs, tools, permissions, process, team, other\n";

const char* kCrewReporting =
    "Do not edit or read the plan (PLAN.md): your task is above, with everything the coach\n"
    "gave you for it. myteam records your report in the plan.\n"
    "\n"
    "When you report done, the Gatekeeper checks your work against the criteria before it\n"
    "counts: say how each criterion was checked, and where the evidence is (a command, a\n"
    "file, a test). A claim it cannot check goes back to you.\n"
    "\n"
    "End your final message with these blocks:\n"
    "\n"
    "<task_status>done</task_status>\n"
    "    one of: done (complete: the Gatekeeper checks it), in_progress (more work\n"
    "    remains), blocked (you cannot continue)\n"
    "<work_summary>\n"
    "What you did, key decisions, files changed, how to check it.\n"
    "</work_summary>\n"
    "\n"
    "For each acceptance criterion, by its number in the task:\n"
    "\n"
    "<criterion n=\"1\" status=\"verified\">How you checked it.</criterion>\n"
    "    status: verified, failed or skipped (not checkable yet: say why)\n"
    "\n"
    "Work for another expert (it does not stop yours) is a handoff; the coach sees it:\n"
    "\n"
    "<handoff to=\"handle\">\n"
    "What they should do, and the file or artifact they should deliver.\n"
    "accept: how they and the Gatekeeper know it is done (one line each)\n"
    "</handoff>\n"
    "\n"
    "When you are blocked, one block per blocker, saying exactly what is missing and what\n"
    "would unblock you:\n"
    "\n"
    "<blocker on=\"^task-id\">What you need from that task.</blocker>\n"
    "<blocker to=\"handle\">The work or decision you need from another expert.</blocker>\n"
    "<blocker to=\"coach\">A decision only the coach can make, or a question for the customer.</blocker>\n"
    "<blocker to=\"operator\">What is missing outside the crew: access, a tool.</blocker>\n"
    "<finding>A gap in a tool you can work without (name the tool first).</finding>\n"
    "\n"
    "What you learned that would have saved you time (a tool's quirk, a check that works, a\n"
    "mistake not to repeat): one block each, one or two sentences. It goes into your memory\n"
    "for your next tasks, and the coach may make it a skill for everyone:\n"
    "\n"
    "<lesson>What you learned.</lesson>\n"
    "\n"
    "When the way the crew works got in your way (optional, for myteam's developers):\n"
    "\n"
    "<org_issue area=\"context\">What got in the way, and what would have helped.</org_issue>\n"
    "    area: context, plan, handoffs, tools, permissions, process, team, other\n";

}  // namespace

std::string crew_section(const Team& team, const std::string& self) {
    if (team.coach.empty()) return "";
    std::string out;
    if (const Member* c = team.find(team.coach))
        out += "The coach, " + c->name + " (@" + c->handle + "), decides what the crew does and assigns it.\n";
    if (const Member* g = team.find(team.gatekeeper))
        out += "The Gatekeeper, " + g->name + " (@" + g->handle + "), checks every piece of work before it counts.\n";
    out += "\nThe experts:\n";
    for (const Member& m : team.members) {
        if (m.handle == team.coach || m.handle == team.gatekeeper) continue;
        out += "- @" + m.handle + " — " + m.name + ", " + m.role;
        if (!m.unit.empty()) out += " (" + m.unit + "/)";
        if (!m.summary.empty()) out += ": " + m.summary;
        if (m.handle == self) out += " (you)";
        out += "\n";
    }
    out += "\nYou see your own unit's repository and the contracts it reads, not the others': ask an\n"
           "expert through a handoff or a blocker, and read what they publish in interfaces/.\n";
    return out;
}

std::string describe_task(const Plan& plan, const TaskRef& ref) {
    const Task& t = *ref.task;
    std::string out = "**" + t.description + "**";
    out += t.id ? " (^" + *t.id + ")\n" : "\n";
    out += meta_line(t) + "\n";

    if (!ref.sections.empty() || !ref.parents.empty()) {
        out += "\nWhere it is in the plan:\n";
        for (const std::string& s : ref.sections) out += "- section: " + s + "\n";
        for (const Task* p : ref.parents) out += "- part of: " + task_one_liner(*p) + "\n";
    }
    if (!t.notes.empty()) {
        out += "\nNotes:\n";
        for (const std::string& n : t.notes) out += "- " + n + "\n";
    }
    if (!t.criteria.empty()) {
        out += "\nAcceptance criteria:\n";
        int n = 0;
        for (const auto& c : t.criteria) {
            out += std::to_string(++n) + ". " + taskml::criterion_marker_ascii(c.status) + " " + c.description;
            if (c.evidence) out += " (evidence: " + *c.evidence + ")";
            out += "\n";
        }
    }
    if (!t.subtasks.empty()) {
        out += "\nSubtasks:\n";
        for (const Task& s : t.subtasks) {
            out += std::string("- [") + taskml::status_marker(s.status) + "] " + s.description;
            if (s.id || s.assignee) {
                out += " (";
                if (s.id) out += "^" + *s.id;
                if (s.id && s.assignee) out += ", ";
                if (s.assignee) out += "@" + *s.assignee;
                out += ")";
            }
            out += "\n";
        }
    }
    list_ids(out, plan, t.depends_on, "Depends on");
    list_ids(out, plan, t.blocked_by, "Blocked by");
    // What resolved the finished blockers: their notes (an answer, where the
    // credentials are, ...). Finished blockers' links are dropped (unblock_resolved): the
    // notes that say so ("Unblocked ...: ^x done", "Blocker ^x done ...") still name them.
    std::vector<std::string> resolved = t.blocked_by;
    for (const std::string& n : t.notes) {
        if (!starts_with(n, "Unblocked ") && !starts_with(n, "Blocker ^")) continue;
        for (std::size_t p = n.find('^'); p != std::string::npos; p = n.find('^', p + 1)) {
            std::size_t e = p + 1;
            while (e < n.size() && (std::isalnum(static_cast<unsigned char>(n[e])) || n[e] == '-' || n[e] == '_')) ++e;
            std::string id = n.substr(p + 1, e - p - 1);
            if (!id.empty() && std::find(resolved.begin(), resolved.end(), id) == resolved.end()) resolved.push_back(id);
        }
    }
    for (const std::string& id : resolved) {
        const TaskRef* b = plan.by_id(id);
        if (!b || !is_finished(b->task->status) || b->task->notes.empty()) continue;
        out += "\nResolution of ^" + id + ":\n";
        for (const std::string& n : b->task->notes)
            if (!starts_with(n, "Needed by")) out += "- " + n + "\n";
    }
    std::vector<const TaskRef*> waiting = plan.dependents(ref);
    if (!waiting.empty()) {
        out += "\nWaiting for this task:\n";
        for (const TaskRef* w : waiting) out += "- " + task_one_liner(*w->task) + "\n";
    }
    return out;
}

AgentContext build_task_context(const ContextInputs& in, const TaskRef& task) {
    AgentContext ctx;
    ctx.sections.push_back({"role", "Your role", role_section(in)});
    if (!in.memory.empty()) ctx.sections.push_back({"memory", "Your memory", in.memory});
    ctx.sections.push_back({"project", "The project", in.brief});
    std::string project_name = in.plan.directive("project");
    std::string task_body = describe_task(in.plan, task);
    if (!project_name.empty() || !in.plan.directive("sprint").empty() || !in.plan.directive("goal").empty()) {
        std::string head;
        if (!project_name.empty()) head += "Project: " + project_name + "\n";
        if (!in.plan.directive("sprint").empty()) head += "Sprint: " + in.plan.directive("sprint") + "\n";
        if (!in.plan.directive("goal").empty())
            head += std::string(in.plan.directive("sprint").empty() ? "Goal: " : "Sprint goal: ") + in.plan.directive("goal") + "\n";
        task_body = head + "\n" + task_body;
    }
    ctx.sections.push_back({"task", "Your task", task_body});
    ctx.sections.push_back({"teammates", "Teammates and handoffs", teammates_section(in)});
    if (!in.skills.empty()) ctx.sections.push_back({"skills", "Skills", in.skills});
    if (!in.services.empty()) ctx.sections.push_back({"services", "Services", in.services});
    if (!in.protected_paths.empty()) ctx.sections.push_back({"protected", "Protected paths", in.protected_paths});
    if (!in.notes.empty()) {
        std::string t;
        for (const auto& n : in.notes) t += "- " + n + "\n";
        ctx.sections.push_back({"operator", "From the operator", t});
    }
    std::string reporting = in.team.coach.empty() ? kReporting : kCrewReporting;
    if (in.tools)
        reporting +=
            "\nYou also have the myteam tools: plan_get_task and plan_list_tasks to read the plan,\n"
            "plan_report_status, plan_handoff, plan_report_blocker, plan_check_criterion,\n"
            "plan_report_finding and plan_report_org_issue to report. They check what you give\n"
            "them and tell you what is wrong. Report with the tools or with the blocks above;\n"
            "both are recorded when your run ends. If a tool's parameters are not in your tool\n"
            "list yet, load them first (in Claude Code: ToolSearch\n"
            "\"select:mcp__myteam__plan_report_blocker\", and so on).\n";
    if (in.tools && in.commits)
        reporting +=
            "\nCommit your work with the git_commit tool (a message; paths if you want to commit\n"
            "only some files). Commit when a piece of work is done and its tests pass. git commit\n"
            "and git add do not work here: myteam checks each commit and makes it on the current\n"
            "branch, with you as the author. Do not push; the operator does that.\n";
    const Member& po = in.team.product_owner_member();
    if (po.handle != in.member.handle)
        reporting += "\nThe customer talks only to " + po.name + " (@" + po.handle +
                     "). Do not contact the customer, and do not read or write .myteam/.\n";
    ctx.sections.push_back({"reporting", "Reporting", reporting});
    return ctx;
}

AgentContext build_repair_context(const ContextInputs& in, const std::vector<taskml::Diagnostic>& problems) {
    AgentContext ctx;
    ctx.sections.push_back({"role", "Your role", role_section(in)});
    std::string task = "The plan file " + in.plan.path().filename().string() +
                       " no longer validates. Fix only these problems, keeping every task, status,\n"
                       "assignee and note as it is. This is the one task where you edit the plan file.\n\n"
                       "Problems:\n";
    for (const auto& d : problems) task += "- " + d.format(in.plan.text()) + "\n";
    task += "\nThe plan is a Markdown file: front matter, # headings for sections, and\n"
            "list items `- [ ] description #p1 ~4h @member ^id` with nested `- depends: ^id`,\n"
            "`- blocked-by: ^id`, notes and subtasks.\n";
    ctx.sections.push_back({"task", "Your task", task});
    ctx.sections.push_back({"reporting", "Reporting",
                            "End your final message with:\n\n<task_status>done</task_status>\n"
                            "<work_summary>What you changed.</work_summary>\n"});
    return ctx;
}

}  // namespace myteam
