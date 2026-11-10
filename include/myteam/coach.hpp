// methodology: coach. A flat crew instead of Scrum's ceremonies:
//
//   the coach       decides and assigns: what the crew works on, who does it, what counts
//                   as done. The customer's only contact. It reads a digest code builds
//                   (never the plan file) and answers with decision blocks. The most
//                   capable model; no tools: one answer per decision.
//   the Gatekeeper  the coach's skeptical right arm, on a different model. It reviews the
//                   coach's plan decisions before they apply (it may object once; the coach
//                   revises or overrules, on record), and it checks every piece of work an
//                   expert reports done, in the expert's unit, before it counts.
//   the experts     persistent people (a role, a memory: memory.hpp), each in its unit's
//                   repository behind its boundaries (Member::unit). They do the tasks.
//
// One step (`myteam run`):
//
//   1. tasks       ready expert tasks run (Loop: the task round), at most tasks_per_step
//   2. acceptance  the Gatekeeper checks work in review: accepted, or sent back with what
//                  is missing (after max_rejections: the coach decides)
//   3. decisions   when there is news (outcomes, verdicts, council views, a customer
//                  message, a task waiting for the coach, an objection) or the crew is
//                  idle: the coach decides; the Gatekeeper reviews plan decisions; code
//                  applies them (tasks, changes, answers, coaching, skills, notes, a
//                  message to the customer, the goal, a milestone)
//   4. council     the coach's questions (<ask>) go to the experts it names, each a
//                  single answer from its own role and memory; the Gatekeeper last
//
// Lessons the experts report go into their memory; the coach turns the ones worth
// keeping into skills (.myteam/skills, committed in git at once) or into tasks for the
// tool teams: skills are the tools' backlog.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/loop.hpp"
#include "myteam/project.hpp"
#include "myteam/skills.hpp"
#include "myteam/team.hpp"

namespace myteam {

// --- the coach's decisions ----------------------------------------------------------------

struct CoachTask {
    std::string id, to, title;
    std::optional<int> priority;
    std::vector<std::string> depends, notes, criteria, tags;
};
struct CoachChange {
    std::string id;
    std::optional<taskml::TaskStatus> status;
    std::string to;
    std::optional<int> priority;
    std::optional<std::vector<std::string>> depends;
    std::string note;
};
struct CoachAnswer {
    std::string id, text;
};
struct CoachAsk {
    std::string topic;
    std::vector<std::string> to;
    std::string question;
};
struct CoachCoaching {
    std::string to, text;
};

struct CoachDecisions {
    std::string reasoning;  ///< the text before the first block
    std::vector<CoachTask> tasks;
    std::vector<CoachChange> changes;
    std::vector<CoachAnswer> answers;
    std::vector<CoachAsk> asks;
    std::vector<CoachCoaching> coaching;
    std::vector<Skill> skills;
    std::optional<std::string> notes;  ///< the coach's own notes (replaced)
    std::string to_customer, goal, milestone, overrule;

