# myteam architecture

myteam runs a team of AI agents against a plan written in Markdown. It is the
written in C++20 with Python for glue.

Status: the `direct` methodology (task rounds), teams, the clean context,
the claude, codex and vibe runners, blockers and failures, feedback for
myteam, the MCP plan tools, agents' commits and the safeguards, and the
`coach` methodology: a flat crew with a coach who decides, a Gatekeeper who
checks, and experts who learn (*The coach* below). myorg, which runs one crew
across an organisation's units, is in [MYORG.md](MYORG.md).

An earlier Scrum methodology (its five ceremonies, the product owner's desk, the
steward, proposals, the blog) was taken out: the ceremonies cost more than the
work they organised.

## Principles

1. **The plan is the source of truth.** `PLAN.md` holds the work as ordinary
   Markdown (front matter, headings, task lists), so editors, previews and
   linters work on it. [tasks/](tasks/) parses and validates it (its
   Markdown dialect of TaskML).
2. **Every agent starts from a clean context.** An agent gets its role, the
   project brief, its task with only the plan around that task, the teammates
   its handoffs lead to, and how to report. Nothing else (see *The clean
   context*).
3. **Agents do not edit the plan.** They report status, a summary and
   handoffs; myteam writes the plan, line by line so prose survives, and only
   when the result validates. Letting agents edit `PLAN.md` meant repairing the
   damage; the repair round remains as a safety net for hand edits. The MCP
   server (`myteam mcp`, the `plan_*` tools) gives agents the same reports as
   tools that check their input. Agents do not read the plan either: what a
   task needs is in its context, and a unit's agent is refused the org's plan.
4. **Teams are data, methodologies are behaviour.** A team is a folder of
   Markdown files with YAML front matter. A methodology (`direct`, `coach`)
   is code.
5. **Agents are external programs.** myteam does not implement an agent loop.
   Claude Code, Codex and Mistral Vibe already provide tools, sandboxing,
   permissions and login; myteam drives them through `AgentRunner`.
6. **What the crew learns is on record.** Lessons go into the members'
   memory, skills into `.myteam/skills/` (committed in git as they are made):
   the user follows up on them, or reverts them.
7. **The customer talks only to the coach.** The experts never see the
   conversation, only the tasks the coach makes of it.
