#include "myteam/blockers.hpp"

#include <cctype>

#include <algorithm>
#include <set>

#include "myteam/util.hpp"

namespace myteam {

using taskml::Task;
using taskml::TaskStatus;

namespace {

bool assigned_to_member(const Task& t, const Team& team) { return t.assignee && team.find(*t.assignee); }

bool has_open_member_subtask(const Task& t, const Team& team) {
    for (const Task& s : t.subtasks)
        if ((is_open(s.status) && assigned_to_member(s, team)) || has_open_member_subtask(s, team)) return true;
    return false;
}

}  // namespace

bool is_customer_question(const Task& t) {
    return std::find(t.tags.begin(), t.tags.end(), kCustomerQuestion) != t.tags.end();
}

bool is_operator_task(const Task& t) { return t.assignee && normalize_handle(*t.assignee) == kOperator; }

std::string handoff_target(const Team& team, std::string_view to) {
    const std::string h = normalize_handle(to);
    if (h.empty()) return "";
    if (const Member* m = team.find(h)) return m->handle;
    if (h == kOperator || team.is_external(h)) return h;
    return "";
}

bool handoff_waits_for_sender(const Team& team, std::string_view handle) { return team.find(handle) != nullptr; }

std::string reviewer_problem(const Team& team, std::string_view author, std::string_view reviewer) {
    const std::string r = normalize_handle(reviewer);
    if (r.empty())
        return "a task in review names its reviewer (reviewer: a teammate's handle, another team's or operator); "
               "review is not a waiting state";
    if (r == normalize_handle(author)) return "the reviewer is not the task's author (@" + r + "): name someone else";
    if (handoff_target(team, r).empty()) return "no teammate, team or operator @" + r + " to review it";
    return "";
}

std::string review_note(std::string_view reviewer, std::string_view author, std::string_view date) {
    return std::string(kReviewNote) + normalize_handle(reviewer) +
           (author.empty() ? std::string() : " (author @" + normalize_handle(author) + ")") + " " + std::string(date);
}

std::string task_reviewer(const taskml::Task& t) {
    for (auto it = t.notes.rbegin(); it != t.notes.rend(); ++it) {
        if (!starts_with(*it, kReviewNote)) continue;
        std::string_view rest = std::string_view(*it).substr(std::string_view(kReviewNote).size());
        std::size_t e = 0;
        while (e < rest.size() && rest[e] != ' ' && rest[e] != ',' && rest[e] != ')' && rest[e] != ':') ++e;
        return normalize_handle(rest.substr(0, e));
    }
    return "";
}

namespace {

std::string today() { return iso_timestamp().substr(0, 10); }

// The lines after the first, for notes; past `max`, a note says how many more there were
// (rather than losing the last lines without a word).
std::vector<std::string> rest_lines(const std::string& text, std::size_t max) {
    std::vector<std::string> out;
    std::size_t more = 0;
    auto lines = split_lines(text);
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (trim(lines[i]).empty()) continue;
        if (out.size() < max) out.emplace_back(trim(lines[i]));
        else ++more;
    }
    if (more)
        out.push_back("(" + std::to_string(more) + " more line" + (more == 1 ? "" : "s") +
                      " in the sender's report, its run's result.md)");
    return out;
}

}  // namespace

bool blocked_without_link(const Task& t) {
    // The newest Blocked/Unblocked note decides: "Blocked <date> (@h): why"
    // names no blocker task; "Blocked <date> (@h) by ^x: ..." does.
    for (auto it = t.notes.rbegin(); it != t.notes.rend(); ++it) {
        if (starts_with(*it, "Unblocked")) return false;
        if (starts_with(*it, "Blocked ")) {
            auto close = it->find(')');
            return close == std::string::npos || it->compare(close + 1, 5, " by ^") != 0;
        }
    }
    return false;
}

bool is_declined(const Task& t) {
    if (t.status != TaskStatus::Cancelled) return false;
    for (const auto& n : t.notes)
        if (starts_with(n, "Declined") || n.find(": Declined") != std::string::npos) return true;
    return false;
}

