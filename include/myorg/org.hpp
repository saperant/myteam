// myorg: one crew across an organisation's units.
//
//   suite/                      the org: a git repository, and a myteam project (methodology: coach)
//     ORG.md                    the units, their Team Topologies types and interactions
//     PLAN.md                   the one task list: every expert's tasks, in every unit
//     interfaces/               the contracts between units (every unit reads them)
//     .myteam/                  the crew: the coach, the Gatekeeper, the experts (one team)
//     .myorg/config.yaml        myorg's settings
//     .myorg/state.json         ticks, milestones
//     .myorg/log/ticks.jsonl    what each tick did
//     api/ web/ cli/            units: git repositories (submodules). Each has a .myteam/config.yaml
//                               that myorg writes from ORG.md: what the unit's experts read, write
//                               and may not see, and the org's rules for them
//
// The crew is flat: the coach decides, the experts work in their units, the Gatekeeper checks
// (coach.hpp). What stays of the organisation is its boundaries. An expert sees its own
// repository, the contracts and the units its interactions open to it (Team Topologies:
// collaboration opens the other's repository, x-as-a-service only what the provider
// publishes), so the experts meet at the contracts and use each other's tools through their
// documented surfaces, as people in separate teams would; that is where a tool's gaps show.
// An interaction can end (`until: N`, the last milestone it lasts).
//
// A tick: make the units' views match ORG.md, run one coach step on the org; at a milestone
// commit every unit and the org (with the units' revisions) and tag it milestone-N.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/coach.hpp"

namespace myorg {

namespace fs = std::filesystem;

/// Team Topologies' interaction modes.
inline constexpr const char* kModes[] = {"collaboration", "x-as-a-service", "facilitating"};

struct Unit {
    std::string handle;   ///< the unit's name (api); the directory by default
    std::string team;     ///< the catalog team its experts come from (for its summary)
    std::string type;     ///< Team Topologies type; empty: the team's own
    std::string purpose;  ///< what the unit is for, in this org
    std::string dir;      ///< relative to the org
    /// What the org requires of the unit's work (its tools, its flows): every expert in the
    /// unit reads them with its brief, and nobody in the crew can change them.
    std::vector<std::string> rules;
    /// What the unit keeps in the org's repository (relative to the org: interfaces/events):
    /// its experts write there as in their own repository.
    std::vector<std::string> owns;
    /// What else its experts read outside it (absolute or ~/: a tool's sources and docs).
    std::vector<std::string> reads;
    /// What its experts write outside the org (absolute or ~/: a worktree of a tool the unit
    /// develops); not committed by myorg: whoever releases the tool commits and merges it.
    std::vector<std::string> writes;
};

struct Interaction {
    std::vector<std::string> between;  ///< two units
    std::string mode;                  ///< collaboration, x-as-a-service, facilitating
    /// x-as-a-service: the unit that provides; facilitating: the unit that helps.
    std::string provider;
    /// What the consumer uses of the provider, relative to the org ("interfaces/web");
    /// empty: interfaces/<provider>.
    std::vector<std::string> via;
    int until = 0;  ///< the last milestone of the mode: it ends when the crew reaches the next (0: open)
    std::string then = "x-as-a-service";  ///< ... and becomes this ("none": it ends)
    std::string note;

    bool involves(std::string_view unit) const;
    std::string other(std::string_view unit) const;
    /// The mode while the crew works toward milestone `milestone` (1, 2, ...): "" when it has ended.
    std::string mode_in(int milestone) const;
};

struct OrgSpec {
    std::string name;
    std::vector<Unit> units;
    std::vector<Interaction> interactions;
    std::vector<std::string> rules;  ///< for every unit, before its own
    std::string body;  ///< ORG.md after the front matter