8. **Everything works headless.** The `myteam` CLI does everything; the
   [myrcp](https://github.com/saperant/myrcp) workbench (`ui/`) is a view onto it.

## Layout

```
myteam/
  include/myteam/    the library's headers
  src/               plan, team, context, loop, coach, memory, report, blockers,
                     guard, checkpoint, commits, sandbox, mcp, process, yaml
  src/org/           myorg: ORG.md, the units' views, the tick
  src/runners/       AgentRunner backends: claude, codex, vibe
  apps/myteam.cpp    CLI
  tests/             GoogleTest suite (a fake agent drives the loop)
  plugins/teams/     crew/ (the coach's), software/, research/
  examples/          example plans
```

Per project, myteam keeps its state next to `PLAN.md`:

```
PLAN.md                the plan
CHANGELOG.md           one entry per agent run
.myteam/
  config.yaml          team, methodology, permission, limits, runners
  project.md           the project brief every agent gets
  team/                project overrides of the team's files
  memory/<handle>.md   each member's memory: the coach's coaching, its lessons
  skills/<slug>/SKILL.md   what the crew learned to do
  coach/state.json     the coach's news, what it saw, a held objection, the cost
  council/             each council: the question and the views
  customer/conversation.md   the customer and the coach
  runs/<stamp>-<member>-<task>/
                       command.txt, prompt.md, stream.jsonl, stderr.log, result.md,
                       run.json (usage, tool calls, report: input for retrospectives)
  runs/<run>/guard.jsonl, guard.json   what the guard refused; the run's checkpoints
  feedback.md          the crew's feedback for myteam
```

## The plan

```markdown
---
project: Demo
sprint: Sprint 3
goal: Ship the API
---

# Demo

Prose, tables and code blocks are for people; myteam ignores them.

## Backend

- [ ] Build the API #p1 ~4h @developer ^api
  - depends: ^design
  - Use REST; JSON errors
  - [ ] Endpoints @developer
  - ( ) Returns JSON
```

Statuses `[ ] [~] [x] [!] [-] [?]` and criteria `( )` pending, `(x)` verified,
`(!)` failed, all plain ASCII so they are easy to type (`○ ✓ ✗` are still
read). Priority `#p0`–`#p3`, estimate `~4h`,
assignee `@member`, tags, due `!2026-12-01`, id `^api`, and nested
`depends:`, `blocked-by:`, notes, subtasks and criteria. `taskml md --taskml
old.plan` converts TaskML plans (`myteam convert` does the same).

A task is **ready** when it is open, its dependencies and blockers are
finished, and it has no open subtask assigned to a member (then the subtasks
are the work).

## The clean context

`build_task_context()` (`include/myteam/context.hpp`) builds the whole prompt
from five sections:

| Section | Contents | Source |
|---|---|---|
| Your role | name, handle, role, team, skills, the role prompt | `members/<h>.md` |
| The project | the project brief, cut to `brief_limit` | `.myteam/project.md`, or a generated outline |
| Your task | sprint and goal; the task and its metadata; its section and parent chain; notes, criteria, subtasks; dependencies and blockers with their state; the tasks waiting for it | the plan |
| Teammates and handoffs | the member's handoff rules, and who those members are | team files |
| Reporting | the report blocks below; "do not edit the plan" | fixed |

Left out on purpose: the rest of the plan, other members, history, the
changelog, other roles' instructions. `myteam context <task>` prints exactly
what an agent would get, with a token estimate (about 600 tokens for a
typical task). The agent CLIs also read their own instruction files
(`CLAUDE.md`, `AGENTS.md`) from the work directory; `isolate: true` for the
claude runner adds `--bare`, which skips them, memory and hooks.

The agent ends its final message with:

```
<task_status>done | review | in_progress | blocked</task_status>
<work_summary>What was done, decisions, files.</work_summary>
<handoff to="tester">What they should do and deliver.</handoff>   (any number)
<blocker on="^schema">...</blocker>                     (when blocked; one per blocker)
<blocker to="lead">...</blocker>   to="customer"   to="operator"
<finding>A tool's gap the agent works without.</finding>  (any number; blocks nothing)
```

myteam then sets the status, records blockers (see *Blockers and
failures*), adds each finding as an `@operator` task tagged `#finding` next
to the task (which it does not block; myorg escalates it like any operator
task), and adds each handoff as a new task right after the original,
assigned to the recipient and depending on the original.

## The task round (`direct`)

`Loop::iterate()` (`include/myteam/loop.hpp`):

1. A plan that does not validate goes to the team's `repairer` (up to 5
   attempts; then a human is needed, noted in the changelog).
2. Ready tasks assigned to members are dispatched, each in its member's
   unit (`unit:`). Unassigned work is the coach's to assign.
3. Each task gets a `^id` to be tracked by and `[~]` while it runs.
4. The report is applied, the run logged to `CHANGELOG.md` and `run.json`.
5. The plan is validated again.

Models: `#p0` tasks use the member's premium model, `#p2`/`#p3` the economy
model, others the default.

## Blockers and failures

**A blocked task always says what blocks it, and links to what unblocks
it.** An agent reports each blocker as a `<blocker>` block; code writes it
into the plan:

```markdown
- [!] Upload results #p1 @developer ^upload
  - blocked-by: ^schema, ^s3-access
  - Blocked 2026-11-06 (@developer) by ^schema: needs the final schema
  - Blocked 2026-11-06 (@developer) by ^s3-access: write access to the test bucket
- [ ] Write access to the test bucket #p1 @operator ^s3-access
  - Needed by ^upload (@developer)
```

