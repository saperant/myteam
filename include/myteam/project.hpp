// A project: the directory with the plan, and its .myteam/ state.
//
//   PLAN.md                 the plan (Markdown, parsed by tasks/)
//   CHANGELOG.md            run summaries
//   .myteam/config.yaml     team, methodology, runners, limits
//   .myteam/project.md      the project brief every agent gets
//   .myteam/team/           project overrides of the team's files
//   .myteam/runs/           one directory per agent run
#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "myteam/runner.hpp"

namespace myteam {

struct Limits {
    int timeout_seconds = 1800;
    int max_turns = 0;
    double max_cost_usd = 0;
};


/// methodology: coach (coach: in config.yaml; see coach.hpp).
struct CoachSettings {
    int tasks_per_step = 3;        ///< expert task runs per step, at most
    int acceptances_per_step = 3;  ///< the Gatekeeper's acceptance runs per step, at most
    int max_rejections = 2;        ///< sent back this often: the task waits for the coach's decision
    int digest_tasks = 150;        ///< open tasks the coach's digest lists, at most (by priority)
    std::size_t memory_limit = 6000;  ///< characters of a member's memory in its context
    int acceptance_turns = 15;     ///< the Gatekeeper's turns per acceptance (0: no limit)
    int quiet_steps = 2;           ///< steps routine news waits before the coach decides on it anyway
    double step_cost = 0;          ///< USD a step may cost before it stops early; 0: no limit
};

/// guard: in config.yaml (see guard.hpp).
struct GuardPolicy {
    bool enabled = true;
    std::vector<std::string> deny_commands;   ///< regexes, added to the built-in rules
    std::vector<std::string> allow_commands;  ///< regexes that override the built-in refusals
    std::vector<std::string> protect;         ///< globs relative to the project, added to the built-in ones
    /// Globs taken out of the protection (e.g. ".github/workflows/**" when the
    /// team owns CI). Never the plan, the changelog, .git/ or myteam's state.
    std::vector<std::string> unprotect;
    std::vector<std::string> allow_paths;     ///< absolute directories outside the project agents may write
    bool checkpoints = true;                  ///< layer 2
    int max_files = 40;                       ///< a run changing more files is flagged
    int max_lines = 3000;                     ///< ... or more lines
};

/// A container myteam runs for the team (services.hpp).
struct ServiceSpec {
    std::string name;
    std::string image;
    std::vector<std::string> ports;            ///< "127.0.0.1:5432:5432" (loopback only)
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> files;  ///< container path -> project-relative file (read-only)
    std::string data;                          ///< container path of a named data volume; "" none
    std::vector<std::string> command;
    std::string network;                       ///< "" (its own) or "host"
    std::map<std::string, std::string> agent_env;  ///< what agents get: addresses, URLs
    std::string description;                   ///< for the agents
};

struct ProjectConfig {
    std::string team;
    std::string methodology = "direct";
    std::string plan = "PLAN.md";
    std::string brief = ".myteam/project.md";
    std::string changelog = "CHANGELOG.md";
    /// Where the product owner adds backlog items (its "Backlog" section).
    /// The plan itself in `direct`; Scrum will use BACKLOG.md.
    std::string backlog;
    Permission permission = Permission::EditFiles;  ///< for task work
    std::map<std::string, RunnerSettings> runners;
    Limits limits;
    /// Most characters of the brief given to an agent; the rest is cut.
    std::size_t brief_limit = 8000;
    /// Give agents the myteam MCP tools where the runner supports them.
    bool mcp = true;
    /// Agents' commits: "tool" (the git_commit MCP tool, checked by myteam) or "off".
    std::string commits = "tool";
    bool commit_hooks = true;      ///< run the project's git hooks on agents' commits
    int commit_hook_timeout = 300;  ///< seconds per hook
    CoachSettings coach;
    /// Shell commands agents with edit permission may run: {"all"}, {} or patterns.
    std::vector<std::string> commands = {"all"};
    GuardPolicy guard;
    /// The agents' sandbox (see sandbox.hpp): auto, bwrap, none.
    std::string sandbox = "auto";
    bool sandbox_network = true;
    std::vector<std::string> sandbox_writable;
    /// Directories hidden from the agents (an empty directory in their place):
    /// absolute, or relative to the project ("../web"). myorg hides the org's
    /// other teams this team does not work with.
    std::vector<std::string> sandbox_hide;
    /// Rules the org sets for this project (myorg writes them from ORG.md). Every agent reads them
    /// with the brief, after it; nobody on the team can change them.
    std::vector<std::string> rules;
    /// Directories outside the project agents read (relative to the project, absolute, or ~/):
    /// the org's interfaces, a partner team's repository, a tool's sources and docs.
    std::vector<std::string> readable;
    /// Directories outside the project agents also write, like the project (relative, absolute
    /// or ~/): a contract the team owns in the org's interfaces/. In the sandbox and the guard.
    std::vector<std::string> writable;
    /// Directories in the project agents read but never write (relative):
    /// protected like the plan, mounted read-only. The org's view of its units.
    std::vector<std::string> readonly;
    /// Other teams of the org (see myorg): handle -> what they do. Agents can
    /// name them in a blocker; tasks for them are not run here (they hold no
    /// sprint), myorg carries them to that team and their outcome back.
    std::map<std::string, std::string> external;
    std::vector<ServiceSpec> services;
    /// Domains agents may read web pages from (official sources, docs); none: no web.
    std::vector<std::string> web;
    /// What the operator tells every agent about the environment (a browser, a
    /// tool, where something is): one line each, in every task's context.
    std::vector<std::string> notes;
    std::vector<std::string> services_podman;  ///< the podman command; empty: found
};

class Project {
public:
    /// Open a project directory; a missing config gives the defaults.
    /// Throws std::runtime_error for a config that does not parse.
    static Project open(const std::filesystem::path& dir);

