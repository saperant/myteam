// One round of work (the `direct` methodology, and the task round of `coach`):
//
//   1. A plan that does not validate goes to the team's repairer first.
//   2. Ready tasks assigned to members are dispatched, each with its clean
//      context, in the member's unit (Member::unit: its repository, its
//      boundaries). Unassigned work is the coach's to assign (coach.hpp).
//   3. Each agent's report updates the plan through PlanEditor: status,
//      handoff tasks (with depends: on the original), a note when the run
//      failed; its lessons go into the member's memory (memory.hpp). With a
//      Gatekeeper, work reported done goes to review for it. The run is
//      logged to CHANGELOG.md and .myteam/runs/.
//   4. The plan is validated again, and repaired if needed.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/checkpoint.hpp"
#include "myteam/context.hpp"
#include "myteam/project.hpp"
#include "myteam/report.hpp"
#include "myteam/runner.hpp"
#include "myteam/team.hpp"

namespace myteam {

/// What is about to run, for confirmation and dry runs.
struct DispatchPlan {
    std::string task_key;
    std::string description;
    const Member* member = nullptr;
    ModelRef model;
    std::string reason;  ///< "assigned", or the product owner's allocation reason
    AgentContext context;
    taskml::TaskStatus previous_status = taskml::TaskStatus::Pending;  ///< before the run marked it [~]
    std::optional<std::string> resume_session;  ///< continue a run stopped at a limit
    std::optional<Permission> permission;        ///< instead of the project's (the Gatekeeper reads only)
    bool tools = false;                          ///< give the agent the MCP plan tools
    bool commits = false;                        ///< and git_commit (commits.hpp)
    /// The unit whose repository and boundaries the run has; empty: the member's own (the
    /// Gatekeeper checks a task in its assignee's unit).
    std::optional<std::string> unit;
    bool talk = false;           ///< one answer, no tools (RunRequest::talk)
    int max_turns = 0;           ///< instead of the project's limit (0: the project's)
    std::string system_prompt;   ///< talk: its system prompt
};

struct DispatchOutcome {
    DispatchPlan plan;
    bool executed = false;
    bool skipped = false;  ///< declined in interactive mode
    RunResult run;
    AgentReport report;
    std::vector<std::string> plan_changes;  ///< what was written to the plan
    std::vector<std::string> problems;      ///< what could not be applied
    std::vector<std::string> feedback;      ///< organisational feedback recorded (for myteam)
    std::vector<std::string> lessons;       ///< lessons added to the member's memory
    std::vector<std::string> guard;         ///< what the safeguards refused, reverted or flagged
    bool guard_hold = false;                ///< the run's changes were reverted or flagged: not done
    std::vector<RunCommit> commits;         ///< what the agent committed through git_commit
    std::filesystem::path run_dir;
    /// "none", "no-report", or a Failure name: what the run record keeps.
    std::string failure = "none";
    /// The runner cannot work now (missing, not logged in, rate limited):
    /// the rest of the round skips it.
    bool stop_runner = false;
};

struct IterationResult {
    std::vector<DispatchOutcome> dispatches;
    std::vector<std::string> waiting;       ///< assigned tasks waiting for others
    std::vector<std::string> unblocked;     ///< tasks whose blockers were all finished
    int repair_attempts = 0;
    bool needs_human = false;
    std::vector<std::string> notes;
};

struct LoopOptions {
    int max_dispatches = 0;      ///< 0: every ready task
    bool dry_run = false;        ///< build contexts, run nothing, change nothing
    std::string only_task;       ///< dispatch just this task (key) when set
    std::string only_tag;        ///< only tasks with this tag (a sprint: "sprint-3")
    std::string runner;          ///< use this runner for everyone (keeps the model when it matches)
    long today = 0;              ///< days since epoch; 0: now
    /// Members whose tasks the round does not run (the coach's, the Gatekeeper's: decisions).
    std::set<std::string> skip_members;
    /// How to start the MCP server (`myteam mcp`), e.g. {"/usr/bin/myteam", "mcp"};
    /// empty: agents get no tools, only the report blocks.
    std::vector<std::string> mcp_command;
    /// How to start the guard hook (`myteam guard`); empty: no hook.
    std::vector<std::string> guard_command;
    int max_repairs = 5;
    /// Consecutive failed or report-less runs of a task before it is blocked
    /// on the operator.
    int max_failures = 3;
    /// Interactive confirmation; false skips the dispatch.
    std::function<bool(const DispatchPlan&)> confirm;
    /// Live events of the running agent.
    std::function<void(const DispatchPlan&, const AgentEvent&)> on_event;
    /// Progress messages ("allocated ...", "plan repaired").
    std::function<void(const std::string&)> on_note;
};

using RunnerFactory = std::function<std::unique_ptr<AgentRunner>(const std::string& runner_id)>;

/// Runners as configured in the project (.myteam/config.yaml runners:).
RunnerFactory project_runners(const Project& project);

/// The ^ids a text names (a commit message), in order, each once.
std::vector<std::string> task_ids_named(std::string_view text);

/// A commit of the run names an open task other than the run's own (`own_id`):
/// a note on that task asks its assignee to close it or check its criteria
/// (the stand-up lists it: FlagKind::CommitNamesOpen). Once per commit and
/// task. Returns the plan changes, for the run record.
std::vector<std::string> note_tasks_commits_name(const Plan& plan, PlanEditor& ed, const std::vector<RunCommit>& commits,
                                                 std::string_view own_id, std::string_view author, std::string_view date);

class Loop {
public:
    Loop(const Project& project, const Team& team, RunnerFactory runners);

