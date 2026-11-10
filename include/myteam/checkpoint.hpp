// Layer 2 of the safeguards: what a run changed.
//
// Before each task run myteam snapshots the work tree into a git commit
// under refs/myteam/checkpoints/ (with its own index file, so the user's
// index, branch and HEAD are untouched; .myteam/runs/ is left out). A
// project without git gets a private repository, .myteam/checkpoints.git. After
// the run it snapshots again and reviews the difference:
//
//   reverted (the run's change is undone, the task cannot be done):
//     - protected paths changed (the plan, .myteam/, .git/, CI, secrets)
//     - test files deleted
//     - secrets added (private keys, cloud and API tokens)
//   flagged (for the steward; the task cannot be done):
//     - tests skipped or disabled in the added lines
//   flagged (for the steward):
//     - more than guard.max_files files or guard.max_lines lines changed
//     - binary files added
//
// `myteam undo RUN` puts back what a run changed (the files nobody changed
// since), and the task goes back to [ ].
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "myteam/project.hpp"

namespace myteam {

struct Snapshot {
    std::string commit;
    std::string tree;
    std::string git_dir;  ///< empty: the project's repository; else myteam's private one
};

bool is_git_repo(const Project& project);
/// Where checkpoints go: "" for the project's own repository, else
/// .myteam/checkpoints.git (created, with the project as its work tree and
/// .myteam/, virtualenvs and build output left out).
std::string checkpoint_repo(const Project& project);
std::optional<Snapshot> take_snapshot(const Project& project, const std::string& message, std::string* error = nullptr);

struct FileChange {
    char status = 'M';      ///< A, M, D, R (renamed: old_path -> path)
    std::string path;
    std::string old_path;
    int added = 0, removed = 0;
    bool binary = false;
};
/// Python bytecode and test caches (*.pyc, *.pyo, __pycache__/, .pytest_cache/):
/// git_commit never commits them, only their deletion.
bool is_bytecode(std::string_view rel);
std::vector<FileChange> diff_snapshots(const Project& project, const Snapshot& before, const Snapshot& after);

struct ChangeReview {
    std::vector<FileChange> files;
    int lines = 0;
    std::vector<std::string> reverted;  ///< what was undone, and why
    std::vector<std::string> flags;     ///< what the steward should look at
    bool hold = false;                  ///< the task cannot be reported done
};

/// Review a run's changes; with `revert`, undo the violations in the work tree.
/// `sandboxed`: the agent ran in the sandbox, which kept the built-in
/// protected paths (the plan, the changelog, .myteam/, .git/, CI) read-only:
/// a change to them came from elsewhere (the operator's `myteam done`, the
/// customer's reply, the PO), so it is not reverted; the project's own
/// guard.protect paths, which only the guard keeps, still are.
ChangeReview review_changes(const Project& project, const GuardPolicy& policy, const Snapshot& before,
                            const Snapshot& after, bool revert, bool sandboxed = false);

/// Put `path` back as it was in `snap` (deleting it if it did not exist).
bool restore_path(const Project& project, const Snapshot& snap, const std::string& path);

/// Whether a path names a test (tests/, test_*.py, *_test.*, *.spec.*, ...).
bool is_test_path(std::string_view path);

/// A commit of a run's work, asked for through the git_commit tool.
struct CommitRequest {
    std::string message;
    std::vector<std::string> paths;  ///< files or directories; empty: all of the run's uncommitted changes
};
struct CommitIdentity {
    std::string handle, name, email;  ///< the author; email empty: <handle>@myteam.invalid
    std::string task, run;            ///< trailers
};
struct RunCommit {
    bool ok = false;
    std::string commit, branch, summary;
    std::string message;  ///< the whole message, as committed
    std::vector<std::string> files;
    std::vector<std::string> left_out;  ///< protected paths not taken
    std::vector<std::string> hooks;     ///< the hooks that ran
    std::vector<std::string> hook_changed;  ///< files a hook changed (a formatter's fixes)
    std::string error;                  ///< why nothing was committed
};
/// Commit what the run changed since `before` (and nobody committed yet) on
/// the current branch, as `who`, with the operator as committer. Only the
/// run's own changes; protected paths are left out (refused when named);
/// a deleted test, an added secret or a skipped test refuses the commit.
/// The project's own repository only; not in a merge or rebase, not on a
/// detached HEAD.
///
/// The project's hooks run (`commit_hooks: true`): pre-commit,
/// prepare-commit-msg and commit-msg can refuse the commit or change the
/// message, post-commit runs after. They run in the sandbox (a hooks path in
/// the project is the agents' to write), on a checkout of exactly what is
/// committed, in `work_dir` (the run directory; empty: .myteam/commit). What
/// they stage into the commit's own files is taken, and put in the work tree.
RunCommit commit_run_work(const Project& project, const GuardPolicy& policy, const Snapshot& before,
                          const CommitRequest& req, const CommitIdentity& who,
                          const std::filesystem::path& work_dir = {});

struct UndoResult {
    std::string run;
    std::string task;
    std::vector<std::string> restored;
    std::vector<std::string> skipped;   ///< changed again since the run
    std::vector<std::string> problems;
    std::vector<std::string> commits;   ///< what the run committed (undo leaves them; git revert)
};
/// Undo a run (a run directory name, or ^task for its latest run).
UndoResult undo_run(const Project& project, const std::string& key);

}  // namespace myteam
