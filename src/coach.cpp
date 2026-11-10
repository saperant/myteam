#include "myteam/coach.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "myteam/blockers.hpp"
#include "myteam/changelog.hpp"
#include "myteam/checkpoint.hpp"
#include "myteam/memory.hpp"
#include "myteam/process.hpp"
#include "myteam/report.hpp"
#include "myteam/util.hpp"

namespace myteam {

using taskml::Json;
using taskml::TaskStatus;

// ---------------------------------------------------------------------------
// The coach's decisions
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> words_of(std::string_view s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : std::string(s) + " ") {
        if (c == ' ' || c == ',' || c == '\t' || c == '\n') {
            while (!w.empty() && w[0] == '^') w.erase(0, 1);
            while (!w.empty() && w[0] == '@') w.erase(0, 1);
            if (!w.empty()) out.push_back(w);
            w.clear();
        } else {
            w += c;
        }
    }
    return out;
}

std::optional<int> priority_of(std::string_view s) {
    std::string t = lower(trim(s));
    if (!t.empty() && (t[0] == 'p' || t[0] == '#')) t.erase(0, t[0] == '#' && t.size() > 1 && t[1] == 'p' ? 2 : 1);
    if (t.size() == 1 && t[0] >= '0' && t[0] <= '3') return t[0] - '0';
    return std::nullopt;
}

std::optional<TaskStatus> change_status(std::string_view s) {
    const std::string t = lower(trim(s));
    if (t.empty()) return std::nullopt;
    if (t == "cancelled" || t == "cancel" || t == "canceled" || t == "drop" || t == "dropped") return TaskStatus::Cancelled;
    if (t == "pending" || t == "open" || t == "reopen" || t == "todo") return TaskStatus::Pending;
    if (t == "blocked" || t == "hold" || t == "on-hold") return TaskStatus::Blocked;
    if (t == "done" || t == "completed" || t == "complete") return TaskStatus::Completed;
    if (t == "in_progress" || t == "in-progress") return TaskStatus::InProgress;
    if (t == "review") return TaskStatus::Review;
    return std::nullopt;
}

std::string last_body(std::string_view text, std::string_view tag) {
    auto bs = report_blocks(text, tag);
    return bs.empty() ? std::string() : bs.back().body;
}

constexpr const char* kTags[] = {"task",  "change", "answer", "ask",       "coaching",  "skill", "notes",
                                 "goal",  "milestone", "overrule", "to_customer"};

}  // namespace

bool CoachDecisions::needs_review() const {
    return !tasks.empty() || !changes.empty() || !answers.empty() || !coaching.empty() || !skills.empty() ||
           !goal.empty() || !milestone.empty();
}

bool CoachDecisions::empty() const {
    return !needs_review() && asks.empty() && !notes && to_customer.empty();
}

CoachDecisions parse_decisions(std::string_view text) {
    CoachDecisions d;
    std::size_t first = text.size();
    for (const char* t : kTags) {
        const std::string open = std::string("<") + t;
        for (std::size_t p = text.find(open); p != std::string_view::npos; p = text.find(open, p + 1)) {
            const char next = p + open.size() < text.size() ? text[p + open.size()] : ' ';
            if (next == '>' || next == ' ' || next == '\n') {
                first = std::min(first, p);
                break;
            }
        }
    }
    d.reasoning = std::string(trim(text.substr(0, first)));
    if (d.reasoning.size() > 3000) d.reasoning = d.reasoning.substr(0, 3000) + " [...]";
    for (const ReportBlock& b : report_blocks(text, "task")) {
        CoachTask t;
        t.to = normalize_handle(block_attr(b.attrs, "to"));
        t.id = block_attr(b.attrs, "id");
        while (!t.id.empty() && t.id[0] == '^') t.id.erase(0, 1);
        t.priority = priority_of(block_attr(b.attrs, "priority"));
        t.depends = words_of(block_attr(b.attrs, "depends"));
        t.tags = words_of(block_attr(b.attrs, "tags"));
        auto lines = split_lines(b.body);
        bool titled = false;
        for (const std::string& l : lines) {
            const std::string_view tl = trim(l);
            if (tl.empty()) continue;
            if (!titled) {
                t.title = std::string(tl);
                titled = true;
            } else if (auto c = criterion_line(tl)) {
                t.criteria.push_back(*c);
            } else {
                std::string n(tl);
                if (starts_with(n, "- ")) n.erase(0, 2);
                t.notes.push_back(n);
            }
        }
        if (!t.title.empty()) d.tasks.push_back(std::move(t));
    }
    for (const ReportBlock& b : report_blocks(text, "change")) {
        CoachChange c;
        c.id = block_attr(b.attrs, "id");
        while (!c.id.empty() && c.id[0] == '^') c.id.erase(0, 1);
        c.status = change_status(block_attr(b.attrs, "status"));
        c.to = normalize_handle(block_attr(b.attrs, "to"));
        c.priority = priority_of(block_attr(b.attrs, "priority"));
        if (b.attrs.find("depends") != std::string::npos) c.depends = words_of(block_attr(b.attrs, "depends"));
        c.note = b.body;
        if (!c.id.empty()) d.changes.push_back(std::move(c));
    }
    for (const ReportBlock& b : report_blocks(text, "answer")) {
        CoachAnswer a;
        a.id = block_attr(b.attrs, "id");
        while (!a.id.empty() && a.id[0] == '^') a.id.erase(0, 1);
        a.text = b.body;
        if (!a.id.empty() && !a.text.empty()) d.answers.push_back(std::move(a));
    }
    for (const ReportBlock& b : report_blocks(text, "ask")) {
        CoachAsk a;
        a.to = words_of(block_attr(b.attrs, "to"));
        for (auto& h : a.to) h = normalize_handle(h);
        a.topic = block_attr(b.attrs, "topic");
        a.question = b.body;
        if (a.topic.empty()) a.topic = first_line(a.question, 40);
        if (!a.to.empty() && !a.question.empty()) d.asks.push_back(std::move(a));
    }
    for (const ReportBlock& b : report_blocks(text, "coaching")) {
        CoachCoaching c{normalize_handle(block_attr(b.attrs, "to")), b.body};
        if (!c.to.empty()) d.coaching.push_back(std::move(c));
    }
    d.skills = parse_skill_blocks(text);
    if (!report_blocks(text, "notes").empty()) d.notes = last_body(text, "notes");
    d.to_customer = last_body(text, "to_customer");
    d.goal = last_body(text, "goal");
    d.milestone = last_body(text, "milestone");
    d.overrule = last_body(text, "overrule");
    return d;
}