BlockerEdits record_blockers(const Plan& plan, PlanEditor& ed, const TaskRef& blocked, const Member& who,
                             const Team& team, const std::vector<BlockerReport>& blockers,
                             const std::string& summary) {
    BlockerEdits out;
    ed.set_status(blocked, TaskStatus::Blocked);
    out.changes.push_back("status → " + status_label(TaskStatus::Blocked));
    const std::string self = blocked.task->id.value_or("");
    // Whose work waits: the task's assignee (the steward may be the one linking).
    const std::string owner = blocked.task->assignee ? normalize_handle(*blocked.task->assignee) : who.handle;
    const std::string needed_by = "Needed by " + (self.empty() ? blocked.key() : "^" + self) + " (@" + owner + ")";
    const std::string stamp = "Blocked " + today() + " (@" + who.handle + ")";

    if (blockers.empty()) {
        std::string why = first_line(summary, 200);
        ed.add_note(blocked, stamp + ": " + (why.empty() ? "no reason given" : why));
        out.problems.push_back("blocked without a <blocker>: nothing links what blocks it; the stand-up reviews it");
        return out;
    }

    for (const BlockerReport& b : blockers) {
        const std::string what = first_line(b.text, 1000);
        BlockerReport::Kind kind = b.kind;
        std::string target = b.on;
        bool unknown_task = false;  // a blocker on an id no task has
        if (kind == BlockerReport::Kind::Task) {
            const TaskRef* t = plan.by_id(b.on);
            // Closed as a duplicate: wait for the copy that carries the work.
            if (t && !duplicate_of(*t->task).empty())
                if (const std::string twin = follow_duplicates(plan, b.on); !twin.empty()) {
                    out.changes.push_back("^" + b.on + " was merged into ^" + twin + ": blocked by ^" + twin);
                    target = twin;
                    t = plan.by_id(twin);
                }
            if (t && t->task != blocked.task) {
                ed.add_blocked_by(blocked, target);
                ed.add_note(blocked, stamp + " by ^" + target + (what.empty() ? "" : ": " + what));
                out.changes.push_back("blocked by ^" + target);
                // The blocker waited for this task (a handoff from it does): now this
                // one waits for it, so that wait goes, or neither could ever start.
                const std::string self = blocked.task->id.value_or("");
                const auto& deps = t->task->depends_on;
                if (!self.empty() && std::find(deps.begin(), deps.end(), self) != deps.end() &&
                    ed.remove_depends(*t, self)) {
                    ed.add_note(*t, "No longer waits for ^" + self + " (" + iso_timestamp().substr(0, 10) + "): ^" + self +
                                        " is blocked until this is done");
                    out.changes.push_back("^" + target + " no longer waits for ^" + self);
                }
                continue;
            }
            // No such task: the work it names is the team's, so it is filed under that id for the
            // team, not turned into an operator task.
            out.problems.push_back("blocker ^" + b.on + " is not a task in the plan; filed it for the team");
            unknown_task = true;
        }
        const Member* member = kind == BlockerReport::Kind::Member ? team.find(b.to) : nullptr;
        // Another team of the org: a task for them, which myorg carries over.
        const bool external = kind == BlockerReport::Kind::Member && !member && team.is_external(b.to);
        if (kind == BlockerReport::Kind::Member && !member && !external) {
            out.problems.push_back("blocker for unknown member @" + b.to + "; asked the operator instead");
            kind = BlockerReport::Kind::Operator;
        }
        // A question for the product owner is a product question: the PO's desk
        // answers it, from what it knows of the product, or asks the customer.
        // (With a coach, a blocker to it is a decision for it, not a question for the customer.)
        if (member && member->handle == team.product_owner_member().handle && team.members.size() > 1 &&
            team.coach.empty()) {
            kind = BlockerReport::Kind::Customer;
            member = nullptr;
        }

        // The operator has this open already (another task asked for it, or an
        // earlier run): wait for that task, not a second copy of it.
        if (kind == BlockerReport::Kind::Operator) {
            const TaskRef* open = nullptr;
            for (const TaskRef& r : plan.tasks())
                if (is_operator_task(*r.task) && !is_finished(r.task->status) && r.task->id &&
                    r.task != blocked.task && same_subject(r.task->description, what)) {
                    open = &r;
                    break;
                }
            if (open) {
                const std::string& oid = *open->task->id;
                ed.add_blocked_by(blocked, oid);
                ed.add_note(blocked, stamp + " by ^" + oid + ": " + what);
                ed.add_note(*open, needed_by);
                out.changes.push_back("blocked by ^" + oid + " (@" + std::string(kOperator) + ", already open)");
                continue;
            }
        }

        NewTask nt;
        nt.priority = blocked.task->priority;
        nt.tags = inherited_tags(*blocked.task);
        auto [title, more] = title_and_rest(what);
        if (!more.empty()) nt.notes.push_back(more);
        for (auto& l : rest_lines(b.text, 40)) nt.notes.push_back(std::move(l));
        std::string head = title.empty() ? "Unblock " + blocked.task->description : title;
        if (kind == BlockerReport::Kind::Task && !b.on.empty() && !unknown_task) head = "Provide ^" + b.on + ": " + head;
        if (unknown_task) {
            nt.id = b.on;
            nt.notes.push_back("Named as a blocker by @" + who.handle + " (no task had this id): the team's, for its "
                               "planning or the steward to give an owner");
        }
        switch (kind) {
            case BlockerReport::Kind::Member:
                nt.description = head;
                nt.assignee = external ? normalize_handle(b.to) : member->handle;
                break;
            case BlockerReport::Kind::Customer: {
                const Member& po = team.product_owner_member();
                nt.description = "Ask the customer: " + head;
                nt.assignee = po.handle;
                nt.tags.insert(nt.tags.begin(), kCustomerQuestion);
                break;
            }
            default:
                nt.description = head;
                if (!unknown_task) nt.assignee = kOperator;
                break;
        }
        nt.notes.push_back(needed_by);
        std::string id = ed.add_sibling(blocked, nt);
        // The twin check found the blocked task itself (it is the same job): nothing to wait for.
        if (blocked.task->id && id == *blocked.task->id) {
            out.problems.push_back("blocker \"" + first_line(head, 80) + "\" is ^" + id + " itself; not recorded");
            continue;
        }
        ed.add_blocked_by(blocked, id);
        ed.add_note(blocked, stamp + " by ^" + id + ": " + head);
        out.new_tasks.push_back(id);
        out.changes.push_back("blocked by ^" + id + " (@" + nt.assignee + ")");
    }
    return out;
}