| Blocker | In the plan |
|---|---|
| `on="^id"`: a task already in the plan | `blocked-by: ^id` |
| `to="member"`: work or a decision of a teammate | a new task for that member |
| `to="customer"`: only the customer can answer | a task for the coach tagged `#customer-question`; it answers it (`<answer>`), or asks the customer first (`<to_customer>`) |
| `to="coach"`: a decision only the coach can make | a task for the coach (in its digest under "Waiting for your decision") |
| `to="operator"`: outside the team (access, credentials, a tool, a service) | a task for `@operator`, the person running myteam: listed under "Needs you" in `myteam status`, finished with `myteam done TASK "note"` |

New blocker tasks go right after the blocked task, so a blocker reads where
it happens. `@operator` and `@customer` are reserved handles; operator and
customer-question tasks are never dispatched to the team.

**Unblocking is code.** At the start of every round, and right when a
blocker task finishes, every `[!]` task whose `blocked-by` tasks are all
finished goes back to `[ ]` with a note saying what resolved it (the
finishing agent's summary, the PO's answer, the operator's note). The next
agent on the task sees the resolution: its context shows the notes of the
finished blockers. `myteam blockers` lists every blocked task, what it waits
for and who has it.

What code cannot settle is the coach's: its digest lists every blocked task
with what it waits for.

**Run failures** are classified from the CLI's exit code, error, stop reason
and stderr:

| Failure | What happens |
|---|---|
| not available (missing CLI, unknown runner) | the task goes back to its status; the runner is skipped for the rest of the round; a human is needed |
| auth (not logged in, bad key) | the same |
| rate limit, overload, quota | the task goes back to its status; the runner is skipped this round |
| limit (turns, cost, time) | a note; the task stays `[~]`, and the next run resumes the CLI session ("continue where you stopped") |
| crash, or no `<task_status>` | a note; after `max_failures` (3) in a row the task is blocked on an `@operator` task naming the error |

Run records (`run.json`) keep the failure kind, so consecutive failures are
counted across rounds.

## AgentRunner

```cpp
struct RunRequest {
    std::string member, model, prompt;
    std::filesystem::path workdir, run_dir;
    Permission permission;                    // ReadOnly | EditFiles | Full
    std::optional<std::string> resume_session;
    int max_turns; double max_cost_usd; int timeout_seconds;
    std::vector<std::string> deny_paths;      // .myteam/customer
    std::optional<McpServerSpec> mcp;         // the plan tools
};

class AgentRunner {
public:
    virtual std::string id() const = 0;       // "claude", "codex", "vibe"
    virtual RunResult run(const RunRequest&, const EventSink& = {});
    // per backend: the command line and the stream parser
    virtual std::vector<std::string> command(const RunRequest&) const = 0;
    virtual void parse_line(std::string_view, StreamState&, const EventSink&) const = 0;
    virtual void finish(const RunRequest&, StreamState&) const;
};
```

Runners turn each CLI's stream into common `AgentEvent`s (text, tool call,
tool result, usage, error) and a `RunResult` (final text, session id, usage,
cost, tool calls). The process runs in its own process group with a timeout;
the prompt goes through an in-memory file, so long prompts never block.

| Runner | Invocation | Permission: read-only / edit / full | Model | Resume |
|---|---|---|---|---|
| claude | `claude -p --output-format stream-json --verbose` (prompt on stdin) | `--permission-mode plan` / `acceptEdits` / `bypassPermissions` | `--model` | `--resume` |
| codex | `codex exec --json --skip-git-repo-check --cd <dir> ... -` | `--sandbox read-only` / `workspace-write` / `--dangerously-bypass-approvals-and-sandbox` | `--model` | `exec ... resume <id>` |
| vibe | `vibe --prompt-file <f> --output streaming --output-dir <run>/vibe --trust` | `--enabled-tools read_file,grep,glob` / `--agent accept-edits` / `--auto-approve` | `VIBE_ACTIVE_MODEL` | `--resume` |

Commands: with edit permission an agent may run shell commands without a
prompt (there is nobody to answer one): `commands:` in `config.yaml` is `all`
(the default), `none`, or a list of patterns (`["pytest:*", "python3 -m
pytest:*"]`); claude gets them as `--allowedTools Bash` or `Bash(<pattern>)`.
Without them an agent cannot run the tests it writes. Codex's `workspace-write` sandbox allows commands itself.