    /// A task round.
    IterationResult iterate(const LoopOptions& options);

    /// The dispatches the next round would make (no side effects).
    std::vector<DispatchPlan> preview(const LoopOptions& options);

    /// The project a member's runs have: its unit's (a myteam project in the unit's directory,
    /// whose config myorg writes: what the unit reads, writes and may not see), or this one.
    const Project& env_for(const std::string& unit) const;
    /// Run an agent outside a task round (the coach, the Gatekeeper, a council view): a new
    /// run directory, put in `run_dir`. Nothing is applied to the plan.
    RunResult run(const DispatchPlan& dp, const LoopOptions& o, std::filesystem::path& run_dir);
    /// Record such a run: run.json in its directory, and `changelog` in CHANGELOG.md.
    void record(const DispatchOutcome& out, const std::string& changelog);
    /// "3 turns, 1.2k in / 300 out tokens, $0.02"
    static std::string usage_text(const Usage& u);
    /// What the safeguards refuse, for a context (in `env`).
    static std::string protected_paths_text(const Project& env);

private:
    struct Selection {
        std::vector<std::string> keys;
        std::vector<std::string> waiting;
    };
    Selection select(const Plan& plan, const LoopOptions& o) const;
    DispatchPlan plan_dispatch(const Plan& plan, const TaskRef& t, const LoopOptions& o, std::string reason) const;
    DispatchOutcome execute(DispatchPlan dp, const LoopOptions& o);
    /// Runs the agent in a new run directory, put in `run_dir` (or in `run_dir` as given, with `keep_dir`).
    RunResult run_agent(const DispatchPlan& dp, const LoopOptions& o, std::filesystem::path& run_dir,
                        bool keep_dir = false);
    std::filesystem::path new_run_dir(const DispatchPlan& dp) const;
    void apply_report(DispatchOutcome& out, const LoopOptions& o);
    void record_org_issues(DispatchOutcome& out);
    bool repair(IterationResult& result, const LoopOptions& o);
    IterationResult task_round(const LoopOptions& o);
    /// The member's unit for this run (dp.unit, else the member's).
    std::string unit_of(const DispatchPlan& dp) const;
    /// Paths of this project a unit's agent may not read (the plan, the changelog, state).
    std::vector<std::string> hidden_from_units() const;
    /// The runner of `model` can give agents the MCP tools, and they are on.
    bool tools_for(const ModelRef& model, const LoopOptions& o) const;
    /// Agents commit through git_commit in `env` (a git project, commits: tool, checkpoints on).
    bool commits_for(bool tools, const Project& env) const;
    /// `wanted`, or `standard` when wanted's CLI cannot start here (said once).
    ModelRef usable(const ModelRef& wanted, const ModelRef& standard, const LoopOptions& o) const;
    mutable std::set<std::string> told_;  ///< runners whose fallback was said
    mutable std::map<std::string, bool> git_repo_;
    mutable std::map<std::string, std::unique_ptr<Project>> envs_;
    void note(const LoopOptions& o, IterationResult& r, std::string text) const;
    std::string changelog_entry(const DispatchOutcome& out) const;
    /// The task's run records, newest first.
    std::vector<taskml::Json> records(const std::string& task_key) const;
    int consecutive_failures(const std::string& task_key) const;
    std::optional<std::string> limit_session(const DispatchPlan& dp) const;
    void unblock(IterationResult& res, const LoopOptions& o, const std::map<std::string, std::string>& why = {});

    const Project& project_;
    const Team& team_;
    RunnerFactory runners_;
};

}  // namespace myteam
