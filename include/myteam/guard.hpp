// Safeguards against agents doing harm, in two layers.
//
// 1. Before each action (`myteam guard`): Claude runs a PreToolUse hook for
//    every shell command and file write; the guard refuses, with a reason
//    the agent reads, what the policy forbids:
//      - destructive or outward commands: recursive deletes outside the
//        work, git push / reset --hard / clean / rebase / amend / switching
//        branches, sudo, piping a download into a shell, system and global
//        package installs, writes to devices, shutdown;
//      - writes outside the project (except the temp directory and
//        allow_paths), and writes to protected paths: the plan, the
//        changelog, .myteam/, .git/, CI workflows, secrets (.env, keys);
//      - reading credentials (~/.ssh, ~/.aws, ~/.gnupg, ...).
//    Refusals are logged in the run directory (guard.jsonl); the loop
//    reports them, and the steward sees them.
//    Codex runs in its own sandbox (workspace-write); Vibe has no hook.
//
// 2. After each run (checkpoints, see checkpoint.hpp): what the run
//    changed is compared with a snapshot taken before it.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/project.hpp"

namespace myteam {


GuardPolicy guard_policy(const Project& project);
/// The built-in protected paths plus the policy's (globs relative to the project).
std::vector<std::string> protected_globs(const Project& project, const GuardPolicy& policy);

/// `*` (not /), `**` (anything), `?`.
bool glob_match(std::string_view pattern, std::string_view path);
/// Whether `rel` (relative to the project) is protected: a glob matches it and
/// no exception ("!glob", e.g. "!.myteam/project.md") does.
bool path_protected(const std::vector<std::string>& globs, std::string_view rel);
/// The project brief, relative to the project, when the team may write it (it
/// is inside the project and not on the customer's side); else "".
std::string writable_brief(const Project& project);

struct GuardDecision {
    bool allow = true;
    std::string reason;  ///< why not, for the agent
};

struct GuardContext {
    std::filesystem::path project;  ///< the project directory
    std::filesystem::path cwd;      ///< where the command runs
    std::vector<std::string> protect;
    std::vector<std::filesystem::path> writable;  ///< besides the project: temp dirs, allow_paths
    const GuardPolicy* policy = nullptr;
    bool commit_tool = false;  ///< agents commit through git_commit, not git commit
    /// Paths the agent may not read, or name in a command (absolute): a unit's agent and the
    /// org's plan, changelog and state. Its task is in its prompt.
    std::vector<std::filesystem::path> hidden;
};

GuardContext guard_context(const Project& project, const GuardPolicy& policy, const std::filesystem::path& cwd);

/// A shell command line (with ;, &&, ||, |, $(...), bash -c "..." looked into).
GuardDecision check_command(const GuardContext& ctx, std::string_view command);
/// Writing (or deleting) this path.
GuardDecision check_write(const GuardContext& ctx, std::string_view path);
/// Reading this path (credentials only).
GuardDecision check_read(const GuardContext& ctx, std::string_view path);

/// A Claude PreToolUse hook input: {tool_name, tool_input, cwd, ...}.
GuardDecision check_tool_call(const GuardContext& ctx, const taskml::Json& input);

/// The guard log of a run directory (guard.jsonl): what was refused.
struct GuardBlock {
    std::string tool;
    std::string what;  ///< the command or path
    std::string reason;
};
std::vector<GuardBlock> load_guard_blocks(const std::filesystem::path& run_dir);
void log_guard_block(const std::filesystem::path& run_dir, const GuardBlock& b);

/// The hook: read the input from stdin, decide, log. Returns the exit code
/// (0 allow, 2 refuse; the reason goes to stderr for the agent).
int run_guard_hook(const Project& project, const std::filesystem::path& run_dir, std::istream& in, std::ostream& err,
                   const std::vector<std::string>& hidden = {});

}  // namespace myteam
