// What an agent reports at the end of its work (see the Reporting section of
// its context): <task_status>, <work_summary> and <handoff to="...">.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <taskml/taskml.hpp>

namespace myteam {

struct HandoffRequest {
    std::string to;            ///< member handle
    std::string instructions;
};

/// What blocks a task, from <blocker ...>text</blocker>.
struct BlockerReport {
    enum class Kind {
        Task,      ///< on="^id": waits for a task in the plan
        Member,    ///< to="handle": work or a decision from a teammate
        Customer,  ///< to="customer": only the customer can answer (through the PO)
        Operator,  ///< to="operator": outside the team (access, credentials, tools)
    };
    Kind kind = Kind::Operator;
    std::string on;    ///< task id (Task)
    std::string to;    ///< member handle (Member)
    std::string text;  ///< what blocks, and what would unblock
};

/// Something in the way the team is organised that got in the way, from
/// <org_issue area="...">text</org_issue>: feedback for myteam itself, not
/// for the product or the plan.
struct OrgIssue {
    std::string area;  ///< context, plan, handoffs, tools, permissions, process, team, other
    std::string text;
};

/// An acceptance criterion checked, from <criterion n="1" status="verified">evidence</criterion>.
struct CriterionReport {
    int number = 0;  ///< 1-based, as listed in the task
    taskml::CriterionStatus status = taskml::CriterionStatus::Verified;
    std::string evidence;
};
std::optional<taskml::CriterionStatus> criterion_status_from_report(std::string_view s);
/// "skipped" (skip, not_run, n/a): the criterion could not be checked yet. It stays
/// pending, never a pass, with "skipped: <why>" as its evidence.
bool is_skipped_status(std::string_view s);
std::string skipped_evidence(std::string_view why);

struct AgentReport {
    std::optional<taskml::TaskStatus> status;
    std::string status_text;   ///< as written, also when not understood
    std::string reviewer;      ///< review: who reviews it (<task_status reviewer="x">, plan_report_status)
    std::string summary;
    std::vector<HandoffRequest> handoffs;
    std::vector<BlockerReport> blockers;
    std::vector<OrgIssue> org_issues;
    std::vector<CriterionReport> criteria;
    /// <finding>text</finding>: something outside the team for the operator (a
    /// tool's shortfall) that does not stop the task, so it does not block it.
    std::vector<std::string> findings;
    /// <lesson>text</lesson>: what the member learned that would have saved time (a tool's
    /// quirk, a way of working). It goes into the member's memory; the coach may make it a skill.
    std::vector<std::string> lessons;
    /// <web_access site="host">why</web_access>: a site the member needs that is not on the
    /// team's web list; the operator approves it (a "web" proposal). The task goes on.
    std::vector<std::pair<std::string, std::string>> web_access;
    /// <plan_edits>steward blocks</plan_edits>: changes to other tasks of the plan the member's
    /// task asks for (close, add, merge, reword); applied after the run as a "plan" proposal.
    std::string plan_edits;
    bool has_blocks() const {
        return !status_text.empty() || !summary.empty() || !handoffs.empty() || !blockers.empty() || !criteria.empty() ||
               !findings.empty() || !lessons.empty() || !web_access.empty() || !plan_edits.empty();
    }
};

/// The host of a site as the web list keeps it: "https://www.ti.com/lit/x.pdf" -> "www.ti.com";
/// "" when it is not a host name.
std::string web_host(std::string_view site);

/// done/complete(d) -> Completed, review -> Review, in_progress -> InProgress,
/// blocked -> Blocked, cancelled -> Cancelled.
std::optional<taskml::TaskStatus> report_status_from_string(std::string_view s);

/// A <tag attrs>body</tag> block of a report.
struct ReportBlock {
    std::string attrs;
    std::string body;  ///< trimmed
};
/// All blocks with this tag, in order.
std::vector<ReportBlock> report_blocks(std::string_view text, std::string_view tag);
/// An attribute's value (quoted or bare); empty when missing.
std::string block_attr(std::string_view attrs, std::string_view name);
/// A <blocker on=|to=> block.
BlockerReport parse_blocker(const ReportBlock& b);

/// The area of an <org_issue>: a known name, or "other".
std::string org_issue_area(std::string_view s);
std::vector<OrgIssue> parse_org_issues(std::string_view text);

/// Read the blocks from the agent's final message (the last of each wins;
/// handoffs accumulate).
AgentReport parse_report(std::string_view text);

}  // namespace myteam