A member whose premium or economy runner is not installed works on its
standard model; runs that never started (no CLI, no login, a rate limit) do
not count against the budget.

MCP: claude gets `--mcp-config <json> --allowedTools mcp__myteam`, codex
`-c mcp_servers.myteam.command=... -c mcp_servers.myteam.args=[...]` (not
checked against a real codex yet); vibe has no MCP option here and reports
in blocks only.

The claude and codex stream formats are the documented ones; Vibe's
streaming format is read leniently, and its `export.json` (usage, cost, stop
reason, session) is authoritative. All three should be checked against a
real run of the installed versions.

Later options: an OpenAI-compatible HTTP runner (libcurl) for tool-less calls
and local models (Ollama, llama.cpp server, vLLM), and an ACP runner. Vibe
ships `vibe-acp`, and adapters exist for Claude Code and Codex, so ACP could
eventually replace the three subprocess adapters with one protocol.

A runner whose CLI cannot start here (not installed, or a script whose
interpreter is missing, such as a uv tool installed on a toolbox's host) is
found before a run is planned (`start_problem`, `usable_model`): the member's
standard model is used, the context and tools follow it, and the reason is
said once per command, not once per member. A run that still fails as not
available falls back the same way. The project's virtualenv (`.venv/`,
`venv/`, when its python runs here) is on every agent's `PATH`, as if
activated.

## Safeguards

Three layers keep agents from doing harm, and make what they do reversible
(`include/myteam/guard.hpp`, `checkpoint.hpp`, `sandbox.hpp`):

| | claude | codex | vibe |
|---|---|---|---|
| 1. the guard, before each action | hook | (its own sandbox) | none |
| 2. checkpoints, after each run | yes | yes | yes |
| 3. the sandbox, around the process | bwrap | bwrap (around its own) | bwrap |

The guard explains refusals to the agent; the sandbox cannot be argued
with, also by a script the guard cannot see into; checkpoints catch what is
allowed but wrong (a deleted test) and make every run undoable.

**Before each action** (`myteam guard`). Claude runs a PreToolUse hook for
every `Bash`, `Edit`, `Write`, `MultiEdit`, `NotebookEdit`, `Read`, `Grep` and
`Glob` call (`--settings`, set by myteam for task runs and the
Gatekeeper's checks). The guard parses the command (`;`, `&&`, `|`, `$(...)`,
backticks, `sh -c`, `eval`, `xargs`, `cd`, redirections) and refuses, with a
reason the agent reads and an invitation to ask the operator instead:

- writes outside the project (except the temp directory and
  `guard.allow_paths`) and to protected paths: the plan, the backlog, the
  changelog, `.myteam/` (except the project brief, `brief:`, which the
  team keeps up to date; `!glob` entries are such exceptions), `.git/`, CI
  workflows, `.env*`, keys (`guard.protect` adds more, `guard.unprotect` takes
  out what the team owns, such as CI, but never the plan, the changelog,
  `.git/` or `.myteam/`; the sandbox follows both);
- recursive deletes of the project, home or `/`; `xargs rm -r`; `shred`;
- `git push`, `reset --hard`, `clean`, `rebase`, `commit --amend`, switching
  branches, `checkout`/`restore` of everything, deleting branches, global
  config, remotes, git in another repository;
- `sudo` and friends, system package managers, global installs (`pip`
  outside a virtualenv, `npm -g`, `cargo install`, ...), service and disk
  commands, `dd` to devices, fork bombs;
- piping a download into a shell or interpreter, and sending data out with
  `curl`/`wget` (except to localhost);
- reading credentials (`~/.ssh`, `~/.aws`, `~/.gnupg`, `gh`, `docker`, ...).

`guard.allow_commands` and `guard.deny_commands` (regexes) override and add.
Refusals go to the run's `guard.jsonl`, the changelog and `run.json`; three in
one run are flagged in the run's record (and the coach's news). `myteam guard --check "COMMAND"` shows
what the guard would say. Codex runs in its own sandbox; Vibe gets no hook.