std::string render_decisions(const CoachDecisions& d) {
    std::string out;
    auto ids = [](const std::vector<std::string>& v) { return join(v, " "); };
    for (const CoachTask& t : d.tasks) {
        out += "<task to=\"" + t.to + "\"" + (t.id.empty() ? "" : " id=\"" + t.id + "\"") +
               (t.priority ? " priority=\"" + std::to_string(*t.priority) + "\"" : "") +
               (t.depends.empty() ? "" : " depends=\"" + ids(t.depends) + "\"") +
               (t.tags.empty() ? "" : " tags=\"" + ids(t.tags) + "\"") + ">\n" + t.title + "\n";
        for (const auto& n : t.notes) out += n + "\n";
        for (const auto& c : t.criteria) out += "accept: " + c + "\n";
        out += "</task>\n";
    }
    for (const CoachChange& c : d.changes) {
        out += "<change id=\"" + c.id + "\"";
        if (c.status) {
            static const std::map<TaskStatus, const char*> word = {
                {TaskStatus::Pending, "pending"},     {TaskStatus::InProgress, "in_progress"}, {TaskStatus::Review, "review"},
                {TaskStatus::Blocked, "blocked"},     {TaskStatus::Completed, "done"},          {TaskStatus::Cancelled, "cancelled"}};
            out += std::string(" status=\"") + word.at(*c.status) + "\"";
        }
        if (!c.to.empty()) out += " to=\"" + c.to + "\"";
        if (c.priority) out += " priority=\"" + std::to_string(*c.priority) + "\"";
        if (c.depends) out += " depends=\"" + ids(*c.depends) + "\"";
        out += ">" + c.note + "</change>\n";
    }
    for (const CoachAnswer& a : d.answers) out += "<answer id=\"" + a.id + "\">" + a.text + "</answer>\n";
    for (const CoachAsk& a : d.asks)
        out += "<ask to=\"" + ids(a.to) + "\" topic=\"" + a.topic + "\">" + a.question + "</ask>\n";
    for (const CoachCoaching& c : d.coaching) out += "<coaching to=\"" + c.to + "\">" + c.text + "</coaching>\n";
    for (const Skill& s : d.skills) out += skill_block(s) + "\n";
    if (d.notes) out += "<notes>" + *d.notes + "</notes>\n";
    if (!d.to_customer.empty()) out += "<to_customer>" + d.to_customer + "</to_customer>\n";
    if (!d.goal.empty()) out += "<goal>" + d.goal + "</goal>\n";
    if (!d.milestone.empty()) out += "<milestone>" + d.milestone + "</milestone>\n";
    return out;
}

GateReview parse_gate_review(std::string_view text) {
    GateReview g;
    if (auto o = report_blocks(text, "object"); !o.empty()) {
        g.approve = false;
        g.text = o.back().body;
    } else {
        g.text = last_body(text, "approve");
    }
    return g;
}

GateVerdict parse_verdict(std::string_view text) {
    GateVerdict v;
    auto bs = report_blocks(text, "verdict");
    if (bs.empty()) return v;
    const std::string st = lower(block_attr(bs.back().attrs, "status"));
    if (st == "accept" || st == "accepted" || st == "pass") v.accept = true;
    else if (st == "reject" || st == "rejected" || st == "fail" || st == "send_back" || st == "sent_back") v.accept = false;
    v.text = bs.back().body;
    return v;
}

// ---------------------------------------------------------------------------
// The customer
// ---------------------------------------------------------------------------

fs::path conversation_path(const Project& project) { return project.customer_dir() / "conversation.md"; }

std::vector<CustomerMessage> load_conversation(const Project& project) {
    std::vector<CustomerMessage> out;
    auto text = try_read_file(conversation_path(project));
    if (!text) return out;
    for (const std::string& line : split_lines(*text)) {
        if (starts_with(line, "### ")) {
            CustomerMessage m;
            std::string head = line.substr(4);
            m.who = starts_with(head, "Customer") ? "customer" : "coach";
            if (auto dot = head.rfind(" · "); dot != std::string::npos) m.when = head.substr(dot + std::string(" · ").size());
            out.push_back(std::move(m));
        } else if (!out.empty()) {
            out.back().text += line + "\n";
        }
    }
    for (auto& m : out) m.text = std::string(trim(m.text));
    return out;
}

void append_conversation(const Project& project, const std::string& who, const std::string& name, const std::string& text) {
    const fs::path p = conversation_path(project);
    std::string head = who == "customer" ? "Customer" : "Coach (" + name + ")";
    if (!fs::exists(p)) append_file(p, "# The customer and the coach\n");
    append_file(p, "\n### " + head + " · " + iso_timestamp() + "\n\n" + std::string(trim(text)) + "\n");
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

fs::path state_path(const Project& p) { return p.state_dir() / "coach" / "state.json"; }

std::vector<std::string> strings_of(const Json* j) {
    std::vector<std::string> out;
    if (j && j->is_array())
        for (const Json& x : j->as_array())
            if (x.is_string()) out.push_back(x.as_string());
    return out;
}

Json json_strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

}  // namespace

CoachState load_coach_state(const Project& project) {
    CoachState s;
    auto text = try_read_file(state_path(project));
    if (!text) return s;
    try {
        Json j = Json::parse(*text);
        auto str = [&](const char* k) {
            const Json* v = j.find(k);
            return v && v->is_string() ? v->as_string() : std::string();
        };
        auto num = [&](const char* k) {
            const Json* v = j.find(k);
            return v && v->is_number() ? v->as_number() : 0.0;
        };
        s.news = strings_of(j.find("news"));
        s.seen_tasks = strings_of(j.find("seen_tasks"));
        s.customer_seen = static_cast<int>(num("customer_seen"));
        s.objection = str("objection");
        s.held = str("held");
        s.idle = static_cast<int>(num("idle"));
        s.urgent = static_cast<int>(num("urgent"));
        s.quiet = static_cast<int>(num("quiet"));
        s.steps = static_cast<int>(num("steps"));
        s.decisions = static_cast<int>(num("decisions"));
        s.milestones = static_cast<int>(num("milestones"));
        s.cost = num("cost");
        s.last_decision = str("last_decision");
    } catch (const std::exception&) {
    }
    return s;
}

