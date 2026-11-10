// The project plan: a Markdown (or TaskML) file parsed by tasks/ (TaskML).
//
// Plan is a read-only snapshot. PlanEditor changes the source text line by
// line, so the prose and layout around tasks survive, and commits only when
// the result parses and validates no worse than before and nobody changed
// the file meanwhile.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <taskml/taskml.hpp>

namespace myteam {

/// A task in its place in the plan.
struct TaskRef {
    const taskml::Task* task = nullptr;
    std::vector<const taskml::Task*> parents;  ///< outermost first
    std::vector<std::string> sections;         ///< heading path, outermost first
    int depth = 0;

    /// "^id" when the task has an id, otherwise "L<line>".
    std::string key() const;
    const taskml::Task& operator*() const { return *task; }
    const taskml::Task* operator->() const { return task; }
};

bool is_open(taskml::TaskStatus s);  ///< pending or in progress
bool is_finished(taskml::TaskStatus s);  ///< completed or cancelled
/// "[ ] pending" etc. for prompts and logs.
std::string status_label(taskml::TaskStatus s);
/// The task a cancelled task was closed as a duplicate of, or merged into (its note
/// "Duplicate of ^x" or "Merged into ^x"): "x"; "" when none. Such a task was not dropped.
std::string duplicate_of(const taskml::Task& t);
class Plan;
/// Where a task closed as a duplicate leads: along the chain of duplicates to a task that is
/// not one; "" when `id` is no duplicate, or the chain runs in a circle or out of the plan.
std::string follow_duplicates(const Plan& plan, const std::string& id);
/// Tags a task made from `t` (a handoff, a blocker task) carries over:
/// its sprint (#sprint-N), so the work stays in the sprint.
std::vector<std::string> inherited_tags(const taskml::Task& t);
/// One line: "Build API (^api, @bob, in progress)".
std::string task_one_liner(const taskml::Task& t);
/// A title as the twin check compares it: lower case, every run of characters that are
/// not letters or digits one space, trimmed ("Fix: the .pyc  commit!" -> "fix the pyc commit").
std::string normalized_title(std::string_view title);

class Plan {
public:
    static Plan load(const std::filesystem::path& path);
    static Plan from_text(std::string text, taskml::Dialect dialect = taskml::Dialect::Markdown,
                          std::filesystem::path path = {});

    Plan(Plan&&) = default;
    Plan& operator=(Plan&&) = default;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    const std::filesystem::path& path() const { return path_; }
    const std::string& text() const { return text_; }
    taskml::Dialect dialect() const { return dialect_; }
    const taskml::Document& doc() const { return result_.document; }

    /// Parse errors plus semantic validation errors (duplicate ids, unknown or
    /// circular dependencies).
    const std::vector<taskml::Diagnostic>& errors() const { return errors_; }
    const std::vector<taskml::Diagnostic>& warnings() const { return result_.warnings; }
    bool ok() const { return errors_.empty(); }

    /// All tasks in document order.
    const std::vector<TaskRef>& tasks() const { return tasks_; }
    /// By "^id", "id", "L12" or "12" (a line number).
    const TaskRef* find(std::string_view key) const;
    const TaskRef* by_id(std::string_view id) const;
    const TaskRef* ref_of(const taskml::Task* t) const;

    /// Tasks that name `t` in depends: or blocked-by:.
    std::vector<const TaskRef*> dependents(const TaskRef& t) const;
    /// The depends: / blocked-by: entries of `t` that are not finished yet
    /// (unknown ids count as not finished; a task closed as a duplicate counts as its
    /// live twin, and as not finished when its chain runs in a circle).
    std::vector<std::string> unmet_dependencies(const TaskRef& t) const;
    std::vector<std::string> active_blockers(const TaskRef& t) const;
    bool ready(const TaskRef& t) const {
        return unmet_dependencies(t).empty() && active_blockers(t).empty();
    }

    std::string directive(std::string_view name, std::string fallback = {}) const;

    /// The first task that is not finished (completed or cancelled), has the same normalized
    /// title and the same assignee (both none counts as the same): the twin a new task would be.
    const TaskRef* open_twin(std::string_view title, std::string_view assignee,
                             const taskml::Task* except = nullptr) const;

private:
    Plan() = default;
    void index();

    std::filesystem::path path_;
    std::string text_;
    taskml::Dialect dialect_ = taskml::Dialect::Markdown;
    taskml::ParseResult result_;
    std::vector<taskml::Diagnostic> errors_;
    std::vector<TaskRef> tasks_;
};

/// A task to add to the plan.
struct NewTask {
    std::string description;
    taskml::TaskStatus status = taskml::TaskStatus::Pending;
    std::string assignee;  ///< member handle, without '@'
    std::optional<int> priority;
    std::string id;        ///< generated from the description when empty
    std::vector<std::string> depends_on;
    std::vector<std::string> notes;
    std::vector<std::string> criteria;  ///< acceptance criteria (pending)
    std::vector<std::string> tags;      ///< without '#'

    std::optional<std::string> estimate;  ///< "4h", without '~'
};

/// A task the twin check did not add, because an open task was the same job.
struct TwinMerge {
    std::string title;  ///< the new task's title
    std::string into;   ///< the id of the open task it went to
};

struct CommitResult {
    bool ok = false;
    std::string error;
    std::vector<taskml::Diagnostic> diagnostics;  ///< what made the commit fail
};

class PlanEditor {
public:
    explicit PlanEditor(const Plan& plan);