namespace {
/// A handle a finding may name: another team of the org, a member, or a unit (its first member).
std::string finding_target(const Team& team, const std::string& word) {
    if (word.empty()) return "";
    if (team.is_external(word) || team.find(word)) return word;
    for (const Member& m : team.members)
        if (!m.unit.empty() && normalize_handle(m.unit) == word && m.handle != team.coach && m.handle != team.gatekeeper)
            return m.handle;
    return "";
}
}  // namespace

std::string finding_address(const std::string& text, const Team* team, std::string* rest) {
    if (!team) return "";
    std::string t(trim(text));
    std::size_t at = 0;
    if (starts_with(lower(t), "to ")) at = 3;
    if (t.compare(at, 2, "\\@") == 0) at += 2;
    else if (t.compare(at, 1, "@") == 0) at += 1;
    else if (at == 0) {
        // A finding that starts with a team's name ("linter has no ...", "toolkit's solver ...")
        // is about that team's tool: theirs, not the operator's.
        std::size_t e = 0;
        while (e < t.size() && (std::isalnum(static_cast<unsigned char>(t[e])) || t[e] == '-' || t[e] == '_')) ++e;
        const std::string first = finding_target(*team, normalize_handle(lower(t.substr(0, e))));
        if (!first.empty()) {
            if (rest) *rest = t;  // the whole text is the finding
            return first;
        }
        return "";
    } else return "";
    std::size_t end = at;
    while (end < t.size() && (std::isalnum(static_cast<unsigned char>(t[end])) || t[end] == '-' || t[end] == '_')) ++end;
    const std::string handle = finding_target(*team, normalize_handle(t.substr(at, end - at)));
    if (handle.empty()) return "";
    if (rest) {
        std::size_t r = end;
        while (r < t.size() && (t[r] == ':' || t[r] == ',' || t[r] == ' ')) ++r;
        *rest = t.substr(r);
    }
    return handle;
}

