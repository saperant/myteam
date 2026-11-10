# myteam in the myrcp workbench

`myteam-ui` is myteam's and myorg's user interface in the
[myrcp](https://github.com/saperant/myrcp) workbench (myrcp from PyPI):

```sh
pip install ./ui          # or: pip install -e ui, from a checkout with a build/
myteam-ui                 # in a myteam project or an org; Ctrl+P: Perspective: myteam / myorg
MYRCP_GTK=1 myteam-ui     # the GTK workbench (myrcp-gtk, from a myrcp source build)
```

## myteam

Watch a crew ([myteam](../README.md)'s coach methodology) work, and be its customer
and operator, from the workbench: the coach decides, the Gatekeeper checks every
decision and every piece of work, the experts work in their units.

Start `myteam-ui` in a myteam project (a directory with `.myteam/`, or below one) and
choose **Perspective: myteam** (Ctrl+P). The plugin needs the `myteam` executable: on
`$PATH`, `$MYTEAM`, the preference *myteam command*, or the `build/` of the checkout an
editable install comes from. The preference *Project* (or `$MYTEAM_PROJECT`) picks another project.

| View | Keys |
|---|---|
| **Overview**: the goal, the coach's state (decisions, cost, news waiting, the Gatekeeper's objection), counts, what needs you, who is running now, the latest runs | `r` a step, `R` five, `t` talk to the coach, `d` what the coach reads |
| **Plan**: the tasks as a tree by section, with status, assignee, priority, estimate, dependencies and criteria | Enter: the line in the editor, `d` finish an `@operator` task, `D` decline it with the reason, `h` hide finished |
| **Customer**: you and the coach, in order; what it has not read yet | Enter sends (`myteam say`), Ctrl+R a step now |
| **Coach**: the coach's desk: the Gatekeeper's objection and the held decisions, the news waiting for the coach, its notes, its decisions and their reviews | Enter: the run, `d` the digest, `r` a step, `c` the councils |
| **Council**: the coach's questions to the experts, and each one's view (the Gatekeeper's last) | Enter: the council's file |
| **Crew** (side): each member's role (coach, Gatekeeper, expert), unit, what they do now, open and in-review tasks, lessons learned, runs, cost; the selection shows the coach's coaching and the newest lessons | Enter: their runs, `m` their memory, `e` their role prompt |
| **Runs**: every run (tasks, checks, the coach, reviews, councils) with result, time, cost, commits, safeguard notes | Enter: the run, `k` kind, `u` undo a task run |
| **Run**: one run, live: report, plan changes, safeguards, commits, stderr, and what the agent did | `p` the prompt, `o` the final message, Enter on a commit: `git show` |
| **Skills**: what the crew learned to do: for whom, the words that call for each, where it came from | Enter: the skill in the editor |
| **Feedback for myteam**: what in the way the crew works got in the experts' way | |
| **Activity** (bottom): the changelog, newest first | Enter: the entry |

The palette has the rest: **myteam: Run a step**, **myteam: What the coach reads**, and one
command per view.

How it works: the views read `myteam json` (see myteam's `include/myteam/inspect.hpp`)
through one model per project, which reloads when the files myteam writes change, so a
step started anywhere (here, in a shell, `myorg run` in tmux) shows up as it happens. Agent
work runs in a Console, where its live output stays; quick commands (say, done, undo) run
as jobs. Tests: `ui/tests/test_myteam.py` (skipped without a built myteam).

## myorg

Watch one crew work across an org's units ([myorg](../MYORG.md)), and run
it, from the workbench.

Start `myteam-ui` in an org (a directory with `ORG.md` and `.myorg/`, or in one of its
units) and choose **Perspective: myorg** (Ctrl+P). The plugin needs the `myorg`
executable: on `$PATH`, `$MYORG`, the preference *myorg command*, or the `build/` of the
checkout an editable install comes from. The preference *Org* (or `$MYORG_DIR`) picks another org.

| View | Keys |
|---|---|
| **Org**: the milestone the crew works toward, ticks, the coach's decisions and cost, what the last tick waits for; every unit with its type, experts, purpose, what it sees and how much is hidden from it; the org's rules | `t` one tick, `r` run until it needs you, `x` stop after this tick, `s` sync, `e` edit ORG.md, `w` the crew's work (the myteam views), Enter: the unit's brief |
| **Topology**: what each unit's experts see, write and may not see, and the interactions as they are now (collaboration, x-as-a-service, facilitating; until which milestone); what the selected unit is told about the others | `e` edit ORG.md |
| **Units** (side): the org and its units, with their experts | Enter: the org in the myteam views, a unit's brief |
| **Org activity** (bottom): every tick myorg ran: its milestone, its cost, what happened | |

The palette has the rest: **myorg: One tick**, **myorg: Run until it needs me**,
**myorg: Stop after this tick**, **myorg: Sync with ORG.md**, **myorg: Talk to the coach**
(the myteam Customer view on the org), **myorg: The crew's work**.

How it works: the views read `myorg json overview` and `myorg json log` (see myteam's
`include/myorg/org.hpp`) through one model per org, which reloads when ORG.md, myorg's
state, the tick log, the coach's state or a unit's config change. Running the org is
`myorg run` in a Console. Tests: `ui/tests/test_myorg.py` (skipped without a built
myorg).