**After each run** (checkpoints). Before a task run myteam snapshots the
work tree into a commit under `refs/myteam/checkpoints/` with its own index
(the user's index, branch and HEAD stay as they are; `.myteam/runs/` is left
out), and again after it. A project without git gets a private repository,
`.myteam/checkpoints.git`, with the project as its work tree (the project is
not made a repository; `.myteam/`, virtualenvs and build output are left
out). The difference is reviewed:

- reverted, and the task cannot be done: protected paths changed, tests
  deleted (or moved out of the tests), secrets added (private keys, cloud
  and API tokens);
- flagged, and the task cannot be done: tests skipped or disabled;
- flagged: more than `guard.max_files` (40) files or `guard.max_lines` (3000)
  lines changed, binary files added.

A run reported done whose changes were reverted or held stays `[~]` with a
"Guard" note saying why, which the coach sees. `run/guard.json`
keeps the snapshots; `myteam undo RUN` (or `^task` for its latest run) puts
back what the run changed, file by file, keeping files changed since, and
sets the task back to `[ ]`.

**Around the process** (the sandbox; `sandbox: auto`, the default, uses
bubblewrap when it is installed and works; `bwrap` requires it; `none` turns
it off). Every agent CLI, of every runner, runs with the
system read-only and a private `/tmp`; the project writable except the plan,
the backlog, the changelog, `.git/`, `.myteam/` (but the project brief, the
team's to keep up to date, and the run's own directory,
where the runner, the MCP server and the guard write) and CI; the CLI's own
state writable (`~/.claude`, `~/.claude.json`, `~/.codex`, `~/.vibe`,
`~/.cache`, `~/.local/state`), package managers' download caches (`~/.npm`,
`~/.cargo/registry`, `~/go/pkg/mod`, `~/.gradle/caches`, `~/.m2/repository`)
and `sandbox_writable:` paths; credentials
hidden (`~/.ssh`, `~/.aws`, `~/.gnupg`, `gh`, `docker`, `kube`, `gcloud`,
`.netrc`, `.git-credentials`, ...); the network shared (the CLIs need their
APIs; `sandbox_network: false` cuts it). With `.git/` read-only an agent
cannot commit itself; it commits through the `git_commit` tool (below).

**Commits** (`commits: tool`, the default; `off`). In a git repository, with
checkpoints on, a task run's MCP server has `git_commit` (message; paths
optional). The server runs in the sandbox too, so it does not commit: it
writes the request to `<run>/commits/N.request.json` and waits. A
`CommitBroker` thread of the loop, outside the sandbox, answers it with
`commit_run_work` (`include/myteam/checkpoint.hpp`, `commits.hpp`):

- only the run's own work: what changed since the run's checkpoint and is
  not committed yet (the operator's uncommitted changes from before the run
  stay theirs; a named path the run did not change is refused);
- protected paths are left out (refused when named): myteam commits nothing
  of the plan, `.myteam/`, CI or secrets;
- a deleted test, an added secret or a skipped test refuses the commit, with
  the reason, so the agent fixes it or names the other paths;
- on the current branch only: not on a detached HEAD, not during a merge,
  rebase or cherry-pick; built in an index of its own (`commit-tree`,
  `update-ref` with the old value), then the user's index is synced for the
  committed paths;
- the member is the author (`Developer <developer@myteam.invalid>`), the operator (git's
  `user.name`) the committer; trailers `Myteam-Task:` and `Myteam-Run:`;