std::string record_finding(const Plan& plan, PlanEditor& ed, const TaskRef& from, const Member& who,
                           const std::string& text_in, std::vector<std::string>* added, const Team* team) {
    // Addressed to one of the org's other teams (a tool's support team): a task for them.
    std::string text = text_in;
    const std::string to = finding_address(text_in, team, &text);
    auto [title, more] = title_and_rest(first_line(text, 1000));
    if (title.empty()) return "";
    if (added)  // the same gap twice in one report
        for (const auto& a : *added)
            if (a == title || same_subject(a, title)) return "";
    for (const TaskRef& r : plan.tasks()) {
        const bool theirs = to.empty() ? is_operator_task(*r.task)
                                       : r.task->assignee && normalize_handle(*r.task->assignee) == to;
        if (theirs && !is_finished(r.task->status) &&
            (r.task->description == title || same_subject(r.task->description, title)))
            return "";
    }
    NewTask nt;
    nt.description = title;
    nt.assignee = to.empty() ? std::string(kOperator) : to;
    nt.priority = from.task->priority;
    nt.tags = inherited_tags(*from.task);
    nt.tags.push_back("finding");
    if (!more.empty()) nt.notes.push_back(more);
    for (auto& l : rest_lines(text, 40)) nt.notes.push_back(std::move(l));
    const std::string self = from.task->id ? "^" + *from.task->id : from.key();
    nt.notes.push_back("Found by @" + who.handle + " on " + self + ", which goes on without it");
    if (added) added->push_back(title);
    return ed.add_sibling(from, nt);
}

std::vector<Unblocked> unblock_resolved(const Plan& plan, PlanEditor& ed,
                                        const std::map<std::string, std::string>& resolutions,
                                        std::vector<std::string>* dropped) {
    std::vector<Unblocked> out;
    // No task keeps a blocker link to a finished task: a dead link reads as live, and makes a
    // task "blocked again". A link to a task
    // closed as a duplicate goes to its live twin instead: the work is not done.
    const std::string date = today();
    auto resolution = [&](const std::string& id) {
        auto it = resolutions.find(id);
        return it != resolutions.end() && !it->second.empty() ? " (" + first_line(it->second, 200) + ")" : std::string();
    };
    note_closed_dependencies(plan, ed, resolutions);
    for (const TaskRef& r : plan.tasks()) {
        const Task& t = *r.task;
        if (t.blocked_by.empty()) continue;
        const bool finished_task = is_finished(t.status);
        const std::string self = t.id.value_or("");
        auto log = [&](const std::string& s) {
            if (dropped) dropped->push_back(r.key() + ": " + s);
        };
        std::vector<std::string> live;                            // still open (or not in the plan)
        std::vector<std::pair<std::string, std::string>> closed;  // finished: id, how
        std::vector<std::string> declined;                        // "^id declined" etc.
        std::vector<std::string> declined_ids;
        for (const std::string& id : t.blocked_by) {
            const TaskRef* b = plan.by_id(id);
            if (!b || !is_finished(b->task->status)) {
                if (std::find(live.begin(), live.end(), id) == live.end()) live.push_back(id);
                continue;
            }
            const auto state = [](const Task& x) { return x.status == TaskStatus::Cancelled ? "cancelled" : "done"; };
            if (is_declined(*b->task)) {
                declined.push_back("^" + id + resolution(id) + " declined");
                declined_ids.push_back(id);
                continue;
            }
            if (duplicate_of(*b->task).empty()) {
                closed.emplace_back(id, state(*b->task));
                continue;
            }
            const std::string twin = follow_duplicates(plan, id);
            const TaskRef* tw = twin.empty() ? nullptr : plan.by_id(twin);
            if (!tw) {
                declined.push_back("^" + id + " was closed as a duplicate in a circle of copies, no live copy");
                declined_ids.push_back(id);
            } else if (twin == self) {
                closed.emplace_back(id, "merged into this task");
            } else if (!is_finished(tw->task->status)) {
                if (finished_task) {
                    closed.emplace_back(id, "merged into ^" + twin);
                    continue;
                }
                ed.remove_blocked_by(r, id);
                const bool had = std::find(t.blocked_by.begin(), t.blocked_by.end(), twin) != t.blocked_by.end() ||
                                 std::find(live.begin(), live.end(), twin) != live.end();
                if (!had) {
                    ed.add_blocked_by(r, twin);
                    live.push_back(twin);
                }
                ed.add_note(r, "Blocker ^" + id + " merged into ^" + twin + " (" + date + "): waits for ^" + twin + " now");
                log("^" + id + " merged into ^" + twin + ", now blocked by ^" + twin);
            } else if (is_declined(*tw->task)) {
                declined.push_back("^" + id + " merged into ^" + twin + ", declined");
                declined_ids.push_back(id);
            } else {
                closed.emplace_back(id, "merged into ^" + twin + " (" + state(*tw->task) + ")");
            }
        }
        // A finished task drops every link to a finished task, with no note.
        if (finished_task) {
            for (const auto& [id, how] : closed) ed.remove_blocked_by(r, id), log("^" + id + " " + how);
            for (const auto& id : declined_ids) ed.remove_blocked_by(r, id), log("^" + id + " declined");
            continue;
        }
        const bool releases = t.status == TaskStatus::Blocked && live.empty() && declined.empty() &&
                              !blocked_without_link(t) && !closed.empty();
        for (const auto& [id, how] : closed) {
            ed.remove_blocked_by(r, id);
            log("^" + id + " " + how);
            // Releasing: the Unblocked note says it. Otherwise a note keeps how it was resolved.
            if (!releases) ed.add_note(r, "Blocker ^" + id + " " + how + " (" + date + ")" + resolution(id) + ": link dropped");
        }
        if (t.status != TaskStatus::Blocked) {
            for (const auto& id : declined_ids) {
                ed.remove_blocked_by(r, id);
                log("^" + id + " declined");
                ed.add_note(r, "Blocker ^" + id + " declined (" + date + "): link dropped");
            }
            continue;
        }
        if (!live.empty()) continue;
        // Blocked again, on something not linked yet: the steward reviews it.
        if (blocked_without_link(t)) continue;
        // What it waited for was declined, not done (a declined "fetch the vendor PDF" must
        // not release the check against that PDF), or has no live copy: it waits for a decision.
        if (!declined.empty()) {
            for (const auto& id : declined_ids) {
                ed.remove_blocked_by(r, id);
                log("^" + id + " declined or without a live copy; blocked on a decision");
            }
            ed.add_note(r, "Blocked " + date + " (@steward): " + join(declined, ", ") + ", not done: decide whether this task " +
                               "still applies, and cancel it or change it to go on without it");
            continue;
        }
        if (!releases) continue;
        Unblocked u;
        u.key = r.key();
        u.description = t.description;
        std::string note = "Unblocked " + date + ":";
        for (const auto& [id, how] : closed) {
            u.resolved_by.push_back(id);
            note += " ^" + id + " " + how + resolution(id) + ",";
        }
        note.pop_back();
        ed.set_status(r, TaskStatus::Pending);
        ed.add_note(r, note);
        out.push_back(std::move(u));
    }
    return out;
}