void save_coach_state(const Project& project, const CoachState& s) {
    Json j = Json::object();
    j.set("news", json_strings(s.news));
    j.set("seen_tasks", json_strings(s.seen_tasks));
    j.set("customer_seen", s.customer_seen);
    j.set("objection", s.objection);
    j.set("held", s.held);
    j.set("idle", s.idle);
    j.set("urgent", s.urgent);
    j.set("quiet", s.quiet);
    j.set("steps", s.steps);
    j.set("decisions", s.decisions);
    j.set("milestones", s.milestones);
    j.set("cost", s.cost);
    j.set("last_decision", s.last_decision);
    write_file_atomic(state_path(project), j.dump(2) + "\n");
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

namespace {

const char* kCoachSystem =
    "You are the coach of a crew of AI experts building a product for a customer. You decide what the crew "
    "works on, who does it and what counts as done; the experts do the work in their own units; the "
    "Gatekeeper checks every decision and every piece of work. You never see files: you see a digest of "
    "the plan and of what happened. You answer with a short rationale and decision blocks, nothing else.";

const char* kDecisionSpec =
    "Answer with a few lines of reasoning, then the blocks. Every block is optional: decide only what\n"
    "needs deciding now, and leave running work alone. A few well-cut tasks with checkable acceptance\n"
    "criteria beat many small ones; each task is for one expert, in its unit.\n"
    "\n"
    "<task to=\"handle\" id=\"short-id\" priority=\"1\" depends=\"other-id another-id\" tags=\"docs\">\n"
    "Title, one line\n"
    "Notes the expert needs: why, the inputs, where things are, what not to do.\n"
    "accept: a criterion the Gatekeeper can check (one per line)\n"
    "</task>\n"
    "    tags=\"docs\" for a task that only changes documentation or skills: a lighter check.\n"
    "<change id=\"task-id\" status=\"cancelled|pending|blocked|done\" to=\"handle\" priority=\"2\" depends=\"id ...\">\n"
    "Why (it becomes a note on the task)</change>\n"
    "<answer id=\"task-id\">Your decision on a task waiting for you, or the answer to an expert's question.</answer>\n"
    "<ask to=\"apidev clidev gatekeeper\" topic=\"short-topic\">A question for their views (experts' handles, or a\n"
    "unit's name for its first expert); they answer before your\n"
    "next decisions. Ask when a decision crosses units or you are unsure; not for routine work.</ask>\n"
    "<coaching to=\"handle\">What this expert should keep in mind from now on (replaces your earlier coaching).</coaching>\n"
    "<skill name=\"Name\" triggers=\"word, word\" members=\"handle\">One line: when to use it\n"
    "The how, short and concrete.</skill>\n"
    "    A lesson worth keeping for everyone. When the lesson is a workaround for a tool's gap, give\n"
    "    the tool's team a task instead (or too): skills are the tools' backlog.\n"
    "<notes>Your own notes for your next decisions (they replace the earlier ones): the plan in your\n"
    "head, open questions, what to watch.</notes>\n"
    "<to_customer>A message to the customer: plain and short.</to_customer>\n"
    "<goal>The goal now, once the customer agreed it.</goal>\n"
    "<milestone>The goal is met: what the customer can see now, and where.</milestone>\n"
    "\n"
    "The Gatekeeper reviews your plan decisions before they apply. When it objects, revise them, or\n"
    "repeat them with <overrule>why you go ahead anyway</overrule> (on record).\n";

const char* kGateSystem =
    "You are the Gatekeeper of a crew of AI experts: the coach's skeptical right arm. You trust nothing "
    "you have not seen evidence for. You review the coach's decisions; you do not decide. You answer with "
    "a short reason and one block.";

const char* kGateAcceptance =
    "You check this task's work before it counts: the expert reported it done (its report is in the\n"
    "task's notes). Check each acceptance criterion yourself, in this repository: read the files, run\n"
    "the tests and checks (you cannot change files, and do not try). Accept only what you checked. A\n"
    "claim without evidence, a criterion skipped without a good reason, or work that is not what the\n"
    "task asked for goes back. Be concrete when you send it back: what is missing, and how you will\n"
    "check it next time.\n"
    "\n"
    "End your final message with:\n"
    "\n"
    "<criterion n=\"1\" status=\"verified\">What you checked, and what you saw.</criterion>\n"
    "    one per criterion: verified, failed, or skipped (cannot be checked yet: why)\n"
    "<verdict status=\"accept\">Why it counts.</verdict>\n"
    "    or <verdict status=\"reject\">What is missing or wrong, for the expert.</verdict>\n"
    "<finding>A gap in a tool you saw (name the tool first).</finding>   (optional)\n"
    "<lesson>What you learned about checking this kind of work.</lesson>   (optional)\n";

std::string status_mark(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending: return "[ ]";
        case TaskStatus::InProgress: return "[~]";
        case TaskStatus::Review: return "[?]";
        case TaskStatus::Blocked: return "[!]";
        case TaskStatus::Completed: return "[x]";
        case TaskStatus::Cancelled: return "[-]";
    }
    return "[ ]";
}

int status_rank(TaskStatus s) {
    switch (s) {
        case TaskStatus::Review: return 0;
        case TaskStatus::InProgress: return 1;
        case TaskStatus::Blocked: return 2;
        default: return 3;
    }
}

/// The last note that is not bookkeeping.
std::string last_note(const taskml::Task& t) {
    for (auto it = t.notes.rbegin(); it != t.notes.rend(); ++it)
        if (!starts_with(*it, "depends:") && !starts_with(*it, "Needed by") && !starts_with(*it, "blocked-by:"))
            return first_line(*it, 200);
    return "";
}

std::string task_line(const Plan& plan, const TaskRef& r) {
    const taskml::Task& t = *r.task;
    std::string out = "- " + status_mark(t.status) + " " + (t.id ? "^" + *t.id + " " : "") + first_line(t.description, 120);
    if (t.priority) out += " #p" + std::to_string(*t.priority);
    if (t.assignee) out += " @" + *t.assignee;
    std::vector<std::string> waits;
    for (const auto& d : plan.unmet_dependencies(r)) waits.push_back("^" + d);
    if (!waits.empty()) out += " — waits for " + join(waits, ", ");
    std::vector<std::string> blocks;
    for (const auto& b : plan.active_blockers(r)) blocks.push_back("^" + b);
    if (!blocks.empty()) out += " — blocked by " + join(blocks, ", ");
    const std::string n = last_note(t);
    if (!n.empty()) out += "\n    last: " + n;
    return out;
}

}  // namespace

namespace {

/// A task for documentation or skills only: tagged #docs, or its title is about docs or a skill
/// and names no code.
bool docs_only(const taskml::Task& t) {
    if (std::find(t.tags.begin(), t.tags.end(), "docs") != t.tags.end()) return true;
    const std::string d = lower(t.description);
    auto any = [&](std::initializer_list<const char*> ws) {
        for (const char* w : ws)
            if (d.find(w) != std::string::npos) return true;
        return false;
    };
    const bool about_docs = any({"docs/", "doc ", "docs ", "document", "skill", "readme", "user_guide", "user guide", ".md"});
    const bool about_code = any({".py", ".cpp", ".hpp", ".v ", ".v`", ".sv", "test", "fix", "bug", "fails", "error",
                                 "export", "simulate", "rtl", "script"});
    return about_docs && !about_code;
}

}  // namespace

// ---------------------------------------------------------------------------
// The coach
// ---------------------------------------------------------------------------

Coach::Coach(const Project& project, const Team& team, RunnerFactory runners)
    : project_(project), team_(team), runners_(runners), loop_(project, team, runners) {
    coach_ = team.find(team.coach);
    if (!coach_) coach_ = &team.product_owner_member();
    gatekeeper_ = team.find(team.gatekeeper);
    if (!gatekeeper_) gatekeeper_ = coach_;
}

std::vector<std::string> Coach::waiting_for_coach() const {
    std::vector<std::string> out;
    Plan plan = Plan::load(project_.plan_path());
    for (const TaskRef& r : plan.tasks())
        if (r.task->assignee && normalize_handle(*r.task->assignee) == coach_->handle && !is_finished(r.task->status) &&
            r.task->id)
            out.push_back(*r.task->id);
    return out;
}

bool Coach::needs_coach(const CoachState& st) const {
    if (st.urgent > 0 || !st.objection.empty()) return true;
    // Routine news (progress, an unblocked task) waits for something that changes the picture,
    // or for quiet_steps steps: the coach is the costliest run of a step.
    if (!st.news.empty() && st.quiet >= std::max(0, project_.config().coach.quiet_steps)) return true;
    if (static_cast<int>(load_conversation(project_).size()) > st.customer_seen) {
        // Only the customer's messages are news (the coach's own are not).
        auto conv = load_conversation(project_);
        for (std::size_t i = static_cast<std::size_t>(std::max(0, st.customer_seen)); i < conv.size(); ++i)
            if (conv[i].who == "customer") return true;
    }
    for (const auto& id : waiting_for_coach())
        if (std::find(st.seen_tasks.begin(), st.seen_tasks.end(), id) == st.seen_tasks.end()) return true;
    return false;
}

std::string Coach::digest() const { return digest_for(load_coach_state(project_), false); }