    /// Decisions the Gatekeeper reviews: they change the plan, the crew's memory or skills,
    /// the goal, or claim a milestone.
    bool needs_review() const;
    bool empty() const;
};

CoachDecisions parse_decisions(std::string_view text);
/// The decisions as blocks again (what the Gatekeeper reads; what is held on an objection).
std::string render_decisions(const CoachDecisions& d);

/// The Gatekeeper's answer to decisions: <approve> or <object>.
struct GateReview {
    bool approve = true;
    std::string text;
};
GateReview parse_gate_review(std::string_view text);

/// The Gatekeeper's verdict on a piece of work: <verdict status="accept|reject">.
struct GateVerdict {
    std::optional<bool> accept;  ///< nullopt: no verdict
    std::string text;
};
GateVerdict parse_verdict(std::string_view text);

// --- the customer -------------------------------------------------------------------------

/// .myteam/customer/conversation.md: the customer and the coach, in order.
struct CustomerMessage {
    std::string who;  ///< "customer" or "coach"
    std::string when;
    std::string text;
};
std::filesystem::path conversation_path(const Project& project);
std::vector<CustomerMessage> load_conversation(const Project& project);
void append_conversation(const Project& project, const std::string& who, const std::string& name, const std::string& text);

// --- the step -----------------------------------------------------------------------------

/// .myteam/coach/state.json
struct CoachState {
    std::vector<std::string> news;  ///< what happened since the coach's last decisions
    int customer_seen = 0;          ///< messages of the conversation the coach has seen
    std::vector<std::string> seen_tasks;  ///< tasks waiting for the coach it has seen
    std::string objection;  ///< the Gatekeeper's objection to the held decisions
    std::string held;       ///< the decisions it objected to (blocks)
    int idle = 0;           ///< coach turns in a row with nothing to do
    /// News that changes the picture (a verdict, a block, a failed run, a council's views, a
    /// decision that did not apply): the coach decides at once. Routine news (progress, an
    /// unblocked task) waits for it, or for `quiet` steps.
    int urgent = 0;
    int quiet = 0;          ///< steps routine news has waited
    int steps = 0;
    int decisions = 0;
    int milestones = 0;
    double cost = 0;        ///< USD, all steps
    std::string last_decision;  ///< when
};
CoachState load_coach_state(const Project& project);
void save_coach_state(const Project& project, const CoachState& s);

struct CoachStep {
    std::vector<DispatchOutcome> runs;  ///< every agent run of the step, in order
    std::vector<std::string> notes;     ///< what happened, one line each
    std::vector<std::string> problems;
    bool decided = false;      ///< the coach decided
    bool milestone = false;    ///< the coach declared the goal met, and the Gatekeeper agreed
    bool waiting = false;      ///< nothing can go on without a person (or the rate limit)
    std::string waiting_for;
    bool rate_limited = false;
    bool needs_human = false;
    double cost = 0;
};

struct CoachOptions {
    LoopOptions loop;  ///< for the task round, and the runs' options
    bool dry_run = false;
};

class Coach {
public:
    Coach(const Project& project, const Team& team, RunnerFactory runners);

    CoachStep step(const CoachOptions& o);

    /// What the coach would read now (`myteam digest`).
    std::string digest() const;
    /// The coach's prompt now: the digest and how to decide.
    std::string coach_prompt() const;

    const Member& coach() const { return *coach_; }
    const Member& gatekeeper() const { return *gatekeeper_; }

private:
    std::string digest_for(const CoachState& st, bool for_gatekeeper) const;
    std::string prompt_for(const CoachState& st) const;
    void task_round(CoachStep& s, CoachState& st, const CoachOptions& o);
    void acceptances(CoachStep& s, CoachState& st, const CoachOptions& o);
    bool accept_one(CoachStep& s, CoachState& st, const CoachOptions& o, const std::string& id);
    void decide(CoachStep& s, CoachState& st, const CoachOptions& o, bool idle);
    void apply(CoachStep& s, CoachState& st, const CoachDecisions& d);
    void council(CoachStep& s, CoachState& st, const CoachOptions& o, const CoachAsk& ask);
    /// A talk run (one answer) of `who`; the outcome is in s.runs. False when it failed.
    bool talk(CoachStep& s, const CoachOptions& o, const Member& who, const std::string& key, const std::string& what,
              const std::string& system, const std::string& prompt, std::string& answer);
    bool needs_coach(const CoachState& st) const;
    std::vector<std::string> waiting_for_coach() const;
    void commit_skill(CoachStep& s, const Skill& sk, const std::filesystem::path& file);

    const Project& project_;
    const Team& team_;
    RunnerFactory runners_;
    Loop loop_;
    const Member* coach_ = nullptr;
    const Member* gatekeeper_ = nullptr;
};

}  // namespace myteam
