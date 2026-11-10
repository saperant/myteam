// Blockers: every blocked task names, through blocked-by:, the task that
// stands for what is missing.
//
//   - [!] Upload results #p1 @developer ^upload
//     - blocked-by: ^s3-credentials
//     - Blocked 2026-11-06 (@developer) by ^s3-credentials: needs write access to the test bucket
//   - [ ] Provide write access to the S3 test bucket @operator ^s3-credentials
//     - Needed by ^upload (@developer)
//
// What blocks can be another task, work or a decision of a member, a
// question for the customer (a task for the product owner, tagged
// #customer-question), or something outside the team: a task for
// @operator, the person running myteam. When every blocker is finished the
// task is unblocked by code, with a note of what resolved it.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "myteam/plan.hpp"
#include "myteam/report.hpp"
#include "myteam/team.hpp"

namespace myteam {

/// The assignee of tasks only the person running myteam can do.
inline constexpr const char* kOperator = "operator";
/// The tag of a question for the customer, answered through the PO.
inline constexpr const char* kCustomerQuestion = "customer-question";

bool is_customer_question(const taskml::Task& t);
bool is_operator_task(const taskml::Task& t);

/// Open tasks assigned to a member (not to a role word or another team), with no open
/// subtask of a member of their own: those ready to run, or those waiting (dependencies,
/// blockers). Questions for the customer are left out (the coach answers them).
std::vector<const TaskRef*> ready_assigned_tasks(const Plan& plan, const Team& team);
std::vector<const TaskRef*> waiting_assigned_tasks(const Plan& plan, const Team& team);
/// Who a handoff (plan_handoff, <handoff to=>) may name: a member, one of the org's
/// other teams (a unit, which myorg carries the task to) or the operator. The
/// normalized handle, or "" when `to` is none of them.
std::string handoff_target(const Team& team, std::string_view to);
/// A handoff to `handle` waits for the sender's task (depends on it): true for a
/// member only. A unit's or the operator's task starts at once and the sender's task
/// does not wait for it either (a planning task's handoffs are not its blockers).
bool handoff_waits_for_sender(const Team& team, std::string_view handle);

/// The note that names a task's reviewer: "Review by @x (author @y) DATE".
inline constexpr const char* kReviewNote = "Review by @";
/// Why `reviewer` cannot review `author`'s task, or "" when they can: a teammate,
/// one of the org's other teams or the operator, and not the author. Review is not a
/// waiting state: a task in review needs someone named to review it.
std::string reviewer_problem(const Team& team, std::string_view author, std::string_view reviewer);
std::string review_note(std::string_view reviewer, std::string_view author, std::string_view date);
/// The reviewer the task's newest review note names; "" when it names none.
std::string task_reviewer(const taskml::Task& t);
/// The task was last blocked without naming a blocker task (its newest
/// "Blocked" note has no "by ^id"), so its old blocked-by: links do not
/// stand for what blocks it now.
bool blocked_without_link(const taskml::Task& t);
/// A cancelled task that was declined (a note "Declined ..." or "...: Declined ..."): what
/// waited for it does not go on as if it were done (myteam done --declined).
bool is_declined(const taskml::Task& t);

struct BlockerEdits {
    std::vector<std::string> changes;   ///< for the changelog
    std::vector<std::string> problems;  ///< for the steward / stand-up
    std::vector<std::string> new_tasks; ///< ids of the tasks created for blockers
};

/// Mark `blocked` as [!] and record each blocker in the plan: a blocked-by:
/// link (to an existing task, or to a new task for a member, the product
/// owner or the operator) and a note saying what blocks. A blocker for the
/// operator on the subject of an open @operator task links to that task (it
/// gets a "Needed by" note) instead of adding a second one. Without any
/// blocker, the summary becomes the note and a problem is reported.
BlockerEdits record_blockers(const Plan& plan, PlanEditor& ed, const TaskRef& blocked, const Member& who,
                             const Team& team, const std::vector<BlockerReport>& blockers,
                             const std::string& summary);

/// A finding (see AgentReport::findings): a task for @operator next to `from`,
/// tagged #finding, which does not block `from`. A finding on the subject of
/// an open @operator task (same_subject), or of a finding in `added` (the titles recorded
/// before in this edit; the new one is added to it), is not added again. Returns the new task's id, or "" when it was already there.
std::string record_finding(const Plan& plan, PlanEditor& ed, const TaskRef& from, const Member& who,
                           const std::string& text, std::vector<std::string>* added = nullptr,
                           const Team* team = nullptr);
/// The org's other team a finding is addressed to ("To @toolkit: ...", "@toolkit: ..."), when
/// `team` knows it as external; "" otherwise. `rest` gets the text without the address.
std::string finding_address(const std::string& text, const Team* team, std::string* rest = nullptr);

struct Unblocked {
    std::string key;
    std::string description;
    std::vector<std::string> resolved_by;  ///< blocker ids
};

/// Every [!] task whose blocked-by tasks are all finished goes back to [ ]
/// with a note "Unblocked <date>: ^x done[: resolution]". `resolutions` gives
/// the text for blockers resolved just now (a summary, an answer). A declined
/// blocker (is_declined) does not release it: its link goes, and the task stays
/// blocked on a decision (a "Blocked" note naming no task), for the steward.
/// No task keeps a link to a finished blocker (an open task gets a note "Blocker ^x done"),
/// and a link to a task closed as a duplicate (duplicate_of) moves to its live twin; a circle
/// of duplicates is no live copy and holds the task like a declined blocker. Each link taken
/// off is added to `dropped` as "^task: ^x how". The editor may change with nothing released:
/// commit when ed.changed().
std::vector<Unblocked> unblock_resolved(const Plan& plan, PlanEditor& ed,
                                        const std::map<std::string, std::string>& resolutions = {},
                                        std::vector<std::string>* dropped = nullptr);

/// The note an open task gets when a task it depends on closes: "Dependency ^x done (DATE)",
/// "... cancelled", "... declined" or "... merged into ^y", with the resolution when
/// `resolutions` has one. Once per dependency (a note with dependency_note_prefix is kept), and
/// not for a dependency that is also a blocker (its blocker note says it). unblock_resolved
/// calls it, so every path that closes a task tells its dependents. "^task: ^x how" per note.
std::string dependency_note_prefix(std::string_view id);
std::vector<std::string> note_closed_dependencies(const Plan& plan, PlanEditor& ed,
                                                  const std::map<std::string, std::string>& resolutions = {});

/// A changelog entry listing the blocker links unblock_resolved took off; "" when none.
std::string dropped_links_entry(const std::vector<std::string>& dropped);

struct BlockedItem {
    const TaskRef* task = nullptr;
    std::vector<const TaskRef*> open;   ///< blockers still open
    std::vector<std::string> missing;   ///< blocked-by ids not in the plan
    bool unlinked = false;              ///< [!] without a blocked-by for what blocks it now
    std::vector<std::string> reasons;   ///< its "Blocked ..." notes
    std::vector<const TaskRef*> chained;  ///< open blockers that are blocked themselves
    std::optional<std::string> since;   ///< date of the newest "Blocked" note
    int times_blocked = 0;              ///< "Blocked" notes (dated blockings, not blockers)
    std::vector<const TaskRef*> in_review;  ///< open blockers in review
    /// Every open blocker is in review: the task waits on a review nobody may be doing.
    bool waits_only_on_review() const { return !open.empty() && in_review.size() == open.size() && missing.empty(); }
};

/// A task in review: who reviews it, and the open tasks that wait for it (blocked by it
/// or depending on it). Customer questions ([?] waiting for the customer) are not reviews.
struct InReview {
    const TaskRef* task = nullptr;
    std::string reviewer;                  ///< "" when none is named
    std::vector<const TaskRef*> waiting;   ///< open tasks blocked by or depending on it
};

struct BlockerReview {
    std::vector<BlockedItem> blocked;
    std::vector<const TaskRef*> for_operator;        ///< open @operator tasks
    std::vector<const TaskRef*> customer_questions;  ///< open #customer-question tasks
    std::vector<InReview> in_review;                 ///< every [?] task but customer questions
};

BlockerReview review_blockers(const Plan& plan);

}  // namespace myteam
