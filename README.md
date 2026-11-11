# myteam

An AI toy experiment: a team of AI agents working through a plan written in
Markdown. Each member of a team (a role, a prompt, models, handoff rules) runs
as an agent CLI — Claude Code, Codex or Mistral Vibe — and starts from a clean
context: its role, the project brief, its task and only the plan around it,
and how to report. myteam updates the plan from the agents' reports.

> An experiment, shared as is. Agents cost money and change files: try it on a
> throwaway repository, with limits set (`limits:` in the configuration).

In C++20. Design: [ARCHITECTURE.md](ARCHITECTURE.md).

## Build

```sh
cmake -S . -B build -G Ninja      # builds tasks/ (the plan parser) in
cmake --build build
ctest --test-dir build
```

Needs a C++20 compiler and GoogleTest for the tests (`-DMYTEAM_BUILD_TESTS=OFF`
without). `tasks/` is the plan parser (the TaskML library, with its own tests, Python
bindings and a Gantt tool), built in statically.

## Use

```sh
myteam init -t software            # .myteam/config.yaml, .myteam/project.md, PLAN.md
$EDITOR .myteam/project.md        # the brief every agent gets
$EDITOR PLAN.md                   # tasks: - [ ] Build the API #p1 @developer ^api
                                  #   criteria: - ( ) Returns JSON   (x) done   (!) failed
myteam check                      # validate the plan and the team
myteam status                     # ready, waiting and blocked work
myteam context ^api               # exactly what @developer would be told
myteam run --dry-run              # who would get what, with which model
myteam run                        # one round of work
myteam run -i -n 10               # one task at a time, confirming each prompt
myteam runners                    # which agent CLIs are installed
myteam convert old/PLAN.md        # a TaskML plan as Markdown
```

`-d DIR` works on another project; `-t TEAM` overrides the configured team;
`--runner codex` sends every task to one runner; `--task ^id` runs one task.

A round (`methodology: direct`) dispatches every ready task assigned to a
member (open, dependencies done, nothing blocking). Each agent ends with:

```
<task_status>done</task_status>
<work_summary>What was done.</work_summary>
<handoff to="tester">Test the API; report in test-report.md.</handoff>
```

and myteam sets the status, adds handoff tasks (depending on the original),
and logs the run to `CHANGELOG.md` and `.myteam/runs/`.

## When work is blocked

An agent that cannot continue names each blocker: a task in the plan, a
teammate, the coach (a decision; it asks the customer when only the customer can answer), or the operator (you:
access, credentials, tools). myteam writes it into the plan as a
`blocked-by:` link to a task for whoever unblocks it, with a note saying what
blocks, and unblocks the task itself when that task is done. No task keeps a
link to a completed or cancelled task: each step drops such links, adds a note
and a changelog entry for each, and moves a link to a task closed as a
duplicate over to the copy that is still open.

A task's dependents hear when it closes. Each open task that `depends:` on a
completed, cancelled or merged task gets one note, "Dependency ^x done" (or
cancelled, declined, merged into ^y), with the closing summary when there is
one. The same goes for the org's contracts: when the `version:` of
`interfaces/<name>/README.md` changes, each open task whose title, notes or
criteria name `interfaces/<name>` gets a note, and the coach's digest shows
those tasks. The first step only records the versions it finds.

No job is open twice for the same person. Every path that makes a task
(the coach, findings, blockers, handoffs) checks for an open task with the
same normalised title (lower case,
punctuation ignored) and the same assignee. If one exists, no new task is made:
the open one gets a "Filed again" note and is used instead. Twins already in a
plan are merged at each step. The task with work in it stays; each other copy
is closed as "Duplicate of ^kept", and the changelog gets one `^old -> ^kept`
line per merge.

```sh
myteam blockers                    # what is blocked, on what, who has it
myteam done ^s3-access "Key is in ~/.aws/credentials"   # finish an @operator task
myteam unblock --dry-run           # the dead blocker links a step would drop
myteam dedup --dry-run             # the twins a step would merge, ^old -> ^kept
```

