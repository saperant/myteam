// Teams: members, roles, prompts, models, handoffs and allocation rules.
//
// A team is a directory:
//
//   team.md            front matter: id, name, product_owner, steward, allocation, ...
//                      body: the team overview
//   members/<h>.md     front matter: name, role, skills, models, handoffs
//                      body: the member's role prompt
//
// A project can override any of these files with the same relative path
// under .myteam/team/ (and add members there). Retrospectives change the
// overlay, never the base team.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace myteam {

/// "claude:opus" -> {runner "claude", model "opus"}. A bare "claude" means the
/// runner's own default model.
struct ModelRef {
    std::string runner;
    std::string model;

    static ModelRef parse(std::string_view spec);
    std::string str() const { return model.empty() ? runner : runner + ":" + model; }
    bool empty() const { return runner.empty(); }
    friend bool operator==(const ModelRef&, const ModelRef&) = default;
};

/// Models by weight class. The default is used unless a task's priority
/// calls for premium (#p0) or economy (#p2, #p3), and ceremonies that only
/// talk use economy.
struct ModelProfile {
    ModelRef standard;
    std::optional<ModelRef> premium;
    std::optional<ModelRef> economy;

    const ModelRef& for_priority(std::optional<int> priority) const;
    const ModelRef& for_talk() const { return economy ? *economy : standard; }
};

struct HandoffRule {
    std::string when;  ///< "implementation is complete and needs tests"
    std::string to;    ///< member handle
    std::string note;
};

struct Member {
    std::string handle;  ///< lower case, without '@'
    std::string name;
    std::string role;
    std::string summary;  ///< one line: what this member does (for teammates' context)
    std::vector<std::string> skills;
    ModelProfile models;
    std::vector<HandoffRule> handoffs;
    std::string prompt;  ///< the role prompt (body of the member file)
    /// The org's unit this member works in (a directory of the project, "api"): its repository
    /// is the member's working directory, and the unit's boundaries (what it reads, writes and
    /// may not see) are the member's. Empty: the project itself.
    std::string unit;
    std::filesystem::path source;
};

struct AllocationWeights {
    int priority = 4;
    int deadline = 3;
    int blocker = 5;
    int skill = 3;
    int workload = 2;
};

struct AllocationRules {
    /// Plan assignee words ("@qa", "@backend") that stand for a member.
    std::vector<std::pair<std::string, std::string>> roles;
    /// Words in task text that point to a member.
    std::vector<std::pair<std::string, std::string>> keywords;
    AllocationWeights weights;
};

/// Team Topologies' four team types (team.md `topology:`).
inline constexpr const char* kTopologies[] = {"stream-aligned", "platform", "complicated-subsystem", "enabling"};

struct Team {
    std::string id;
    std::string name;
    std::string overview;     ///< body of team.md
    /// For the catalog an org picks teams from (myorg): one line on what the
    /// team does, its Team Topologies type (empty: not said), its domain and
    /// what it delivers.
    std::string summary;
    std::string topology;
    std::string domain;
    std::vector<std::string> delivers;
    /// The org's other teams this project works with (config.yaml external:):
    /// handle -> what they do. Not members: blockers name them, myorg carries
    /// the work across.
    std::map<std::string, std::string> external;
    /// Member handle: the customer's counterpart, owns the backlog, allocates
    /// work in `direct`. Read from product_owner: (or the older facilitator:).
    std::string product_owner;
    /// Member handle of the methodology's steward (Scrum Master, ...); empty:
    /// the methodology's built-in steward.
    std::string steward;
    std::string repairer;     ///< member handle that repairs a broken plan
    /// methodology: coach. The coach decides and assigns (the customer's contact: also the
    /// product owner); the Gatekeeper checks every decision and every acceptance.
    std::string coach;
    std::string gatekeeper;
    std::vector<Member> members;
    AllocationRules allocation;
    std::filesystem::path dir;
    std::filesystem::path overlay;  ///< empty when the project has none

    const Member* find(std::string_view handle) const;
    /// `handle` is one of the org's other teams (external), not a member.
    bool is_external(std::string_view handle) const;
    const Member& product_owner_member() const;
};

/// Load a team directory, applying `overlay` (may be empty or missing).
/// Throws std::runtime_error with the file and line on errors.
Team load_team(const std::filesystem::path& dir, const std::filesystem::path& overlay = {});

/// Directories searched for a team by name, in order: <project>/.myteam/teams,
/// $MYTEAM_TEAMS (colon separated), ~/.config/myteam/teams, the source tree's
/// plugins/teams.
std::vector<std::filesystem::path> team_search_path(const std::filesystem::path& project_dir);
std::optional<std::filesystem::path> find_team_dir(std::string_view name,
                                                   const std::filesystem::path& project_dir);

/// A team found on the search path, for an org choosing teams (myteam teams).
struct CatalogEntry {
    std::string name;  ///< what -t takes
    std::filesystem::path dir;
    Team team;
    std::string error;  ///< the team does not load: why
};

/// Every team on team_search_path(), the first of each name.
std::vector<CatalogEntry> team_catalog(const std::filesystem::path& project_dir);

}  // namespace myteam