- the project's hooks run (`commit_hooks: true`, `commit_hook_timeout: 300`):
  `pre-commit`, `prepare-commit-msg` and `commit-msg` can refuse (the agent
  gets the hook's output) or change the message, `post-commit` runs after.
  They run in the sandbox, since `core.hooksPath` may point into the project,
  which agents write. They run on a checkout of exactly what is committed
  (`<run>/commits/c.../tree`, with `node_modules`, `.venv` linked in), not on
  the work tree: the agent's other changes are not "unstaged" to them, and a
  `pre-commit`-style stash cannot touch the work tree. Git's object store is
  read-only there, so they get one of their own over it
  (`GIT_OBJECT_DIRECTORY` plus alternates): what a formatter stages is taken
  into the commit (only the commit's own files; anything else refuses it),
  its objects are copied in, and the fixed files are put in the work tree
  where the agent's file is unchanged.

The guard refuses `git commit` and `git add` with a pointer to the tool, and
`git push` stays the operator's. The run record, `guard.json` and the
changelog (`- Commit: ...`) list the commits; `myteam undo` puts the files
back but leaves the commits, and prints the `git revert` to run. A refused
push becomes an `@operator` task; a deleted failing test is put back and the
task stays in progress; in the sandbox, a script writing to the home directory
fails with "read-only file system" while the MCP tools work.

## Services

A project's containers (a database, a proxy) are myteam's to run, outside
the sandbox (`include/myteam/services.hpp`; `services:` in `config.yaml`,
which agents cannot change). Each is a container `myteam-<project>-<name>`
in the host's podman (through its socket when there is one: a toolbox's
own podman often cannot run containers), labelled, with a hash of its
definition: `myteam run` starts what is missing, starts what stopped and
recreates what changed, before every round. Only what the operator wrote
runs: ports bind to 127.0.0.1, files mount read-only and only from inside
the project (not `.myteam/` or `.git/`), a `data:` path is a named volume,
`network: host` is the one other network.

Agents get each service's `agent_env` (`DATABASE_URL`, ...) in their
environment and a Services section in their context, and, through the MCP
server (`--services`), `service_status`, `service_logs`, `service_restart`
(after they change a file the service reads, which is theirs) and
`service_reset` (its data removed). The MCP server is in the sandbox, so
these go to the run's broker outside it, as commits do. The guard refuses
`podman` and `docker` with a pointer to the services: the podman socket
would be the whole account's (its toolboxes, and mounts of the home), so it
never enters the sandbox. A new service, or a change to one, is the
operator's, asked for as a blocker. `myteam services [status | up | down
[--purge] | logs NAME | restart NAME | reset NAME]` and `myteam json
services` for the operator and the UI.

## The MCP server

`myteam mcp --member H --task ID --run-dir DIR` is an MCP server on stdio
(`include/myteam/mcp.hpp`). `myteam run` starts one per task run for the
runners that support it (`mcp: true` in `config.yaml`, the default), and
the context tells the agent about the tools.

| Tool | |
|---|---|
| `plan_get_task` | the task (or another) as `describe_task` gives it, criteria numbered |
| `plan_list_tasks` | tasks, one line each, by status, assignee, text |
| `plan_team` | handles, names, roles; `@operator`; the customer through the PO |
| `plan_report_status` | `done`, `review`, `in_progress`, `blocked`, with the summary |
| `plan_handoff` | a task for a teammate (after the sender's), or for another team of the org or `operator` (neither waits for the other) |
| `plan_report_blocker` | one blocker: `on` a task, or `to` a teammate, `customer`, `operator` |
| `plan_check_criterion` | a criterion verified, failed or skipped, with evidence (for skipped, why) |
| `plan_report_finding` | a gap outside the team for `@operator` that does not block the task |
| `plan_report_org_issue` | feedback for myteam |
| `git_commit` | commit the run's work (with `--commits`; see Safeguards) |

Reading tools answer from the plan as it is. Reporting tools check their
input (a known teammate, a task in the plan, a criterion the task has, one
of `on`/`to`) and answer with an error the agent can act on ("no teammate
@ghost. The team: @lead, @developer, ..."), then record into
`<run>/mcp-report.json`. When the run ends the loop merges that with the
final message's blocks (the tools' status and summary win; handoffs,
blockers, criteria and issues are joined) and applies both as one validated
edit. So the plan changes once per run, and a failed run changes nothing.
The blocks remain for runners without MCP, and as a fallback.

Reading tools are annotated `readOnlyHint`, so Claude allows them in plan
mode (read-only runs); reporting tools are refused there, and read-only runs
(the Gatekeeper's acceptance) report in blocks. There is no tool listing the
whole plan: an agent's task, its dependencies and what waits for it are in its
context (`plan_get_task` reads one task).

Agents also report criteria in blocks:
`<criterion n="1" status="verified">How it was checked.</criterion>`; the
criterion becomes `(x)` or `(!)` with an `evidence:` line. `status="skipped"`
(it could not be checked yet: an input not delivered) leaves it `( )`, never
a pass, with `evidence: "skipped: <why>"`.

## Teams

```
plugins/teams/software/
  team.md              front matter: id, name, coach, gatekeeper, product_owner, repairer,
                       members, models; body: the team overview
  members/developer.md      front matter: name, role, summary, unit, skills, models,
                       handoffs; body: the role prompt