std::string Coach::digest_for(const CoachState& st, bool for_gatekeeper) const {
    Plan plan = Plan::load(project_.plan_path());
    std::string out;
    // The project: the brief (the coach's own), without front matter (ORG.md's units and rules
    // are the experts': the crew below says who works where).
    {
        std::string brief;
        auto text = try_read_file(project_.brief_path());
        brief = text ? std::string(trim(*text)) : project_.brief();
        if (starts_with(brief, "---\n"))
            if (auto end = brief.find("\n---", 4); end != std::string::npos) brief = std::string(trim(brief.substr(end + 4)));
        const std::size_t limit = std::max<std::size_t>(project_.config().brief_limit, 12000);
        if (brief.size() > limit) brief = brief.substr(0, limit) + "\n[the brief goes on in " + project_.config().brief + "]";
        out += "# The project\n\n" + brief + "\n\n";
    }
    const std::string goal = plan.directive("goal");
    out += "# The goal\n\n" + (goal.empty() ? std::string("No goal yet: agree on one with the customer, then set it with <goal>.") : goal) +
           "\n\n";

    // The crew, with what each has.
    std::map<std::string, std::map<TaskStatus, int>> load;
    for (const TaskRef& r : plan.tasks())
        if (r.task->assignee && !is_finished(r.task->status)) ++load[normalize_handle(*r.task->assignee)][r.task->status];
    out += "# The crew\n\n";
    for (const Member& m : team_.members) {
        out += "- @" + m.handle + " — " + m.name + ", " + m.role;
        if (!m.unit.empty()) out += " (" + m.unit + "/)";
        if (m.handle == coach_->handle) out += " — you";
        const auto& l = load[m.handle];
        auto n = [&](TaskStatus s) { auto it = l.find(s); return it == l.end() ? 0 : it->second; };
        const int open = n(TaskStatus::Pending) + n(TaskStatus::InProgress);
        if (open || n(TaskStatus::Review) || n(TaskStatus::Blocked))
            out += " · open " + std::to_string(open) + ", review " + std::to_string(n(TaskStatus::Review)) +
                   ", blocked " + std::to_string(n(TaskStatus::Blocked));
        if (!m.summary.empty()) out += "\n    " + m.summary;
        out += "\n";
    }
    out += "\n";

    // What happened.
    out += "# News since the last decisions\n\n";
    if (st.news.empty()) out += "Nothing new.\n";
    for (const auto& n : st.news) out += "- " + n + "\n";
    out += "\n";

    // The customer.
    auto conv = load_conversation(project_);
    out += "# The customer\n\n";
    bool any = false;
    // The last coach message, for context, then what came after.
    const std::size_t from = static_cast<std::size_t>(std::clamp(st.customer_seen, 0, static_cast<int>(conv.size())));
    for (std::size_t i = from; i < conv.size(); ++i) {
        if (conv[i].who != "customer") continue;
        out += "New from the customer (" + conv[i].when + "):\n\n" + conv[i].text + "\n\n";
        any = true;
    }
    if (!any) {
        std::string last;
        for (auto it = conv.rbegin(); it != conv.rend(); ++it)
            if (it->who == "customer") {
                last = "The customer's last message (" + it->when + ", answered):\n\n" + first_line(it->text, 600) + "\n\n";
                break;
            }
        out += last.empty() ? "The customer has not written yet.\n\n" : last;
    }

    if (!st.objection.empty() && !for_gatekeeper) {
        out += "# The Gatekeeper objected to your last decisions (they were not applied)\n\n" + st.objection +
               "\n\nYour held decisions:\n\n" + st.held + "\n" +
               "Revise them, or repeat them with <overrule>why</overrule>.\n\n";
    }

    // Tasks waiting for the coach.
    std::vector<const TaskRef*> mine, operator_tasks, unassigned;
    std::vector<const TaskRef*> open;
    for (const TaskRef& r : plan.tasks()) {
        if (is_finished(r.task->status)) continue;
        if (!r.task->assignee) {
            unassigned.push_back(&r);
            continue;
        }
        const std::string h = normalize_handle(*r.task->assignee);
        if (h == coach_->handle) mine.push_back(&r);
        else if (h == kOperator) operator_tasks.push_back(&r);
        else open.push_back(&r);
    }
    if (!for_gatekeeper) {
        out += "# Waiting for your decision\n\n";
        if (mine.empty()) out += "Nothing.\n";
        for (const TaskRef* r : mine) {
            out += task_line(plan, *r) + "\n";
            int k = 0;
            for (const auto& n : r->task->notes)
                if (++k <= 6) out += "    " + first_line(n, 400) + "\n";
        }
        out += "\n";
    }
    if (!operator_tasks.empty()) {
        out += "# Waiting for the operator (the person running the crew)\n\n";
        for (const TaskRef* r : operator_tasks) out += task_line(plan, *r) + "\n";
        out += "\n";
    }
    std::stable_sort(open.begin(), open.end(), [](const TaskRef* a, const TaskRef* b) {
        int ra = status_rank(a->task->status), rb = status_rank(b->task->status);
        if (ra != rb) return ra < rb;
        return a->task->priority.value_or(2) < b->task->priority.value_or(2);
    });
    out += "# Open work (" + std::to_string(open.size()) + " tasks)\n\n";
    const std::size_t cap = static_cast<std::size_t>(std::max(10, project_.config().coach.digest_tasks));
    for (std::size_t i = 0; i < open.size() && i < cap; ++i) out += task_line(plan, *open[i]) + "\n";
    if (open.size() > cap) out += "- ... and " + std::to_string(open.size() - cap) + " more (lower priority)\n";
    if (!unassigned.empty()) {
        out += "\n# Unassigned (give each an expert, or cancel it)\n\n";
        for (std::size_t i = 0; i < unassigned.size() && i < cap; ++i) out += task_line(plan, *unassigned[i]) + "\n";
        if (unassigned.size() > cap) out += "- ... and " + std::to_string(unassigned.size() - cap) + " more\n";
    }
    out += "\n";

    // What the crew knows.
    if (!for_gatekeeper) {
        std::vector<Skill> skills = load_skills(project_);
        if (!skills.empty()) {
            out += "# The crew's skills\n\n";
            for (const Skill& s : skills) out += "- " + s.name + ": " + s.description + "\n";
            out += "\n";
        }
        const Memory mine_mem = load_memory(project_, coach_->handle);
        out += "# Your notes\n\n" + (mine_mem.other.empty() ? std::string("None yet.") : mine_mem.other) + "\n";
    }
    return out;
}

std::string Coach::coach_prompt() const { return prompt_for(load_coach_state(project_)); }

std::string Coach::prompt_for(const CoachState& st) const {
    std::string out = "# You\n\nYou are " + coach_->name + " (@" + coach_->handle + "), the coach.\n";
    if (!coach_->prompt.empty()) out += "\n" + coach_->prompt + "\n";
    out += "\n" + digest_for(st, false);
    out += "\n# How to decide\n\n" + std::string(kDecisionSpec);
    return out;
}