std::string dependency_note_prefix(std::string_view id) { return "Dependency ^" + std::string(id) + " "; }

std::vector<std::string> note_closed_dependencies(const Plan& plan, PlanEditor& ed,
                                                  const std::map<std::string, std::string>& resolutions) {
    // Dependents hear when a gating task closes. A blocked-by
    // link gets its note when the link is dropped (unblock_resolved); a depends: link stays (it is
    // the order of the work), so it gets one note, once.
    std::vector<std::string> out;
    const std::string date = today();
    for (const TaskRef& r : plan.tasks()) {
        const Task& t = *r.task;
        if (is_finished(t.status) || t.depends_on.empty()) continue;
        std::set<std::string> done_here;
        for (const std::string& id : t.depends_on) {
            if (!done_here.insert(id).second) continue;
            // Also a blocker: its blocker note says it.
            if (std::find(t.blocked_by.begin(), t.blocked_by.end(), id) != t.blocked_by.end()) continue;
            const TaskRef* d = plan.by_id(id);
            if (!d || !is_finished(d->task->status)) continue;
            const std::string prefix = dependency_note_prefix(id);
            // Told already: by this note, or by a blocker note when the link was dropped.
            const std::string as_blocker = "Blocker ^" + id + " ", in_list = "^" + id + " ";
            if (std::any_of(t.notes.begin(), t.notes.end(), [&](const std::string& n) {
                    return starts_with(n, prefix) || starts_with(n, as_blocker) ||
                           (starts_with(n, "Unblocked ") && n.find(in_list) != std::string::npos);
                }))
                continue;
            std::string how = d->task->status == TaskStatus::Cancelled ? "cancelled" : "done";
            if (is_declined(*d->task)) how = "declined";
            if (const std::string dup = duplicate_of(*d->task); !dup.empty()) how = "merged into ^" + dup;
            std::string why;
            if (auto it = resolutions.find(id); it != resolutions.end() && !it->second.empty())
                why = ": " + first_line(it->second, 200);
            ed.add_note(r, prefix + how + " (" + date + ")" + why +
                               (how == "done" ? "" : "; check that this task still applies as written"));
            out.push_back(r.key() + ": ^" + id + " " + how);
        }
    }
    return out;
}