```

```markdown
---
name: Developer
role: developer
summary: implements features and fixes bugs, with tests
skills: [implementation, refactoring, testing]
models:                      # optional; else the team's
  default: claude:sonnet
  premium: claude:opus
  economy: vibe
handoffs:
  - when: the implementation is complete and needs verifying
    to: tester
    note: QA and verification flow
---

Deliver focused, well-structured changes with tests that cover them.
```

```yaml
# team.md front matter
id: software
name: Software
coach: coach                  # methodology coach: decides, assigns; the customer's contact
gatekeeper: gatekeeper        # checks the coach's decisions and every piece of work
product_owner: lead           # the customer's counterpart in `direct` (the coach, when there is one)
repairer: developer           # repairs a broken plan
members: [lead, developer, tester, ...]   # order; files not listed follow
models: {default: claude:sonnet, premium: claude:opus, economy: vibe}
```

Teams are found in `<project>/.myteam/teams`, `$MYTEAM_TEAMS`,
`~/.config/myteam/teams`, then the source tree's `plugins/teams`. A project
overrides any team file with the same path under `.myteam/team/` and can add
members there, so a team can change in one project without changing
everywhere; promoting an overlay back to the base team is a deliberate user
action. A member's `unit:` is a directory of the project with a myteam config
of its own (myorg writes it): the member's runs work there, with that
config's brief, rules, readable and writable paths, hidden directories, web
list and guard.

## The coach (`methodology: coach`)

`include/myteam/coach.hpp`. Scrum is for people; a crew of agents needs less
ceremony and more checking. A flat crew:

| Who | Model | Does |
|---|---|---|
| the coach | the most capable (Fable) | decides what the crew works on, who does it, what counts as done; the customer's only contact; curates skills; no tools, one answer per decision |
| the Gatekeeper | the best that is not the coach's (Opus) | the coach's skeptical right arm: reviews its plan decisions (may object once), checks every piece of work in the expert's unit before it counts |
| the experts | Sonnet | persistent people (a role prompt and a memory), each in its unit (`unit:`), behind the unit's boundaries |

A step (`myteam run`): the ready expert tasks run (the task round, at most
`coach.tasks_per_step`); the Gatekeeper checks what is in review
(`acceptances_per_step`), accepting it or sending it back with what is
missing (after `max_rejections`, the coach decides); then, when there is news
(outcomes, verdicts, council views, a customer message, a task waiting for
the coach, an objection) or the crew is idle, the coach decides; the
Gatekeeper reviews its plan decisions; code applies them; the council's
questions are answered. A rate limit ends the step; `myorg run` waits it out.

**The coach never reads files.** Code builds its digest (`myteam digest`
prints it): the goal, the crew and its load, the news since its last
decisions, the customer's new messages, what waits for it and for the
operator, the open work in one line each (review first, then by priority,
at most `digest_tasks`), the skills, its own notes. Its answer is blocks:

| Block | Applied |
|---|---|
| `<task to id priority depends>` title, notes, `accept:` lines | a task in the plan's Work section |
| `<change id status to priority depends>` why | the task's status, assignee, priority, dependencies; a note |
| `<answer id>` | a note; a decision waiting for the coach is done |
| `<ask to topic>` | the council: one answer each from the experts named, the Gatekeeper last (seeing the others) |
| `<coaching to>` | the "From the coach" section of the expert's memory |
| `<skill name triggers members>` | `.myteam/skills/<slug>/SKILL.md`, committed in git at once |
| `<notes>` | the coach's own memory, for its next decisions |
| `<to_customer>`, `<goal>`, `<milestone>` | the conversation; the plan's `goal:`; a milestone (myorg commits and tags it) |
| `<overrule>` | the held decisions apply without a second review, on record |

The Gatekeeper reviews every batch that changes the plan, the crew's memory
or skills, the goal, or claims a milestone. An objection holds the batch
(only the coach's notes and questions apply); the coach sees the objection
and its held decisions, and revises or overrules. A second objection to a
revision is recorded, not binding: the Gatekeeper may block once.

**Acceptance.** An expert's "done" sets the task to review with the
expert's summary as a note ("Done by @x ...: ..."). The Gatekeeper's run is
in the expert's unit, read-only (plan mode: no file writes; it may run
commands: tests, checks, the tools' CLIs, under the guard), with the task's
context and its own instructions; it ends with `<criterion>` lines and
`<verdict status="accept|reject">`. A task tagged `#no-gate` skips it.