    void set_status(const TaskRef& t, taskml::TaskStatus status);
    void set_assignee(const TaskRef& t, std::string_view handle);
    /// Give a task without an id one (from its description); returns the id
    /// (the existing one when it has one).
    std::string ensure_id(const TaskRef& t);
    void add_note(const TaskRef& t, std::string_view text);
    /// Add `id` to the task's blocked-by: (creating the line when missing).
    void add_blocked_by(const TaskRef& t, std::string_view id);
    /// A depends: entry (its order: it waits until ^id is done or in review).
    void add_depends(const TaskRef& t, std::string_view id);
    bool remove_depends(const TaskRef& t, std::string_view id);
    /// Remove `id` from the task's blocked-by:. When none is left the line
    /// becomes the note "No longer blocked by ^id" (lines are never deleted).
    /// Returns false when the task was not blocked by it.
    bool remove_blocked_by(const TaskRef& t, std::string_view id);
    /// Set the status of the task's `index`th (0-based) acceptance criterion,
    /// and its evidence when given (replacing an existing evidence: line).
    void set_criterion(const TaskRef& t, std::size_t index, taskml::CriterionStatus status,
                       std::optional<std::string> evidence = std::nullopt);
    /// Add a pending acceptance criterion after the task's existing children.
    void add_criterion(const TaskRef& t, std::string_view text);
    /// The text of criterion `index` (its status stays).
    void reword_criterion(const TaskRef& t, std::size_t index, std::string_view text);
    /// Criterion `index` becomes a note saying what it was, and why it went (its evidence kept as a note).
    void remove_criterion(const TaskRef& t, std::size_t index, std::string_view why);
    /// Set the ~estimate ("3h"; replacing an existing one).
    void set_estimate(const TaskRef& t, std::string_view estimate);
    /// Add a #tag to the task line (no-op when it has it).
    void add_tag(const TaskRef& t, std::string_view tag);
    /// Set a front matter directive (`key: value`), creating the front
    /// matter when the plan has none.
    void set_directive(std::string_view key, std::string_view value);
    /// Set the #pN priority (replacing an existing one).
    void set_priority(const TaskRef& t, int priority);
    /// Add a subtask of `parent`, after its existing children. Returns the id.
    std::string add_subtask(const TaskRef& parent, NewTask task);
    /// Add a task right after `sibling` (and its children). Returns the id.
    std::string add_sibling(const TaskRef& sibling, NewTask task);
    /// Add a top-level task at the end of the section with this heading,
    /// creating the section (at `level`) at the end of the plan when missing.
    std::string add_to_section(std::string_view title, NewTask task, int level = 2);
    // The twin check (every add_* above, so every path that makes a task: planning, the
    // org planner, findings, blockers, escalations, handoffs, the steward): a new open task
    // whose normalized title and assignee are an open task's (in the plan, or added through
    // this editor) is not added; the open task gets a note, and its id is returned.
    /// The adds that went to an open twin.
    const std::vector<TwinMerge>& twin_merges() const { return twin_merges_; }

    /// The edited text.
    std::string text() const;
    bool changed() const { return changed_; }
    /// Validate and write the plan file. Nothing is written on failure.
    CommitResult commit();
    /// Validation only (no write).
    CommitResult check() const;

private:
    void add_link(const TaskRef& t, std::string_view key, std::string_view id);
    bool remove_link(const TaskRef& t, std::string_view key, std::string_view id, std::string_view gone);
    int current_index(int line) const;  // 1-based source line -> index into lines_
    int line_indent(int index) const;
    int subtree_end(int index) const;   // last index of the task's block
    std::string marker_of(int index) const;
    void insert(int at, const std::vector<std::string>& new_lines);
    std::string unique_id(std::string_view base);
    std::string free_id(const std::string& id);
    std::vector<std::string> render(const NewTask& t, int indent, const std::string& marker);
    std::string add_at(const TaskRef& anchor, NewTask task, bool as_child);
    std::optional<std::string> merge_into_twin(NewTask& task);

    const Plan& plan_;
    std::vector<std::string> lines_;
    std::vector<int> index_;  // source line (1-based) -> current index
    std::vector<std::string> new_ids_;
    std::map<int, int> after_;  // source line of an anchor -> lines already added after it
    std::map<std::string, std::string> added_keys_;  // normalized title + assignee of an added task -> its id
    std::vector<TwinMerge> twin_merges_;
    bool trailing_newline_ = true;
    bool changed_ = false;
};

/// The cleanup of twins already in a plan: for each group of open tasks with the same
/// normalized title and assignee, the first (in progress before pending, then plan order)
/// stays; each other one is cancelled with the note "Duplicate of ^kept", so what waits
/// for it waits for the kept one (follow_duplicates, unblock_resolved). Tasks without
/// an id are given one. Returns one entry per merge: {old id, kept id, title}.
struct TwinCleanup {
    std::string from;
    std::string into;
    std::string title;
};
std::vector<TwinCleanup> merge_open_twins(const Plan& plan, PlanEditor& ed);
/// "^old -> ^kept  title": one line per merge.
std::string twin_cleanup_line(const TwinCleanup& m);
/// A changelog entry listing the merges ("" when none).
std::string twin_cleanup_entry(const std::vector<TwinCleanup>& merges);

}  // namespace myteam