bool Coach::talk(CoachStep& s, const CoachOptions& o, const Member& who, const std::string& key, const std::string& what,
                 const std::string& system, const std::string& prompt, std::string& answer) {
    DispatchOutcome out;
    DispatchPlan& dp = out.plan;
    dp.task_key = key;
    dp.description = what;
    dp.member = &who;
    dp.model = who.models.standard;
    if (!o.loop.runner.empty() && dp.model.runner != o.loop.runner) dp.model = ModelRef{o.loop.runner, ""};
    dp.reason = what;
    dp.talk = true;
    dp.system_prompt = system;
    dp.context.sections.push_back({"talk", "", prompt});
    out.executed = true;
    out.run = loop_.run(dp, o.loop, out.run_dir);
    out.report.summary = first_line(out.run.final_text, 400);
    if (!out.run.ok) out.failure = to_string(out.run.failure);
    s.cost += out.run.usage.cost_usd;
    if (out.run.failure == Failure::RateLimit) s.rate_limited = true;
    std::error_code ec;
    std::string e = "## " + iso_timestamp() + " · " + what + " (@" + who.handle + ")\n\n";
    e += "- Model: " + dp.model.str() + "\n";
    if (!out.run.ok) e += "- Failed: " + first_line(out.run.error, 300) + "\n";
    e += "- Usage: " + Loop::usage_text(out.run.usage) + "\n";
    e += "- Run: `" + fs::relative(out.run_dir, project_.dir(), ec).string() + "`\n";
    loop_.record(out, e);
    answer = out.run.final_text;
    const bool ok = out.run.ok && !trim(answer).empty();
    if (!ok) s.problems.push_back(what + " by @" + who.handle + " failed: " + first_line(out.run.error, 200));
    s.runs.push_back(std::move(out));
    return ok;
}

// --- 1. tasks -------------------------------------------------------------------------------

void Coach::task_round(CoachStep& s, CoachState& st, const CoachOptions& o) {
    LoopOptions lo = o.loop;
    lo.max_dispatches = std::max(1, project_.config().coach.tasks_per_step);
    lo.skip_members = {coach_->handle, gatekeeper_->handle};
    IterationResult r = loop_.iterate(lo);
    for (auto& n : r.notes) s.notes.push_back(n);
    if (r.needs_human) s.needs_human = true;
    for (const auto& u : r.unblocked) st.news.push_back("unblocked " + u);
    for (DispatchOutcome& d : r.dispatches) {
        if (!d.executed) continue;
        s.cost += d.run.usage.cost_usd;
        const std::string who = "@" + d.plan.member->handle;
        const std::string what = d.plan.task_key + " " + first_line(d.plan.description, 80);
        if (!d.run.ok) {
            if (d.run.failure == Failure::RateLimit) s.rate_limited = true;
            else {
                st.news.push_back(what + ": the run by " + who + " failed (" + d.failure + "): " + first_line(d.run.error, 200));
                ++st.urgent;
            }
        } else {
            std::string line = what + " — " + who + ": " +
                               (d.report.status ? status_label(*d.report.status) : std::string("no status"));
            for (const auto& c : d.plan_changes)
                if (starts_with(c, "status → ")) line += " (" + c.substr(std::string("status → ").size()) + ")";
            if (!d.report.summary.empty()) line += "\n    " + first_line(d.report.summary, 500);
            for (const auto& b : d.report.blockers) line += "\n    blocker: " + first_line(b.text, 300);
            for (const auto& c : d.plan_changes)
                if (starts_with(c, "handoff ") || starts_with(c, "finding ") || starts_with(c, "web access "))
                    line += "\n    " + c;
            for (const auto& l : d.lessons) line += "\n    lesson: " + first_line(l, 300);
            for (const auto& g : d.guard) line += "\n    guard: " + first_line(g, 200);
            st.news.push_back(line);
            // A block, or work to check, changes the picture; progress on running work does not.
            if (!d.report.blockers.empty() || (d.report.status && (*d.report.status == TaskStatus::Blocked)) ||
                !d.report.findings.empty() || !d.report.handoffs.empty())
                ++st.urgent;
        }
        s.notes.push_back(what + " by " + who + ": " +
                          (d.run.ok ? (d.report.status ? status_label(*d.report.status) : "no status") : "failed (" + d.failure + ")"));
        s.runs.push_back(std::move(d));
    }
}

// --- 2. acceptance --------------------------------------------------------------------------

void Coach::acceptances(CoachStep& s, CoachState& st, const CoachOptions& o) {
    Plan plan = Plan::load(project_.plan_path());
    std::vector<const TaskRef*> review;
    for (const TaskRef& r : plan.tasks())
        if (r.task->status == TaskStatus::Review && r.task->id) review.push_back(&r);
    std::stable_sort(review.begin(), review.end(), [](const TaskRef* a, const TaskRef* b) {
        return a->task->priority.value_or(2) < b->task->priority.value_or(2);
    });
    std::vector<std::string> ids;
    for (const TaskRef* r : review) ids.push_back(*r->task->id);
    int n = 0;
    for (const std::string& id : ids) {
        if (n++ >= std::max(1, project_.config().coach.acceptances_per_step)) break;
        if (project_.config().coach.step_cost > 0 && s.cost >= project_.config().coach.step_cost) break;
        if (!accept_one(s, st, o, id) && s.rate_limited) break;
    }
}

