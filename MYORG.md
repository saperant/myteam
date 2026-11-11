# myorg: one crew across an org's units

myorg runs one crew (myteam's `coach` methodology) across an organisation's
units: an API, the web front end that uses it, a command-line client, and the
tools they are built with. Code: `include/myorg/org.hpp`, `src/org/`,
`apps/myorg.cpp`. Tests: `tests/org_test.cpp`.

## Principles

1. **One crew, one plan.** The org is a myteam project. Its team is a crew: a
   coach who decides, a Gatekeeper who checks, and experts. Its plan is the one
   task list; every expert's tasks are in it, whatever their unit.
2. **Units are boundaries, not teams.** A unit is a repository and what its
   experts may see. An expert (`unit: api` in its member file) works in its
   unit's repository; it reads the contracts (`interfaces/`) and the units its
   interactions open to it, and nothing else. It never sees the org's plan,
   changelog or state: its task is in its prompt. So experts meet at the
   contracts and use each other's work and tools through their documented
   surfaces, as separate teams would. That is where the tools' gaps show: a
   fix has to be asked for, reproduced and released, not patched in passing.
3. **Team Topologies shapes the boundaries.** The interactions in ORG.md say
   what each unit's experts see: collaboration opens the other's repository,
   x-as-a-service only what the provider publishes, facilitating opens the
   helped unit to its helper. An interaction can end at a milestone
   (`until: N`). Cognitive load is the reason for a boundary, and for an
   agent it is a number: the context its work needs.
4. **The org's rules are the units' too.** ORG.md's `rules:` (for every unit)
   and each unit's own reach every expert in that unit with its brief, and
   nobody in the crew can change them.
5. **Milestones, not sprints.** The coach declares a milestone when the goal
   is met and the Gatekeeper agrees; myorg commits every unit and the org (with
   the units' revisions) and tags `milestone-N`. You say what is next.

## Layout

```
suite/                        the org: a git repository, a myteam project (methodology: coach)
  ORG.md                      units, interactions, rules; the body is the org's brief (the coach's)
  PLAN.md                     the crew's one task list
  interfaces/                 contracts between units (schemas, events, configuration)
  deliverables/               what the org delivers to the customer: one directory per unit
                              (renders, layouts, documentation, reports) and README.md, its index
  integration/                cross-repository checks (an expert's, in a unit that owns them)
  .myteam/                    the crew: config, team (or .myteam/teams/crew/), memory, skills, runs
  .myorg/config.yaml          myorg's settings
  .myorg/state.json           the milestone, ticks
  .myorg/log/ticks.jsonl      what each tick did
  api/  web/  cli/            units: git submodules; each has a .myteam/config.yaml (its boundaries,
                              written by myorg) and .myteam/project.md (its brief, the experts')
```

## ORG.md

```yaml
---
name: suite
deliverables: deliverables          # the customer's results (the default; none: no such directory)
rules:
  - "Nothing is installed: say what is missing in a blocker to the operator."
units:
  api:
    purpose: "the HTTP API"
    owns: [interfaces/schema]         # its experts write there
    reads: [~/src/toolkit]            # and read this, outside the org
    rules: ["The API is built with toolkit."]
  web: {purpose: "the web front end", owns: [interfaces/events]}
  cli: {purpose: "the command-line client"}
  toolkit:
    purpose: "the build tool api works in"
    type: platform
    writes: [~/src/toolkit-suite]     # a worktree of the tool it maintains
interactions:
  - {between: [api, web], mode: collaboration, until: 2, provider: api}
  - {between: [cli, api], mode: x-as-a-service, provider: api, via: [interfaces/schema]}
  - {between: [api, toolkit], mode: x-as-a-service, provider: toolkit}
---
# suite
The system, its parts, how they fit (the coach's brief).
```

`myorg sync` (and every tick) writes each unit's `.myteam/config.yaml` block
from this: `sandbox_hide` (the units it does not see), `readable` (the
interfaces, the units it sees, its `reads`), `writable` (its `owns`, its
`writes`), `rules`, and `notes` (who it works with, and how). The org's own
block makes the units read-only to the coach's and the Gatekeeper's runs.

### Deliverables

Each unit's experts write `deliverables/<unit>/` (and read the whole directory): what the
customer gets from the unit, such as renders, layouts and fabrication files, documentation for
the customer, and reports. The unit's note says so. They keep the sources in their own repository
and make the deliverables from them with a command they name, so the files can be made again, and
they name each file they add or change in their delivery, where the Gatekeeper checks it. The
experts commit only in their unit's repository, so myorg commits the directory: after each coach
step it refreshes `deliverables/README.md` (the index, per unit) and commits what changed under
`deliverables/`, and only that, as *Deliverables: <units>*. `myorg json` lists the files.
`deliverables: DIR` in ORG.md moves the directory; `deliverables: none` turns it off.

## A tick (`myorg run`)

1. The units' views from ORG.md, for the milestone the crew works toward.
2. A coach step on the org (`myteam run`'s step, in-process): the ready
   expert tasks, the Gatekeeper's checks, the coach's decisions and the
   council (ARCHITECTURE.md, *The coach*).
3. The deliverables: the index refreshed and the changes under `deliverables/` committed.
4. A milestone: commit, tag, and wait for you.

It goes on until the crew waits for you (an `@operator` task, nothing to do,
a milestone), `-n` ticks, or `.myorg/stop`. A rate limit is waited out
(`rate_limit_wait`, seconds).

## What needs you (`myorg watch`)

The crew goes on while something waits for a person, so a person has to be told, or
operator tasks, the coach's messages and accepted tool fixes wait all night while
`myorg run` keeps stepping. myorg says what needs you:

| Need | When |
|---|---|
| operator | an open `@operator` task |
| customer | the coach wrote to you last |
| release | a tool unit's (a unit with `writes:`) task the Gatekeeper accepted is not released: merge it, then `myorg released UNIT` |
| waiting | the last tick waits for you (idle, a milestone, ORG.md's problems) |
| stopped | no tick for `--stale` minutes (120 for the watch): nothing is running the org |

`myorg run` prints each new one (`NEEDS YOU: ...`) after its tick and runs `notify:` from
`.myorg/config.yaml` (`sh -c`, the text as `$1`, `MYORG_NEED_KIND/KEY/TEXT` set; e.g.
`notify-send myorg "$1"`). `myorg watch` waits until something new needs you, prints it,
runs `notify:` and exits: run it in the background next to `myorg run`, and its exit is the
alarm (`--follow` keeps going, a line per need, for a watcher reading lines). `myorg needs`
lists what needs you now. `.myorg/watch.json` keeps what was said to whom, and each tool
unit's releases.

## The tool teams

A tool the units work in (a build tool, a code generator) is a unit too, of type platform,
whose experts maintain it in a worktree (`writes:`) and react to what its
users report: findings (`<finding>` "toolkit: ...") become tasks for its
expert, and so do lessons the coach finds are workarounds for the tool.
Skills are the tools' backlog: a skill that only exists because a tool lacks
something is a task for the tool's team, and is deleted when the tool has it.

## Commands

```sh
myorg init -t TEAM [--name N]     # git, the crew's project, ORG.md, interfaces/, .myorg/
myorg add UNIT [--purpose P] [--type T] [--dir D]
myorg link A B --mode M [--provider U] [--via PATH] [--until N]
myorg sync [--dry-run]            # the units, and what each sees
myorg status                      # the milestone, units, interactions, the cost
myorg run [-n N] [--dry-run] [-q] # ticks until the crew waits for you
myorg log [-n N]                  # the ticks
myorg needs                       # what needs you now
myorg watch [--follow]            # wait for what needs you (keep it running beside myorg run)
myorg released UNIT               # a tool unit's accepted fixes are merged
myteam say -d ORG "..."           # you, to the coach
```