Failed runs: a missing CLI or a login problem puts the task back and stops
that runner for the round; a rate limit just waits; a run stopped at a
limit continues in the same session next round; three crashes (or runs
without a status) in a row block the task on an `@operator` task.

## Commands agents may run

With `permission: edit` (the default) agents edit files in the project and
may run shell commands, so they can run the tests they write. Narrow it in
`.myteam/config.yaml` with `commands: ["pytest:*", "python3 -m pytest:*"]`,
or `commands: none`.

## Safeguards

Agents work behind three guards:

- **Before each action**, `myteam guard` checks every shell command and file
  write of a Claude agent and refuses what does harm: writing outside the
  project or to the plan, `.myteam/`, `.git/`, CI and secrets; `git push`,
  `reset --hard` and other history rewrites; `git stash` other than `list`
  and `show` (the stack is shared by every worktree); `sudo`, system and global
  installs; piping downloads into a shell; sending data out; reading
  credentials. The agent is told why, and how to ask you instead.
- **After each run** (in a git repository) myteam compares the work tree with
  a checkpoint taken before it: deleted tests, protected files and added
  secrets are put back, skipped tests and very large changes are flagged, and
  the task is not marked done. `myteam undo RUN` puts back a whole run. A
  project without git gets a private checkpoint repository in `.myteam/`.
- **Around the process**, every agent CLI (Claude, Codex, Vibe) runs in a
  bubblewrap sandbox when bwrap is installed: only the project is writable,
  the plan, `.git/` and `.myteam/` are read-only, credentials are hidden.
  No command or script an agent runs can change your system or home.

Agents commit through the `git_commit` tool: myteam, outside the sandbox,
checks the commit (only the agent's own changes, no protected files, deleted
tests, skipped tests or secrets) and makes it on the current branch, with the
member as author and you as committer. Your git hooks run on what is
committed, in the sandbox: a failing `pre-commit` refuses the commit and the
agent sees its output; a formatter's fixes go into the commit and the work
tree. Pushing stays yours. `commits: off` turns commits off, `commit_hooks:
false` the hooks.

```sh
myteam guard --check "git push --force"   # what the guard would say
myteam undo ^count                        # undo the task's latest run
```

```yaml
guard:
  deny_commands: ["\\bdocker\\b"]     # more to refuse (regexes)
  allow_commands: ["^git push origin feature/"]
  protect: ["migrations/**"]           # more protected paths
  unprotect: [".github/workflows/**"]  # the team owns CI (never the plan or .myteam/)
  allow_paths: ["/srv/scratch"]        # writable outside the project
  max_files: 40                        # a bigger run is flagged
  max_lines: 3000
  checkpoints: true
sandbox: auto                          # bwrap when it works | bwrap (required) | none
sandbox_network: true
sandbox_writable: []                   # more writable paths
commits: tool                          # agents commit via git_commit | off
commit_hooks: true                     # run your git hooks on their commits
```

## The coach

With `methodology: coach` (and a team with a `coach:` and a `gatekeeper:`,
like `plugins/teams/crew`), `myteam run` is a step of a flat crew:

- **the coach** (the most capable model) decides what the crew works on, who
  does it and what counts as done. You talk only to it. It never reads files:
  it reads a digest code builds (`myteam digest` prints it) and answers with
  decision blocks: tasks, changes, answers, council questions, coaching, skills,
  its own notes, a message to you, the goal, a milestone.
- **the Gatekeeper** (a different model) is its skeptical right arm. It reviews
  the coach's plan decisions before they apply (it may object once; the coach
  revises or overrules, on record), and it checks every piece of work an expert
  reports done, in the expert's unit, read-only, before it counts.
- **the experts** do the tasks, each in its unit (`unit:` in its member file),
  and keep a memory: what the coach coached them on, and the lessons they
  report (`<lesson>`). The coach turns lessons worth keeping into skills, and
  lessons that are workarounds for a tool into tasks for the tool's team.

