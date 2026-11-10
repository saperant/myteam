// The clean context an agent starts from.
//
// An agent gets only what its work needs, in this order:
//
//   1. Role       who it is: name, role, team, its role prompt
//   2. Project    the project brief (.myteam/project.md), cut to a budget
//   3. Task       the task and its own place in the plan: section, parent
//                 chain, notes, criteria, subtasks, dependencies and blockers
//                 with their state, and the tasks waiting for it
//   4. Teammates  only the members its handoff rules lead to
//   5. Reporting  how to report status, a summary and handoffs
//
// Left out on purpose: the rest of the plan, other members, history,
// changelog, and instructions of other roles. The agent does not edit the
// plan file; it reports, and myteam updates the plan.
#pragma once

#include <string>
#include <vector>

#include "myteam/plan.hpp"
#include "myteam/team.hpp"

namespace myteam {

class Project;

struct ContextSection {
    std::string id;     ///< role, project, task, teammates, reporting
    std::string title;
    std::string body;
};

struct AgentContext {
    std::vector<ContextSection> sections;

    std::string render() const;
    std::size_t estimated_tokens() const;
    const ContextSection* section(std::string_view id) const;
};

struct ContextInputs {
    const Team& team;
    const Member& member;
    const Plan& plan;
    std::string brief;  ///< Project::brief()
    /// The agent has the myteam MCP tools (plan_*), besides the report blocks.
    bool tools = false;
    bool commits = false;  ///< the git_commit tool
    std::string services;  ///< services_brief(): the containers the project runs; "" none
    std::string protected_paths;  ///< what the safeguards refuse, and the project's exceptions
    std::vector<std::string> notes;  ///< the operator's notes on the environment
    std::string skills;  ///< skills_section(): what the team learned, and the skills this task calls for
    std::string memory;  ///< memory_section(): the member's coaching and lessons
};

/// The crew as a member sees it (coach methodology): who is who, by unit. "" for other teams.
std::string crew_section(const Team& team, const std::string& self);

/// The context for working on `task`.
AgentContext build_task_context(const ContextInputs& in, const TaskRef& task);

/// The context for repairing a plan that no longer validates.
AgentContext build_repair_context(const ContextInputs& in, const std::vector<taskml::Diagnostic>& problems);

/// The "Task" section alone (also used by `myteam show`).
std::string describe_task(const Plan& plan, const TaskRef& task);

}  // namespace myteam
