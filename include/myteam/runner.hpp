// AgentRunner: drives an agent CLI (Claude Code, Codex, Mistral Vibe) in
// headless mode and turns its output into a common event stream.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace myteam {

enum class Permission {
    ReadOnly,   ///< read and search only (ceremonies, reviews)
    EditFiles,  ///< edit files in the work directory; commands as the CLI allows
    Full,       ///< no sandbox, no prompts
};
std::optional<Permission> permission_from_string(std::string_view s);  // read-only, edit, full
const char* to_string(Permission p);

struct RunnerSettings {
    std::string command;             ///< executable; the runner id when empty
    bool isolate = false;            ///< skip the CLI's own memory/instruction files where it can
    /// Leave out what the agent never needs but pays for on every turn: the user's own MCP
    /// servers, skills, slash commands and settings (claude: --strict-mcp-config,
    /// --disable-slash-commands, --setting-sources ""). Measured 2026-11-10: a worker's first
    /// turn 19.5k -> 13k tokens.
    bool slim = true;
    std::vector<std::string> args;   ///< extra arguments
    std::map<std::string, std::string> env;
};

/// An MCP server for the agent (stdio): myteam's plan tools.
struct McpServerSpec {
    std::string name = "myteam";
    std::vector<std::string> command;  ///< argv
};

struct RunRequest {
    std::string member;       ///< handle, for logs
    std::string what;         ///< what the run is for: "^task", "steward", "ceremony: standup", "customer: THREAD"
    std::map<std::string, std::string> env;  ///< added to the agent's environment (the services' addresses)
    std::string model;        ///< runner-specific model name; empty: the CLI's default
    std::string prompt;
    std::filesystem::path workdir;
    std::filesystem::path run_dir;  ///< where the runner keeps prompt, stream and logs
    Permission permission = Permission::EditFiles;
    std::optional<std::string> resume_session;
    int max_turns = 0;        ///< 0: no limit
    double max_cost_usd = 0;  ///< 0: no limit
    int timeout_seconds = 0;  ///< 0: no limit
    /// Directories (relative to workdir) the agent may not read or write.
    /// Enforced where the CLI can (claude); elsewhere only by instruction.
    std::vector<std::string> deny_paths;
    /// Tools for the agent, where the CLI supports MCP servers.
    std::optional<McpServerSpec> mcp;
    /// Shell commands the agent may run without asking, with edit permission:
    /// {"all"}, {} (none), or patterns ("pytest:*", "python3 -m pytest:*").
    std::vector<std::string> commands = {"all"};
    /// Read-only runs get `commands` too (the Gatekeeper runs the checks; plan mode keeps the
    /// file tools from writing, the guard and the checkpoint watch the commands).
    bool read_only_shell = false;
    /// Domains whose pages the agent may fetch (Claude: WebFetch(domain:...)); none: no web.
    std::vector<std::string> web_domains;
    /// Directories outside the project the agent reads (absolute; Claude: Read(//dir/**)).
    std::vector<std::string> read_paths;
    /// Directories outside the project the agent writes (absolute; Claude: Edit and Write rules,
    /// which acceptEdits gives only inside the working directory).
    std::vector<std::string> write_paths;
    /// A command checking every tool call before it runs (argv; claude:
    /// a PreToolUse hook). Empty: none.
    std::vector<std::string> guard_hook;
    /// argv prefix running the CLI in a sandbox (bwrap ... --); empty: none.
    std::vector<std::string> sandbox;
    /// A run that only talks: one answer to the prompt, no tools and no files (the coach, the
    /// Gatekeeper's review of the coach's decisions, a council view). The CLI's own system
    /// prompt, tools and skills are left out where it can (claude: 19.5k -> 0.6k tokens).
    bool talk = false;
    std::string system_prompt;  ///< talk: the system prompt; empty: a short default
    /// Absolute paths the agent may not read (a unit's expert: the org's plan, changelog and
    /// state; it gets what it needs in its prompt). Claude: Read/Grep/Glob deny rules.
    std::vector<std::string> deny_read;
};

struct Usage {
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cached_input_tokens = 0;
    double cost_usd = 0;  ///< 0 when the CLI does not report it
    int turns = 0;
};