std::string dropped_links_entry(const std::vector<std::string>& dropped) {
    if (dropped.empty()) return "";
    std::string e = "## " + iso_timestamp() + " · Dead blocker links dropped\n\n";
    for (const auto& d : dropped) e += "- " + d + "\n";
    return e;
}

BlockerReview review_blockers(const Plan& plan) {
    BlockerReview rv;
    for (const TaskRef& r : plan.tasks()) {
        const Task& t = *r.task;
        if (!is_finished(t.status)) {
            if (is_operator_task(t)) rv.for_operator.push_back(&r);
            if (is_customer_question(t)) rv.customer_questions.push_back(&r);
        }
        if (t.status != TaskStatus::Blocked) continue;
        BlockedItem item;
        item.task = &r;
        item.unlinked = t.blocked_by.empty() || blocked_without_link(t);
        for (const std::string& id : t.blocked_by) {
            const TaskRef* b = plan.by_id(id);
            if (!b) item.missing.push_back(id);
            else if (!is_finished(b->task->status)) {
                item.open.push_back(b);
                if (b->task->status == TaskStatus::Blocked) item.chained.push_back(b);
                if (b->task->status == TaskStatus::Review && !is_customer_question(*b->task)) item.in_review.push_back(b);
            }
        }
        // A blocking starts with the first "Blocked" note after an
        // "Unblocked" one; more Blocked notes in between only add blockers.
        bool in_block = false;
        for (const std::string& n : t.notes) {
            if (starts_with(n, "Unblocked")) in_block = false;
            if (!starts_with(n, "Blocked ")) continue;
            item.reasons.push_back(n);
            if (in_block) continue;
            in_block = true;
            ++item.times_blocked;
            if (std::string date = n.substr(8, 10); parse_date_days(date)) item.since = date;
        }
        rv.blocked.push_back(std::move(item));
    }
    for (const TaskRef& r : plan.tasks()) {
        if (r.task->status != TaskStatus::Review || is_customer_question(*r.task)) continue;
        InReview ir{&r, task_reviewer(*r.task), {}};
        if (r.task->id)
            for (const TaskRef& w : plan.tasks()) {
                if (w.task == r.task || is_finished(w.task->status) || w.task->status == TaskStatus::Review) continue;
                const auto& bb = w.task->blocked_by;
                const auto& dd = w.task->depends_on;
                if (std::find(bb.begin(), bb.end(), *r.task->id) != bb.end() ||
                    std::find(dd.begin(), dd.end(), *r.task->id) != dd.end())
                    ir.waiting.push_back(&w);
            }
        rv.in_review.push_back(std::move(ir));
    }
    return rv;
}

namespace {
std::vector<const TaskRef*> assigned(const Plan& plan, const Team& team, bool want_ready) {
    std::vector<const TaskRef*> out;
    for (const TaskRef& r : plan.tasks()) {
        const Task& t = *r.task;
        if (!is_open(t.status) || !assigned_to_member(t, team) || has_open_member_subtask(t, team)) continue;
        if (is_customer_question(t)) continue;
        if (plan.ready(r) == want_ready) out.push_back(&r);
    }
    return out;
}
}  // namespace

std::vector<const TaskRef*> ready_assigned_tasks(const Plan& plan, const Team& team) { return assigned(plan, team, true); }
std::vector<const TaskRef*> waiting_assigned_tasks(const Plan& plan, const Team& team) { return assigned(plan, team, false); }

}  // namespace myteam