bool Coach::accept_one(CoachStep& s, CoachState& st, const CoachOptions& o, const std::string& id) {
    Plan plan = Plan::load(project_.plan_path());
    const TaskRef* ref = plan.by_id(id);
    if (!ref || ref->task->status != TaskStatus::Review) return false;
    const Member* expert = ref->task->assignee ? team_.find(*ref->task->assignee) : nullptr;
    const std::string unit = expert ? expert->unit : std::string();
    const Project& env = loop_.env_for(unit);

    DispatchOutcome out;
    DispatchPlan& dp = out.plan;
    dp.task_key = "gate:^" + id;
    dp.description = "Check ^" + id + ": " + first_line(ref->task->description, 80);
    dp.member = gatekeeper_;
    dp.model = gatekeeper_->models.standard;
    // Documentation or skills only: the Gatekeeper's economy model checks it (prose against the
    // code it describes needs reading, not the strongest model); code stays on the standard one.
    if (gatekeeper_->models.economy && docs_only(*ref->task)) dp.model = *gatekeeper_->models.economy;
    if (!o.loop.runner.empty() && dp.model.runner != o.loop.runner) dp.model = ModelRef{o.loop.runner, ""};
    dp.reason = "acceptance";
    dp.permission = Permission::ReadOnly;
    dp.unit = unit;
    dp.max_turns = project_.config().coach.acceptance_turns;
    dp.context = build_task_context({team_, *gatekeeper_, plan, env.brief(), false, false, "",
                                     Loop::protected_paths_text(env), env.config().notes,
                                     skills_section(load_skills(project_), *ref->task, gatekeeper_->handle),
                                     memory_section(project_, gatekeeper_->handle, project_.config().coach.memory_limit)},
                                    *ref);
    for (auto& sec : dp.context.sections) {
        if (sec.id == "task") sec.title = "The task to check";
        if (sec.id == "reporting") {
            sec.title = "How to check";
            sec.body = kGateAcceptance;
        }
    }
    out.executed = true;
    out.run = loop_.run(dp, o.loop, out.run_dir);
    s.cost += out.run.usage.cost_usd;
    if (out.run.failure == Failure::RateLimit) s.rate_limited = true;
    out.report = parse_report(out.run.final_text);
    const GateVerdict v = parse_verdict(out.run.final_text);
    const std::string date = iso_timestamp().substr(0, 10);
    const std::string g = "@" + gatekeeper_->handle;
    bool decided = false;

    if (out.run.ok && v.accept) {
        Plan now = Plan::load(project_.plan_path());
        const TaskRef* r = now.by_id(id);
        if (r) {
            PlanEditor ed(now);
            for (const CriterionReport& c : out.report.criteria)
                if (c.number >= 1 && static_cast<std::size_t>(c.number) <= r->task->criteria.size())
                    ed.set_criterion(*r, static_cast<std::size_t>(c.number - 1), c.status, first_line(c.evidence, 240));
            const int sent_back = static_cast<int>(std::count_if(r->task->notes.begin(), r->task->notes.end(), [](const std::string& n) {
                return starts_with(n, "Sent back by the Gatekeeper");
            }));
            if (*v.accept) {
                ed.set_status(*r, TaskStatus::Completed);
                ed.add_note(*r, "Accepted by the Gatekeeper " + g + " " + date + ": " + first_line(v.text, 400));
                out.plan_changes.push_back("accepted: status → " + status_label(TaskStatus::Completed));
                st.news.push_back("^" + id + " accepted by the Gatekeeper: " + first_line(v.text, 400));
                ++st.urgent;
            } else if (sent_back + 1 > project_.config().coach.max_rejections) {
                BlockerReport b;
                b.kind = BlockerReport::Kind::Member;
                b.to = coach_->handle;
                b.text = "Decide on ^" + id + ": the Gatekeeper sent it back " + std::to_string(sent_back + 1) +
                         " times. Last: " + first_line(v.text, 600);
                BlockerEdits be = record_blockers(now, ed, *r, *gatekeeper_, team_, {b}, "");
                out.plan_changes.insert(out.plan_changes.end(), be.changes.begin(), be.changes.end());
                ++st.urgent;
                st.news.push_back("^" + id + " sent back " + std::to_string(sent_back + 1) +
                                  " times by the Gatekeeper: it waits for your decision. " + first_line(v.text, 400));
            } else {
                ed.set_status(*r, TaskStatus::Pending);
                ed.add_note(*r, "Sent back by the Gatekeeper " + g + " " + date + ": " + first_line(v.text, 800));
                out.plan_changes.push_back("sent back: status → " + status_label(TaskStatus::Pending));
                ++st.urgent;
                st.news.push_back("^" + id + " sent back to @" + (expert ? expert->handle : std::string("?")) +
                                  " by the Gatekeeper: " + first_line(v.text, 400));
            }
            std::vector<std::string> added;
            for (const std::string& f : out.report.findings) {
                const std::string fid = record_finding(now, ed, *r, *gatekeeper_, f, &added, &team_);
                if (!fid.empty()) out.plan_changes.push_back("finding ^" + fid);
            }
            CommitResult c = ed.commit();
            if (!c.ok) {
                out.problems.push_back("plan not updated: " + c.error);
                out.plan_changes.clear();
            } else {
                decided = true;
            }
        }
    } else if (out.run.ok) {
        out.failure = "no-report";
        out.problems.push_back("the Gatekeeper gave no verdict on ^" + id);
    } else {
        out.failure = to_string(out.run.failure);
        out.problems.push_back("the Gatekeeper's check of ^" + id + " failed: " + first_line(out.run.error, 200));
    }
    if (out.run.ok && !out.report.lessons.empty())
        out.lessons = add_lessons(project_, gatekeeper_->handle, id, out.report.lessons, date);
    for (const auto& p : out.problems) s.problems.push_back(p);
    s.notes.push_back("the Gatekeeper on ^" + id + ": " +
                      (decided ? (*v.accept ? std::string("accepted") : std::string("sent back")) : std::string("no verdict")));
    std::error_code ec;
    std::string e = "## " + iso_timestamp() + " · " + dp.description + " (" + g + ")\n\n";
    e += "- Model: " + dp.model.str() + "\n";
    e += "- Verdict: " + (v.accept ? (*v.accept ? std::string("accept") : std::string("reject")) : std::string("none")) + "\n";
    for (const auto& c : out.plan_changes) e += "- Plan: " + c + "\n";
    for (const auto& p : out.problems) e += "- Problem: " + p + "\n";
    e += "- Usage: " + Loop::usage_text(out.run.usage) + "\n";
    e += "- Run: `" + fs::relative(out.run_dir, project_.dir(), ec).string() + "`\n";
    if (!v.text.empty()) e += "\n" + v.text + "\n";
    loop_.record(out, e);
    s.runs.push_back(std::move(out));
    return decided;
}

// --- 3. decisions ---------------------------------------------------------------------------

void Coach::decide(CoachStep& s, CoachState& st, const CoachOptions& o, bool idle) {
    const bool revising = !st.objection.empty();
    const std::string digest = digest_for(st, true);
    const std::string prompt = prompt_for(st);
    std::string answer;
    if (!talk(s, o, *coach_, "coach", "Coach decisions", kCoachSystem, prompt, answer)) return;
    s.decided = true;
    st.decisions++;
    st.last_decision = iso_timestamp();
    // What the coach saw is consumed.
    st.news.clear();
    st.urgent = 0;
    st.quiet = 0;
    st.customer_seen = static_cast<int>(load_conversation(project_).size());
    st.seen_tasks = waiting_for_coach();
    CoachDecisions d = parse_decisions(answer);
    if (d.empty()) {
        st.idle = idle ? st.idle + 1 : 0;
        s.notes.push_back("the coach decided nothing" + std::string(idle ? " (the crew is idle)" : ""));
        if (revising) {
            st.objection.clear();
            st.held.clear();
        }
        return;
    }
    st.idle = 0;
    if (d.needs_review() && d.overrule.empty()) {
        std::string gp = "# You\n\nYou are " + gatekeeper_->name + " (@" + gatekeeper_->handle + "), the Gatekeeper.\n";
        if (!gatekeeper_->prompt.empty()) gp += "\n" + gatekeeper_->prompt + "\n";
        gp += "\n" + digest;
        gp += "\n# The coach's decisions to review\n\nThe coach's reasoning:\n\n" + d.reasoning + "\n\n" + render_decisions(d) + "\n";
        gp += "# How to answer\n\n"
              "Approve, or object. Object to what is wrong, unsupported or wasteful: a task without criteria\n"
              "you could check, work that duplicates open work or that nobody asked for, a decision that\n"
              "contradicts a contract, the customer or the evidence, a milestone not backed by accepted work.\n"
              "Do not object to style. End with one block:\n\n"
              "<approve>Why it holds (one or two sentences).</approve>\n"
              "or\n"
              "<object>What is wrong, and what would fix it.</object>\n";
        if (revising) gp += "\nThe coach revised these after your objection: " + st.objection + "\n";
        std::string review;
        if (!talk(s, o, *gatekeeper_, "gatekeeper", "The Gatekeeper reviews the coach's decisions", kGateSystem, gp, review)) {
            // No review (a failure, the rate limit): nothing applies; the coach decides again.
            st.objection = "(The Gatekeeper's review did not run: decide again.)";
            st.held = render_decisions(d);
            return;
        }
        GateReview gr = parse_gate_review(review);
        if (!gr.approve && !revising) {
            st.objection = gr.text.empty() ? first_line(review, 800) : gr.text;
            st.held = render_decisions(d);
            s.notes.push_back("the Gatekeeper objected to the coach's decisions: " + first_line(st.objection, 200));
            // What needs no review still applies: the coach's notes and its questions.
            CoachDecisions free;
            free.notes = d.notes;
            free.asks = d.asks;
            apply(s, st, free);
            for (const CoachAsk& a : d.asks) council(s, st, o, a);
            return;
        }
        if (!gr.approve) {
            // All of it: an objection is often a list, and its first line alone loses the items.
            std::string text(trim(gr.text.empty() ? review : gr.text));
            if (text.size() > 3000) text = text.substr(0, 3000) + " [...]";
            ++st.urgent;
            st.news.push_back("The Gatekeeper still objects (your revised decisions applied; it may object once, so "
                              "act on what is right in it with new decisions):\n" + text);
        }
    }
    if (!d.overrule.empty()) {
        ++st.urgent;
        st.news.push_back("You overruled the Gatekeeper: " + first_line(d.overrule, 300) +
                          (st.objection.empty() ? "" : "\n    its objection was: " + std::string(trim(st.objection)).substr(0, 2000)));
        s.notes.push_back("the coach overruled the Gatekeeper");
    }
    st.objection.clear();
    st.held.clear();
    apply(s, st, d);
    for (const CoachAsk& a : d.asks) council(s, st, o, a);
}