struct ToolCall {
    std::string name;    ///< "Bash", "shell", "read_file", ...
    std::string detail;  ///< command line, path or compact arguments
    bool failed = false;
};

struct AgentEvent {
    enum class Kind { Started, Text, ToolCall, ToolResult, Usage, Error, Finished };
    Kind kind = Kind::Text;
    std::string text;  ///< message text, tool detail or error
    std::string tool;  ///< tool name for ToolCall / ToolResult
};
using EventSink = std::function<void(const AgentEvent&)>;

/// Why a run did not finish its work, as far as the output tells.
enum class Failure {
    None,
    NotAvailable,  ///< the CLI is missing or could not start
    Auth,          ///< not logged in, bad key
    RateLimit,     ///< rate limit, overload, quota: try later
    Limit,         ///< stopped at a turn, cost or time limit: work may continue
    Crash,         ///< anything else
};
const char* to_string(Failure f);
/// Classify from the exit code, error, stop reason and stderr.
Failure classify_failure(int exit_code, bool timed_out, std::string_view error, std::string_view stop_reason,
                         std::string_view stderr_tail);

struct RunResult {
    bool ok = false;
    Failure failure = Failure::None;
    int exit_code = -1;
    bool timed_out = false;
    std::string final_text;   ///< the agent's last message
    std::string session_id;   ///< for resuming
    std::string model;        ///< as reported by the CLI
    std::string stop_reason;
    std::string error;
    Usage usage;
    std::vector<ToolCall> tool_calls;
    std::filesystem::path prompt_path, stream_path, stderr_path;
};

/// Accumulates what a runner's stream parser finds.
struct StreamState {
    RunResult result;
    std::string last_text;  ///< latest assistant text, used when no final text is reported
};

class AgentRunner {
public:
    explicit AgentRunner(RunnerSettings settings) : settings_(std::move(settings)) {}
    virtual ~AgentRunner() = default;

    virtual std::string id() const = 0;
    /// It can give the agent an MCP server (RunRequest::mcp).
    virtual bool supports_mcp() const { return false; }
    /// The executable is installed (on $PATH or ~/.local/bin).
    bool available() const;
    /// Why the CLI cannot start here, or "": not installed, or a script whose
    /// interpreter is missing (installed for another system, e.g. on the host
    /// of a toolbox container, or a removed Python).
    virtual std::string start_problem() const;
    std::filesystem::path executable() const;

    /// Run to completion. Writes prompt.md, stream.jsonl and stderr.log into
    /// request.run_dir. Events are delivered on the calling thread.
    virtual RunResult run(const RunRequest& request, const EventSink& sink = {});

    // Exposed for tests: the command line and the stream parser.
    virtual std::vector<std::string> command(const RunRequest& request) const = 0;
    /// Text written to the process's stdin (the prompt, unless the CLI reads a file).
    virtual std::string stdin_text(const RunRequest& request) const { return request.prompt; }
    virtual std::map<std::string, std::string> environment(const RunRequest&) const { return {}; }
    virtual void parse_line(std::string_view line, StreamState& state, const EventSink& sink) const = 0;
    /// After the process exits: read exported files, settle the result.
    virtual void finish(const RunRequest& request, StreamState& state) const;

    const RunnerSettings& settings() const { return settings_; }

protected:
    RunnerSettings settings_;
};

/// The model to run: `wanted`, or `standard` when wanted's CLI cannot start
/// here (start_problem: not installed, its interpreter missing). `note` gets
/// why once per runner in `told` (the caller's: a ceremony of ten members says
/// it once). `make` makes the runner for an id (nullptr: unknown, left as is).
struct ModelRef;
ModelRef usable_model(const ModelRef& wanted, const ModelRef& standard,
                      const std::function<std::unique_ptr<AgentRunner>(const std::string&)>& make,
                      std::set<std::string>& told, const std::function<void(const std::string&)>& note);

/// "claude", "codex" or "vibe"; nullptr for an unknown id.
std::unique_ptr<AgentRunner> make_runner(std::string_view id, RunnerSettings settings = {});
std::vector<std::string> runner_ids();

}  // namespace myteam