    const std::filesystem::path& dir() const { return dir_; }
    const ProjectConfig& config() const { return config_; }
    ProjectConfig& config() { return config_; }
    bool has_config() const { return has_config_; }

    std::filesystem::path state_dir() const { return dir_ / ".myteam"; }
    std::filesystem::path config_path() const { return state_dir() / "config.yaml"; }
    std::filesystem::path plan_path() const { return dir_ / config_.plan; }
    std::filesystem::path brief_path() const { return dir_ / config_.brief; }
    std::filesystem::path changelog_path() const { return dir_ / config_.changelog; }
    std::filesystem::path runs_dir() const { return state_dir() / "runs"; }
    std::filesystem::path team_overlay() const { return state_dir() / "team"; }
    std::filesystem::path backlog_path() const { return config_.backlog.empty() ? plan_path() : dir_ / config_.backlog; }

    // The customer's side; never part of a team member's context.
    /// Organisational feedback from the team, for myteam itself.
    std::filesystem::path feedback_path() const { return state_dir() / "feedback.md"; }
    std::filesystem::path customer_dir() const { return state_dir() / "customer"; }
    /// Relative to dir(): ".myteam/customer".
    std::string customer_rel() const { return ".myteam/customer"; }

    /// The brief for agents: the brief file, or a generated outline of the
    /// project when there is none. Cut to config().brief_limit.
    std::string brief() const;
    /// The readable directories, absolute (those that exist).
    std::vector<std::string> read_paths() const;
    /// The writable directories, absolute (those that exist).
    std::vector<std::string> write_paths() const;

    RunnerSettings runner_settings(const std::string& runner) const;

private:
    std::filesystem::path dir_;
    ProjectConfig config_;
    bool has_config_ = false;
};

struct InitResult {
    std::vector<std::filesystem::path> created;
    std::vector<std::filesystem::path> kept;  ///< existed; not overwritten
};

/// Create .myteam/config.yaml, the brief template and an empty plan, keeping
/// what exists unless `force`.
InitResult init_project(const std::filesystem::path& dir, const std::string& team, bool force);

}  // namespace myteam