void Coach::commit_skill(CoachStep& s, const Skill& sk, const fs::path& file) {
    if (!is_git_repo(project_)) return;
    std::error_code ec;
    const std::string rel = fs::relative(file.parent_path(), project_.dir(), ec).string();
    auto git = [&](std::vector<std::string> args) {
        ProcessOptions po;
        po.argv = {"git", "-C", project_.dir().string()};
        po.argv.insert(po.argv.end(), args.begin(), args.end());
        return run_process(po, [](std::string_view) {}).exit_code == 0;
    };
    if (!git({"add", "--", rel}) ||
        !git({"commit", "-q", "-m", "skill: " + sk.name + " (the coach)", "--only", "--", rel}))
        s.problems.push_back("could not commit the skill " + rel + " (it is written)");
}

void Coach::apply(CoachStep& s, CoachState& st, const CoachDecisions& d) {
    const std::string date = iso_timestamp().substr(0, 10);
    const std::string by = "@" + coach_->handle;
    std::vector<std::string> changes, problems;

    Plan plan = Plan::load(project_.plan_path());
    PlanEditor ed(plan);
    std::set<std::string> new_ids;
    auto known = [&](const std::string& id) { return plan.by_id(id) || new_ids.count(id); };
    auto assignable = [&](const std::string& h) { return h == kOperator || team_.find(h); };
    for (const CoachTask& t : d.tasks) {
        if (!assignable(t.to)) {
            problems.push_back("task \"" + first_line(t.title, 60) + "\" for unknown @" + t.to + ": not added");
            continue;
        }
        NewTask nt;
        nt.description = t.title;
        nt.assignee = t.to;
        nt.priority = t.priority;
        nt.id = t.id;
        nt.tags = t.tags;
        for (const auto& dep : t.depends) {
            if (known(dep)) nt.depends_on.push_back(dep);
            else problems.push_back("task \"" + first_line(t.title, 60) + "\": depends on unknown ^" + dep + " (left out)");
        }
        nt.notes = t.notes;
        nt.notes.push_back("From the coach " + date);
        nt.criteria = t.criteria;
        if (nt.criteria.empty())
            problems.push_back("task \"" + first_line(t.title, 60) + "\" has no acceptance criteria: the Gatekeeper has nothing to check");
        const std::string id = ed.add_to_section("Work", nt);
        new_ids.insert(id);
        changes.push_back("task ^" + id + " → @" + t.to + ": " + first_line(t.title, 100));
    }
    for (const CoachChange& c : d.changes) {
        const TaskRef* r = plan.by_id(c.id);
        if (!r) {
            problems.push_back("change of unknown ^" + c.id);
            continue;
        }
        std::vector<std::string> what;
        if (c.status) {
            ed.set_status(*r, *c.status);
            what.push_back("status → " + status_label(*c.status));
        }
        if (!c.to.empty()) {
            if (!assignable(c.to)) {
                problems.push_back("change of ^" + c.id + ": unknown @" + c.to);
            } else {
                ed.set_assignee(*r, c.to);
                what.push_back("→ @" + c.to);
            }
        }
        if (c.priority) {
            ed.set_priority(*r, *c.priority);
            what.push_back("#p" + std::to_string(*c.priority));
        }
        if (c.depends) {
            for (const auto& old : r->task->depends_on)
                if (std::find(c.depends->begin(), c.depends->end(), old) == c.depends->end()) ed.remove_depends(*r, old);
            for (const auto& dep : *c.depends) {
                if (!known(dep)) {
                    problems.push_back("change of ^" + c.id + ": depends on unknown ^" + dep + " (left out)");
                    continue;
                }
                if (std::find(r->task->depends_on.begin(), r->task->depends_on.end(), dep) == r->task->depends_on.end())
                    ed.add_depends(*r, dep);
            }
            what.push_back("depends: " + join(*c.depends, ", "));
        }
        if (!trim(c.note).empty()) ed.add_note(*r, "Coach " + date + ": " + first_line(c.note, 600));
        changes.push_back("^" + c.id + " " + (what.empty() ? std::string("noted") : join(what, ", ")));
    }
    for (const CoachAnswer& a : d.answers) {
        const TaskRef* r = plan.by_id(a.id);
        if (!r) {
            problems.push_back("answer to unknown ^" + a.id);
            continue;
        }
        ed.add_note(*r, "Answer from the coach " + date + ": " + first_line(a.text, 1000));
        // A decision waiting for the coach is made by answering it.
        if (r->task->assignee && normalize_handle(*r->task->assignee) == coach_->handle && !is_finished(r->task->status))
            ed.set_status(*r, TaskStatus::Completed);
        changes.push_back("answered ^" + a.id);
    }
    if (!d.goal.empty()) {
        ed.set_directive("goal", first_line(d.goal, 400));
        changes.push_back("goal: " + first_line(d.goal, 200));
    }
    if (ed.changed()) {
        CommitResult c = ed.commit();
        if (!c.ok) {
            problems.push_back("the plan was not changed: " + c.error);
            for (const auto& x : c.diagnostics) problems.push_back("  " + x.format());
            changes.clear();
        }
    }
    for (const CoachCoaching& c : d.coaching) {
        if (!team_.find(c.to)) {
            problems.push_back("coaching for unknown @" + c.to);
            continue;
        }
        set_coaching(project_, c.to, c.text);
        changes.push_back("coaching for @" + c.to);
    }
    for (Skill sk : d.skills) {
        if (sk.source.empty()) sk.source = "the coach, " + date;
        const fs::path f = write_skill(project_, sk);
        commit_skill(s, sk, f);
        changes.push_back("skill: " + sk.name);
    }
    if (d.notes) {
        Memory m = load_memory(project_, coach_->handle);
        m.other = std::string(trim(*d.notes));
        save_memory(project_, coach_->handle, m);
    }
    if (!d.to_customer.empty()) {
        append_conversation(project_, "coach", coach_->name, d.to_customer);
        st.customer_seen = static_cast<int>(load_conversation(project_).size());
        changes.push_back("message to the customer");
    }
    if (!d.milestone.empty()) {
        s.milestone = true;
        st.milestones++;
        append_conversation(project_, "coach", coach_->name, "Milestone: " + d.milestone);
        st.customer_seen = static_cast<int>(load_conversation(project_).size());
        changes.push_back("milestone");
    }
    for (const auto& p : problems) {
        s.problems.push_back("coach: " + p);
        st.news.push_back("Your decision did not apply: " + p);
        ++st.urgent;
    }
    for (const auto& c : changes) s.notes.push_back("coach: " + c);
    if (changes.empty() && problems.empty() && !d.notes) return;
    std::string e = "## " + iso_timestamp() + " · Coach decisions (" + by + ")\n\n";
    for (const auto& c : changes) e += "- " + c + "\n";
    for (const auto& p : problems) e += "- Problem: " + p + "\n";
    if (!d.overrule.empty()) e += "- Overruled the Gatekeeper: " + first_line(d.overrule, 300) + "\n";
    if (!d.reasoning.empty()) e += "\n" + d.reasoning + "\n";
    append_changelog(project_.changelog_path(), e);
}