```sh
myteam say "We need a todo app with a web UI and an API"   # you, to the coach
myteam run -n 10        # steps until the crew waits for you
myteam say              # the conversation
myteam digest           # what the coach reads at its next decision
myteam status           # the work, what needs you, the coach's cost so far
```

Settings (all optional):

```yaml
coach:
  tasks_per_step: 3        # expert task runs per step
  acceptances_per_step: 3  # the Gatekeeper's checks per step
  max_rejections: 2        # sent back this often: the coach decides
  digest_tasks: 150        # open tasks in the coach's digest
  memory_limit: 6000       # characters of a member's memory in its context
  acceptance_turns: 30     # the Gatekeeper's turns per check
  step_cost: 0             # USD a step may spend on checks (0: no limit)
```

## Tools for agents (MCP)

`myteam run` gives Claude and Codex agents an MCP server, `myteam mcp`, with
plan tools: `plan_get_task`, `plan_team`, and for reporting
`plan_report_status`, `plan_handoff`, `plan_report_blocker`,
`plan_check_criterion` (verified, failed, or skipped: it stays open with the
reason), `plan_report_finding` (a gap outside the team, for the operator,
that does not block the task), `plan_report_lesson` (into the member's
memory) and `plan_report_org_issue`, and `git_commit` in a git repository.
There is no tool that lists the plan: the task and what it needs are in the
agent's context. The reporting tools check
what the agent gives them (a teammate that exists, a task in the plan, a
criterion the task has) and say what is wrong; what they record is applied
to the plan when the run ends, together with the report blocks. Turn it off
with `mcp: false` in `.myteam/config.yaml`.

## Feedback for myteam

Agents can say when the way the team is organised gets in their way (the
context they got, the plan format, handoffs, tools, the process) with an
`<org_issue area="...">` block. myteam collects these in
`.myteam/feedback.md`, counting repeated reports, and the retrospective asks
for them. `myteam feedback` lists them; `myteam feedback --export -o FILE`
writes a digest for myteam's development.

## Services (a database, a proxy)

