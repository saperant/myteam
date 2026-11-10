// The myteam MCP server: plan_* tools for an agent at work on a task.
//
//   myteam mcp --member developer --task ^count --run-dir .myteam/runs/<run> [--commits]
//
// With --commits it also has git_commit, which myteam answers (commits.hpp).
//
// The runner starts it for the agent (claude: --mcp-config; codex:
// -c mcp_servers.myteam...). It speaks MCP (JSON-RPC 2.0, one message per
// line) on stdin/stdout.
//
// Reading tools answer from the plan as it is now. Reporting tools do not
// write the plan: they check their input (a known member, a task in the
// plan, a criterion the task has) and answer with an error the agent can
// act on, then record into <run-dir>/mcp-report.json. When the run ends the
// loop merges that report with the blocks of the final message and applies
// both as one validated plan edit, exactly like a report written in blocks.
// So the plan changes once per run, and a failed run changes nothing.
#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

#include <taskml/json.hpp>

#include "myteam/project.hpp"
#include "myteam/report.hpp"
#include "myteam/team.hpp"

namespace myteam {

struct McpSession {
    const Project& project;
    const Team& team;
    std::string member;  ///< handle
    std::string task;    ///< task id, without '^'
    std::filesystem::path report_path;  ///< mcp-report.json; empty: keep it in memory
    bool commits = false;  ///< offer git_commit (myteam answers it from outside the sandbox)
    bool services = false;  ///< offer service_* (the same way)
};

class McpServer {
public:
    explicit McpServer(McpSession session);

    /// One JSON-RPC message; the response, or nothing for a notification.
    std::optional<taskml::Json> handle(const taskml::Json& message);
    /// Serve until end of input.
    void serve(std::istream& in, std::ostream& out);

    /// The tools/list result.
    taskml::Json tools() const;
    /// The tools/call result: {content: [{type: text, text}], isError}.
    taskml::Json call(std::string_view name, const taskml::Json& arguments);

    const AgentReport& report() const { return report_; }

private:
    void save() const;

    McpSession s_;
    AgentReport report_;
};

/// The report file name inside a run directory.
inline constexpr const char* kMcpReportFile = "mcp-report.json";

taskml::Json report_to_json(const AgentReport& r);
AgentReport report_from_json(const taskml::Json& j);
/// The report recorded through the tools in `run_dir`; empty when none.
AgentReport load_mcp_report(const std::filesystem::path& run_dir);

/// Merge what the tools recorded into the report from the final message:
/// the tools' status and summary win (they were checked); handoffs,
/// blockers, criteria and issues are joined without duplicates.
void merge_reports(AgentReport& from_text, const AgentReport& from_tools);

}  // namespace myteam