// --- 4. council -----------------------------------------------------------------------------

void Coach::council(CoachStep& s, CoachState& st, const CoachOptions& o, const CoachAsk& ask) {
    std::vector<std::pair<std::string, std::string>> views;
    std::vector<const Member*> who;
    bool gate = false;
    for (const auto& h : ask.to) {
        if (h == gatekeeper_->handle) {
            gate = true;
            continue;
        }
        const Member* m = team_.find(h);
        // A unit's name: its first expert (the coach asks a unit's view).
        if (!m)
            for (const Member& x : team_.members)
                if (!x.unit.empty() && normalize_handle(x.unit) == h && &x != coach_ && &x != gatekeeper_) {
                    m = &x;
                    break;
                }
        if (!m || m == coach_) {
            s.problems.push_back("council on " + ask.topic + ": no expert @" + h);
            continue;
        }
        if (std::find(who.begin(), who.end(), m) == who.end()) who.push_back(m);
    }
    auto view_of = [](const std::string& answer) {
        auto b = report_blocks(answer, "view");
        return std::string(trim(b.empty() ? answer : b.back().body));
    };
    for (const Member* m : who) {
        const Project& env = loop_.env_for(m->unit);
        std::string brief = env.brief();
        if (brief.size() > 4000) brief = brief.substr(0, 4000) + "\n[...]";
        std::string p = "# You\n\nYou are " + m->name + " (@" + m->handle + "), " + m->role +
                        (m->unit.empty() ? "" : ", working in " + m->unit + "/") + ".\n";
        if (!m->prompt.empty()) p += "\n" + m->prompt + "\n";
        const std::string mem = memory_section(project_, m->handle, project_.config().coach.memory_limit);
        if (!mem.empty()) p += "\n# Your memory\n\n" + mem + "\n";
        p += "\n# Your unit\n\n" + brief + "\n";
        p += "\n# The coach asks\n\n" + ask.question + "\n";
        p += "\n# How to answer\n\nYou have no tools here: answer from what you know of your unit's work. Give your\n"
             "own view, in at most 200 words: what you know, what you would do, what you are unsure of and how\n"
             "it could be checked. End with <view>your answer</view>.\n";
        std::string answer;
        if (talk(s, o, *m, "council:" + slug(ask.topic, 32), "Council on " + ask.topic, "You are an expert of an AI crew, asked for your view. You answer briefly and concretely.", p, answer))
            views.emplace_back(m->handle, view_of(answer));
    }
    if (gate) {
        std::string p = "# You\n\nYou are " + gatekeeper_->name + " (@" + gatekeeper_->handle + "), the Gatekeeper.\n";
        if (!gatekeeper_->prompt.empty()) p += "\n" + gatekeeper_->prompt + "\n";
        p += "\n# The coach asks\n\n" + ask.question + "\n";
        if (!views.empty()) {
            p += "\n# The experts' views\n\n";
            for (const auto& [h, v] : views) p += "@" + h + ":\n" + v + "\n\n";
        }
        p += "\n# How to answer\n\nBe the skeptic, in at most 200 words: what is missing, wrong, unproven or risky in the\n"
             "question and the views, and what evidence would settle it. End with <view>your answer</view>.\n";
        std::string answer;
        if (talk(s, o, *gatekeeper_, "council:" + slug(ask.topic, 32), "Council on " + ask.topic, kGateSystem, p, answer))
            views.emplace_back(gatekeeper_->handle, view_of(answer));
    }
    if (views.empty()) return;
    std::string file = "# Council: " + ask.topic + "\n\n" + iso_timestamp() + "\n\n## The question\n\n" + ask.question + "\n";
    std::string news = "Council on \"" + ask.topic + "\" (" + first_line(ask.question, 160) + "):";
    for (const auto& [h, v] : views) {
        file += "\n## @" + h + "\n\n" + v + "\n";
        std::string flat;
        for (const auto& l : split_lines(v))
            if (!trim(l).empty()) flat += (flat.empty() ? "" : " ") + std::string(trim(l));
        if (flat.size() > 1500) flat = flat.substr(0, 1500) + " [...]";
        news += "\n    @" + h + ": " + flat;
    }
    write_file_atomic(project_.state_dir() / "council" / (file_timestamp() + "-" + slug(ask.topic, 40) + ".md"), file);
    st.news.push_back(news);
    ++st.urgent;
    s.notes.push_back("council on " + ask.topic + ": " + std::to_string(views.size()) + " views");
}

// --- the step -------------------------------------------------------------------------------

CoachStep Coach::step(const CoachOptions& o) {
    CoachStep s;
    CoachState st = load_coach_state(project_);
    if (o.dry_run) {
        for (const DispatchPlan& dp : loop_.preview([&] {
                 LoopOptions lo = o.loop;
                 lo.max_dispatches = std::max(1, project_.config().coach.tasks_per_step);
                 lo.skip_members = {coach_->handle, gatekeeper_->handle};
                 return lo;
             }()))
            s.notes.push_back("would run " + dp.task_key + " by @" + dp.member->handle + " (" + dp.model.str() + ", ~" +
                              std::to_string(dp.context.estimated_tokens()) + " tokens)");
        s.notes.push_back(needs_coach(st) ? "the coach would decide (" + std::to_string(estimate_tokens(coach_prompt())) +
                                                " tokens: myteam digest)"
                                          : "the coach has no news");
        return s;
    }

    task_round(s, st, o);
    if (!s.rate_limited) acceptances(s, st, o);
    if (!s.rate_limited) {
        Plan plan = Plan::load(project_.plan_path());
        bool ready = false, review = false;
        for (const TaskRef& r : plan.tasks()) {
            if (r.task->status == TaskStatus::Review) review = true;
            if (!r.task->assignee || !is_open(r.task->status) || !plan.ready(r)) continue;
            const std::string h = normalize_handle(*r.task->assignee);
            if (h != coach_->handle && h != gatekeeper_->handle && team_.find(h)) ready = true;
        }
        const bool worked = !s.runs.empty();
        if (needs_coach(st)) decide(s, st, o, false);
        else if (!ready && !review && !worked && st.idle == 0) decide(s, st, o, true);
        if (!s.decided && !ready && !review && !worked) {
            s.waiting = true;
            std::vector<std::string> ops;
            for (const TaskRef& r : plan.tasks())
                if (r.task->assignee && normalize_handle(*r.task->assignee) == kOperator && !is_finished(r.task->status))
                    ops.push_back(r.key());
            s.waiting_for = ops.empty() ? "the crew is idle: tell the coach what you want (myteam say \"...\")"
                                        : "the crew waits for you: " + join(ops, ", ") + " (myteam done ID \"note\"), or myteam say";
        }
    }
    if (s.rate_limited) {
        s.waiting = true;
        s.waiting_for = "rate limited: try again later";
    }
    if (!s.decided && !st.news.empty()) {
        st.quiet++;
        s.notes.push_back("routine news waits for the coach (" + std::to_string(st.news.size()) + " items, step " +
                          std::to_string(st.quiet) + ")");
    }
    st.steps++;
    st.cost += s.cost;
    save_coach_state(project_, st);
    return s;
}

}  // namespace myteam