myteam runs the project's containers for the team, outside the sandbox, in
your podman (the host's, when you work in a toolbox):

```yaml
services:
  db:
    image: docker.io/library/postgres:16-alpine
    ports: ["127.0.0.1:5432:5432"]
    env: {POSTGRES_DB: app, POSTGRES_USER: app, POSTGRES_PASSWORD: dev}
    data: /var/lib/postgresql/data
    agent_env: {DATABASE_URL: "postgres://app:dev@127.0.0.1:5432/app"}
  proxy:
    image: docker.io/library/nginx:alpine
    network: host
    files: {/etc/nginx/conf.d/default.conf: deploy/nginx.conf}
```

`myteam run` keeps them running (recreated when you change them). Agents get
the addresses and tools to restart a service, read its logs or reset its
data; what runs (images, ports, mounts) stays yours. Ports bind to
127.0.0.1 only, files mount read-only from the project. `myteam services`
shows them; `up`, `down`, `logs NAME`, `reset NAME`.

## Watching the crew (myrcp)

`ui/` is myteam's and myorg's user interface in the
[myrcp](https://github.com/saperant/myrcp) workbench, with myrcp from PyPI
([ui/README.md](ui/README.md)):

```sh
pip install ./ui                  # the myteam-ui command (and myrcp)
myteam-ui                         # in the project; Ctrl+P: Perspective: myteam (or myorg)
```

They show the plan, the coach's desk (the Gatekeeper's objections and the
held decisions, the news it reads next, its notes), the councils, the crew with each
member's unit and memory, every run as it happens, and the conversation with the coach
(you are the customer). They read `myteam json` and `myorg json`, one JSON document per
call, for anything else that wants to watch:

```sh
myteam json overview          # the goal, counts, what needs you, the coach's state, who is running
myteam json coach             # the news waiting, a held objection, the coach's notes
myteam json events RUN        # a run's events, while it runs too
myorg json overview           # the units, what each sees, the interactions now
```

### Screenshots

A food bank's stock app (made up), a day into its first milestone: what needs you, who
is working now and the latest runs, with the crew at the side:

![The overview in the terminal workbench](docs/screenshots/rcp-overview.svg)

The coach's desk: the Gatekeeper objects, so the coach's decision is held; the news the
coach reads at its next decision, and its notes:

![The coach's desk](docs/screenshots/rcp-coach.svg)

The org in myorg's perspective: its units, their experts and what each sees:

![The org in the terminal workbench](docs/screenshots/rcp-org.svg)

The same in the GTK workbench (`MYRCP_GTK=1 myteam-ui`):

![The overview in the GTK workbench](docs/screenshots/gtk-overview.png)

![The org in the GTK workbench](docs/screenshots/gtk-org.png)

More: [the plan](docs/screenshots/rcp-plan.svg), [you and the coach](docs/screenshots/rcp-customer.svg),
[a run as it happens](docs/screenshots/rcp-run.svg), [the topology](docs/screenshots/rcp-topology.svg),
and in GTK [the plan](docs/screenshots/gtk-plan.png), [the coach's desk](docs/screenshots/gtk-coach.png)
and [the topology](docs/screenshots/gtk-topology.png). `tools/screenshots/make.sh` makes them
again, from a made-up org in a sandbox (no agent runs: the crew's work is written as myteam
would leave it).

## One crew across units (myorg)

For a product made of several repositories (an API, a web front end, a
command-line client, the tools they are built with), `myorg` runs one crew across an org's
units. Each unit is a git repository (a submodule of the org); its experts
work there and see only what its interactions in ORG.md open to them (Team
Topologies: collaboration, x-as-a-service, facilitating), so they meet at the
contracts in `interfaces/` as separate teams would. A tick: the units' views
from ORG.md, a coach step, and at a milestone a commit of every unit and the
org, tagged `milestone-N`. Design: [MYORG.md](MYORG.md).

```sh
myorg init -t crew --name suite && cd suite
myorg add api --purpose "the HTTP API"
myorg add web --purpose "the web front end"
myorg link api web --mode collaboration --until 1 --provider api
$EDITOR .myteam/team/members/apidev.md          # an expert: unit: api
myteam say -d . "We need a todo app: an API, a web UI and a CLI"
myorg run                                      # until the crew waits for you
myorg status
```

## Configuration

`.myteam/config.yaml`:

```yaml
team: software
methodology: direct        # direct (task rounds) | coach (the coach decides, the Gatekeeper checks)
permission: edit           # read-only | edit | full
limits: {timeout: 1800, max_turns: 0, max_cost: 0}
runners:
  claude: {command: claude, isolate: false, slim: true}   # isolate: --bare; slim: without your MCP servers and skills
  codex:  {command: codex}
  vibe:   {command: vibe}
```

## Teams

`plugins/teams/crew` (the coach, the Gatekeeper, an engineer: the start of a
crew), `plugins/teams/software` (lead, developer, tester, writer) and
`plugins/teams/research` (research and documentation: editor, researcher,
analyst, reviewer, writer). Your own teams go in `~/.config/myteam/teams/`
(or `$MYTEAM_TEAMS`, or a project's `.myteam/teams/`), which come first.
`myteam teams` lists them with their Team Topologies type (`topology:` in
team.md: stream-aligned, platform, complicated-subsystem, enabling). A team
is `team.md` plus `members/<handle>.md`, Markdown with YAML front matter: the
front matter holds the role, models and handoffs, the body is the role prompt.
Put your own teams in `~/.config/myteam/teams/`, `$MYTEAM_TEAMS` or the
project's `.myteam/teams/`; override a team's files for one project under
`.myteam/team/`.

Models are `runner:model`: `claude:opus`, `codex:gpt-5.2-codex`, `vibe` (Vibe's
configured model; others by alias through `VIBE_ACTIVE_MODEL`).

## Licence

MIT ([LICENSE](LICENSE)), `tasks/` included.