    const Unit* find(std::string_view handle) const;
};

/// ORG.md: front matter (name, units, interactions) and a body. Throws
/// std::runtime_error on bad front matter.
OrgSpec parse_org_spec(std::string_view text);
OrgSpec load_org_spec(const fs::path& org_dir);
/// What is wrong with it: unknown units, modes, a provider outside the pair.
std::vector<std::string> check_org_spec(const OrgSpec& spec);
/// ORG.md again, front matter from `spec` (comments in it are not kept).
std::string render_org_spec(const OrgSpec& spec);

/// .myorg/config.yaml
struct OrgConfig {
    std::string runner;   ///< passed to every run (--runner); empty: the members' own
    int max_steps = 200;  ///< coach steps per myorg run, at most (a safety net)
    /// Seconds to wait when the runner is rate limited before trying again (myorg run); 0: stop.
    int rate_limit_wait = 600;
    /// A command run for each new need (myorg run and myorg watch): `sh -c`, with
    /// MYORG_NEED_KIND, MYORG_NEED_KEY and MYORG_NEED_TEXT set and the text as $1
    /// (e.g. notify-send myorg "$1"). Empty: none.
    std::string notify;
};
OrgConfig load_org_config(const fs::path& org_dir);

/// What a unit's experts see, from the interactions while the crew works toward `milestone`
/// (Team Topologies: keep each one's cognitive load to its own work and the interfaces it uses).
struct UnitView {
    std::vector<std::string> hide;      ///< relative to the unit ("../cli")
    std::vector<std::string> rules;     ///< the org's, then the unit's own
    std::vector<std::string> readable;  ///< relative to the unit: the interfaces, the units it sees
    std::vector<std::string> writable;  ///< relative to the unit: what it owns in the org
    std::vector<std::string> notes;     ///< who it works with, and how
};
UnitView unit_view(const OrgSpec& spec, std::string_view unit, int milestone, const fs::path& org_dir = {});

/// The lines myorg keeps in a config.yaml, between its markers.
std::string unit_config_block(const UnitView& view);
std::string org_config_block(const OrgSpec& spec);
/// `config` with its myorg block replaced by `block` (appended when missing).
/// Throws when a key of the block is also set outside it.
std::string replace_managed_block(std::string_view config, std::string_view block);

struct InitOptions {
    std::string name;           ///< empty: the directory's name
    std::string team;           ///< the crew's team (coach, Gatekeeper, experts): a name or a directory; required
    bool force = false;
};
/// A new org in `dir`: git, the crew's myteam project (methodology coach), ORG.md,
/// interfaces/, .myorg/. Returns what it did.
std::vector<std::string> init_org(const fs::path& dir, const InitOptions& o);

struct SyncResult {
    std::vector<std::string> created, updated, notes, problems;
};
/// Make the org match ORG.md: create missing units (a git repository with a myteam config, a
/// submodule), and write each unit's view and the org's config.
SyncResult sync_org(const fs::path& org_dir, int milestone, bool dry_run = false);

/// .myorg/state.json
struct OrgState {
    int milestone = 1;  ///< the milestone the crew works toward
    int ticks = 0;
};
OrgState load_org_state(const fs::path& org_dir);
void save_org_state(const fs::path& org_dir, const OrgState& s);

struct Tick {
    std::vector<std::string> notes;
    std::vector<std::string> problems;
    bool waiting = false;  ///< nothing goes on without you (or the rate limit)
    std::string waiting_for;
    bool rate_limited = false;
    bool milestone = false;
    double cost = 0;
};

struct EngineOptions {
    bool dry_run = false;
    std::function<void(const std::string&)> on_note;
    /// The runs' options (live events, the MCP and guard commands).
    myteam::LoopOptions loop;
    /// The crew's runners; empty: the org project's (tests give fakes).
    myteam::RunnerFactory runners;
};

class Engine {
public:
    Engine(fs::path org_dir, EngineOptions o = {});

    /// Sync, one coach step, and the milestone's commit when there is one.
    Tick tick();
    /// Commit every unit and the org, and tag milestone-N.
    std::vector<std::string> close_milestone(OrgState& st);

private:
    fs::path org_;
    EngineOptions o_;
};

/// The ticks myorg ran, newest first (.myorg/log/ticks.jsonl).
taskml::Json org_log(const fs::path& org_dir, int max = 200);

// --- what needs you ------------------------------------------------------------------
//
// The crew goes on while something waits for a person, so a person has to be told, or
// operator tasks, the coach's messages and accepted tool fixes wait while `myorg run` keeps
// stepping. What needs you is computed from the org's files:
//
//   operator  an open @operator task
//   customer  the coach wrote to you last, and you have not answered
//   release   a tool unit's (a unit with writes:) task the Gatekeeper accepted that is not
//             released yet (myorg released UNIT says it is)
//   waiting   the last tick waits for you (idle, a milestone, ORG.md problems)
//   stopped   the last tick is older than `stale` minutes: nothing is running the org
//
// `myorg run` prints each new one (NEEDS YOU) and runs the notify command; `myorg watch`
// exits as soon as there is a new one, so a watcher in the background hears it.

struct Need {
    std::string kind;  ///< operator, customer, release, waiting, stopped
    std::string key;   ///< stable: what makes it new ("operator:^id", "release:linter:^id", ...)
    std::string text;  ///< one line, for a person
};

/// Everything that needs you now. `stale_minutes` 0: no "stopped" need.
std::vector<Need> org_needs(const fs::path& org_dir, int stale_minutes = 0);

/// .myorg/watch.json: what each audience was told, and what each tool unit released.
struct WatchState {
    std::set<std::string> notified;  ///< by myorg run's notify
    std::set<std::string> watched;   ///< by myorg watch
    std::map<std::string, std::set<std::string>> released;  ///< unit -> task ids released
};
WatchState load_watch_state(const fs::path& org_dir);
void save_watch_state(const fs::path& org_dir, const WatchState& s);

/// The needs not in `seen`, which then holds them; keys no longer needed leave `seen`, so
/// a need that comes back is new again.
std::vector<Need> new_needs(const std::vector<Need>& now, std::set<std::string>& seen);

/// Record `unit`'s accepted tasks as released (myorg released UNIT): their release needs go.
std::vector<std::string> mark_released(const fs::path& org_dir, const std::string& unit);

/// Run the config's notify command for each need; what failed.
std::vector<std::string> notify_needs(const fs::path& org_dir, const std::vector<Need>& needs);

}  // namespace myorg