**Single answers are cheap.** The coach, the Gatekeeper's review and the
council are talk runs (`RunRequest::talk`): no tools, no MCP servers, no
skills or settings, a system prompt of their own. Measured with Claude Code:
about 0.6k tokens of overhead against 19.5k for a default run.

**Learning.** A `<lesson>` (or `plan_report_lesson`) goes into the
member's memory (`memory.hpp`: `.myteam/memory/<handle>.md`, the coach's
coaching and the newest lessons, in every context of the member). The
coach sees the lessons in its news; one worth keeping for everyone becomes a
skill; one that is a workaround for a tool's gap becomes a task for the
tool's team. Skills are the tools' backlog: a skill the tool absorbed is
deleted.

**The customer** talks to the coach only: `myteam say "..."` appends to
`.myteam/customer/conversation.md`; the coach's `<to_customer>` answers there.

State: `.myteam/coach/state.json` (the news not yet seen, what the coach
saw of the conversation, a held objection, counts and the cost so far);
`.myteam/council/` (each council, its question and views).

## Feedback for myteam

Agents report what in the way the crew works got in their way
(`<org_issue>`, `plan_report_org_issue`); `.myteam/feedback.md` collects
them, merged by subject (`myteam feedback`, `--export` for a digest).

## Milestones

1. ✓ CMake project with the plan parser (tasks/); its Markdown dialect; plan
   access and validated line edits; the task round and allocation (`direct`).
2. ✓ Team loader (Markdown + front matter) and project overlays; the bundled
   teams; the clean context.
3. ✓ `AgentRunner` with claude, codex and vibe backends; run records.
4. ✓ The customer and the PO: threads, `product.md`, `requests.md`,
   `myteam talk` / `ask` / `threads`, the PO's blocks, the boundary.
5. ✓ Blockers and failures: blocker tasks, automatic unblocking, operator
   tasks, team questions through the PO, failure classes; ASCII criteria.
6. ✓ Feedback for myteam: `<org_issue>`, `.myteam/feedback.md`, `myteam feedback`.
7. ✓ Flags in the loop and the steward round (the `direct` Housekeeper),
   with the stand-up's blocker review (taken out in 13).
8. ✓ MCP server with the `plan_*` tools; criteria checked by agents.
9. ✓ Ceremony engine and proposals; Scrum with all five ceremonies, the PO's
   planning digest and the customer's OK, the Scrum Master.
10. ✓ Review demos, `review.json` and the blog (`myteam blog init` / `draft` / `build`).
11. ✓ `myteam json` and the myrcp plugin (with the Customer view); a JSON
    interface in place of Python bindings.
12. ✓ myorg: units (teams in git submodules) in step, ORG.md with Team
    Topologies types and interactions, org tasks to unit backlogs and back,
    requests between units, escalations, the maintainer's myteam built and
    adopted (MYORG.md).
13. ✓ The coach: Scrum, the steward, the product owner's desk,
    proposals, allocation, the blog, epics and `myteam json` taken out; the
    coach, the Gatekeeper, the council, members' memory and lessons, units as
    members' environments, talk runs, slim workers, the org's plan hidden
    from the units; myorg reduced to ORG.md, the units' boundaries and a
    tick (MYORG.md).
