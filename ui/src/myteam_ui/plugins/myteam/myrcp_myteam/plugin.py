"""myteam in the workbench: watch the crew work, and be its customer and operator.

The crew (myteam's coach methodology): the coach decides, the Gatekeeper checks every
decision and every piece of work, the experts work in their units. You talk to the coach.
The views read `myteam json` through one shared Model (model.py), which follows the
project's files, so whatever happens (a step in a console, `myteam run` or `myorg run` in a
shell or tmux) shows up here as it happens. Agent work runs in a Console (live output),
quick commands as jobs.
"""

from __future__ import annotations

import datetime as dt
import tempfile
import textwrap
import threading
import weakref
from pathlib import Path
from typing import Any

from myrcp import Context, Item, Job, Marker, Plugin, View, ui

from .model import Model, candidates, command, find_project, is_project, save_project

plugin = Plugin()

STATUS_STYLE = {
    "done": "ok",
    "review": "warning",
    "in_progress": "accent",
    "blocked": "error",
    "pending": "normal",
    "cancelled": "dim",
}
STATUS_WORD = {
    "done": "done",
    "review": "review",
    "in_progress": "in progress",
    "blocked": "blocked",
    "pending": "pending",
    "cancelled": "cancelled",
}
KIND_WORD = {"coach": "coach", "gatekeeper": "Gatekeeper", "expert": "expert"}
WRAP = 110


# ---------------------------------------------------------------------------------
# helpers


def shared(rcp: Context) -> Model:
    """The project's model; the first time, also what feeds the Problems view."""
    m = Model.get(rcp)
    if not getattr(m, "_markers_on", False):
        m._markers_on = True  # type: ignore[attr-defined]
        m.subscribe(lambda: update_markers(m))
    return m


def update_markers(m: Model) -> None:
    plan_path = Path(m.data.plan.get("path") or m.project / "PLAN.md")
    markers = [
        Marker(e["message"], plan_path, e.get("line") or 0, severity="error", source="myteam plan")
        for e in m.data.plan.get("errors", [])
    ]
    for r in m.data.runs[:50]:
        if r.get("hold"):
            markers.append(Marker(f"held by the safeguards: {r['what']} (@{r['member']})", severity="warning",
                                  source="myteam guard", view=f"myteam.run#{r['name']}"))
    if m.data.coach.get("objection"):
        markers.append(Marker("the Gatekeeper objects to the coach's decisions: " + m.data.coach["objection"][:200],
                              severity="info", source="myteam coach", view="myteam.coach"))
    m.rcp.set_markers("myteam", markers)


def member_name(m: Model, handle: str) -> str:
    mem = next((x for x in m.data.team.get("members", []) if x.get("handle") == handle), None)
    return mem.get("name", handle) if mem else handle


def parse_time(s: str) -> dt.datetime | None:
    s = (s or "").strip()
    if not s:
        return None
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            t = dt.datetime.strptime(s, fmt)
            # myteam's ISO times are UTC; the conversations' are local.
            return t.replace(tzinfo=dt.timezone.utc) if s.endswith("Z") else t.astimezone()
        except ValueError:
            continue
    return None


def ago(s: str) -> str:
    t = parse_time(s)
    if t is None:
        return s or ""
    secs = int((dt.datetime.now(dt.timezone.utc) - t).total_seconds())
    if secs < 60:
        return "just now"
    if secs < 3600:
        return f"{secs // 60} min ago"
    if secs < 86400:
        return f"{secs // 3600} h ago"
    return t.astimezone().strftime("%Y-%m-%d %H:%M")


def duration(a: str, b: str, running: bool = False) -> str:
    """From a to b, or to now while running; nothing when the end is not known."""
    if not b and not running:
        return ""
    ta, tb = parse_time(a), parse_time(b) if b else dt.datetime.now(dt.timezone.utc)
    if ta is None or tb is None:
        return ""
    secs = max(0, int((tb - ta).total_seconds()))
    return f"{secs // 60} min {secs % 60:02d} s" if secs >= 60 else f"{secs} s"


def money(x: Any) -> str:
    try:
        v = float(x or 0)
    except (TypeError, ValueError):
        return ""
    return f"${v:.2f}" if v else ""


def wrapped(text: str, style: str = "normal", indent: str = "") -> list[Any]:
    out: list[Any] = []
    for para in (text or "").splitlines() or [""]:
        if not para.strip():
            out.append("")
            continue
        for line in textwrap.wrap(para, WRAP - len(indent), replace_whitespace=False, drop_whitespace=True) or [""]:
            out.append([(indent + line, style)])
    return out


def run_result(r: dict[str, Any]) -> tuple[str, str]:
    """(text, style) for a run's result column."""
    state = r.get("state")
    if state == "running":
        return "running", "accent"
    if state == "interrupted":
        return "interrupted", "error"
    if r.get("hold"):
        return "held", "warning"
    if r.get("status"):
        st = r["status"]
        return st, STATUS_STYLE.get({"in_progress": "in_progress"}.get(st, st), "normal")
    if r.get("ok"):
        return "ok", "ok"
    f = r.get("failure") or "failed"
    return (f if f != "none" else "failed"), "error"


def background(rcp: Context, fn, done) -> None:
    """fn() in a thread, done(result, error) on the main thread (no job shown)."""

    def work() -> None:
        try:
            res, err = fn(), None
        except Exception as e:  # noqa: BLE001 - shown to the user
            res, err = None, e
        rcp.call_soon(lambda: done(res, err))

    threading.Thread(target=work, daemon=True).start()


def myteam_console(rcp: Context, m: Model, args: list[str], title: str) -> None:
    """Agent work: in a Console, where its live output stays."""
    rcp.run_in_console([m.exe, *args, "-d", str(m.project)], cwd=m.project, title=title, name=f"myteam {args[0]}")


def myteam_job(rcp: Context, m: Model, args: list[str], title: str, on_ok=None) -> None:
    """A quick myteam command as a job; what it said in the status bar."""

    def work(_j: Job) -> tuple[bool, str]:
        return command(m.exe, m.project, *args)

    def done(j: Job) -> None:
        ok, said = j.result if j.ok else (False, str(j.error))
        last = said.strip().splitlines()[-1] if said.strip() else ("done" if ok else "failed")
        rcp.status(f"{title}: {last}", "ok" if ok else "error")
        m.reload()
        if ok and on_ok:
            on_ok(said)

    rcp.run_job(title, work, on_done=done)


_views: weakref.WeakSet[TeamView] = weakref.WeakSet()


def choose_project(rcp: Context, path: str) -> None:
    """Show this project from now on (and next time, without a project in the cwd)."""
    p = Path(path)
    if not is_project(p):
        rcp.status(f"{p} is not a myteam project (no .myteam/)", "error")
        return
    save_project(p)
    for v in list(_views):
        v.rebind()
    rcp.status(f"myteam: {p}", "ok")


def project_picker(rcp: Context, here: Path) -> ui.Node:
    found = candidates()
    items = [ui.item(str(p), p.name, str(p.parent)) for p in found]
    return ui.vbox(
        ui.text([("No myteam project in ", "warning"), (str(here), "bold"),
                 (". Choose one (remembered), or start myrcp in a project:", "warning")], wrap=True),
        ui.listbox("projects", items, on_activate=lambda key: choose_project(rcp, key),
                   empty="No myteam projects found nearby: myteam init in a directory makes one.", grow=1),
        grow=1,
    )


class TeamView(View):
    """A view of the shared model: rendered again whenever it changes."""

    def setup(self) -> None:
        self.m = shared(self.rcp)
        self.m.subscribe(self.invalidate)
        _views.add(self)
        self.init()

    def rebind(self) -> None:
        """The project changed: follow the new one's model."""
        if find_project(self.rcp) == self.m.project:
            return
        self.dispose()
        self.m = shared(self.rcp)
        self.m.subscribe(self.invalidate)
        self.init()
        self.invalidate()

    def init(self) -> None:
        pass

    def dispose(self) -> None:
        self.m.unsubscribe(self.invalidate)

    @property
    def d(self):  # noqa: ANN201
        return self.m.data

    def banner(self) -> ui.Node | None:
        if not is_project(self.m.project):
            return project_picker(self.rcp, self.m.project)
        if not self.m.loaded:
            return ui.text(f"Loading {self.m.project}…", "dim")
        if self.d.error:
            return ui.text(f"myteam: {self.d.error}", "error", wrap=True)
        return None


# ---------------------------------------------------------------------------------
# palette commands (any view)


@plugin.command("myteam.show")
def show(rcp: Context) -> None:
    rcp.open_view("myteam.overview")


@plugin.command("myteam.project.choose")
def choose(rcp: Context) -> None:
    rcp.open_view("myteam.projects")


@plugin.view("myteam.projects")
class Projects(View):
    """Choose the project the views show."""

    def title(self) -> str:
        return "myteam projects"

    def render(self) -> ui.Node:
        now = find_project(self.rcp)
        items = [ui.item(str(p), p.name + ("  (shown now)" if p == now else ""), str(p.parent),
                         "accent" if p == now else None) for p in candidates()]
        return ui.vbox(
            ui.listbox("projects", items, on_activate=self._choose, empty="No myteam projects found nearby.", grow=1),
            ui.text([("Enter", "accent"), " show this project in the myteam views (remembered)"], "dim"),
        )

    def _choose(self, key: str) -> None:
        choose_project(self.rcp, key)
        self.rcp.open_view("myteam.overview")
        self.close()


for _vid in ("plan", "team", "runs", "customer", "coach", "council", "feedback", "activity", "skills"):

    def _opener(rcp: Context, _v: str = _vid) -> None:
        rcp.open_view(f"myteam.{_v}")

    plugin.command(f"myteam.view.{_vid}")(_opener)


@plugin.command("myteam.run.step")
def run_step(rcp: Context) -> None:
    myteam_console(rcp, shared(rcp), ["run"], "myteam run")


@plugin.command("myteam.run.steps")
def run_steps(rcp: Context) -> None:
    myteam_console(rcp, shared(rcp), ["run", "-n", "5"], "myteam run -n 5")


@plugin.command("myteam.digest")
def open_digest(rcp: Context) -> None:
    """What the coach reads at its next decision, in the editor (a temporary file)."""
    m = shared(rcp)

    def fetch() -> str:
        return m.json("digest").get("text", "")

    def done(text: Any, err: Exception | None) -> None:
        if err:
            rcp.status(f"The digest: {err}", "error")
            return
        f = Path(tempfile.gettempdir()) / f"myteam-digest-{m.project.name}.md"
        f.write_text(text)
        rcp.open_in_editor(f, 1)

    background(rcp, fetch, done)


# ---------------------------------------------------------------------------------
# Overview


@plugin.view("myteam.overview")
class Overview(TeamView):
    def title(self) -> str:
        o = self.d.overview
        n = len(o.get("needs_you", []))
        return f"myteam: {o.get('project', '')}" + (f" ({n})" if n else "") if o else "myteam"

    def render(self) -> ui.Node:
        if (b := self.banner()) and not self.d.overview:
            return ui.vbox(b)
        o = self.d.overview
        c = o.get("counts", {})
        co = o.get("coach", {})
        head = [
            (o.get("project", ""), "title"),
            f"   {o.get('team', '')}   {o.get('methodology', '')}   ",
            (str(self.m.project), "dim"),
        ]
        goal = ui.text([("Goal: ", "dim"), (o.get("goal") or "none yet: tell the coach what you want (Customer)", "bold")],
                       wrap=True)
        counts = []
        for k in ("pending", "in_progress", "review", "blocked", "done"):
            if c.get(k):
                counts += [(f"{c[k]} {STATUS_WORD[k]}", STATUS_STYLE[k]), "   "]
        counts += [(f"{o.get('ready', 0)} ready to run", "accent")]
        if o.get("unassigned"):
            counts += ["   ", (f"{o['unassigned']} unassigned (the coach's)", "warning")]
        coach_name = member_name(self.m, self.d.team.get("coach", "")) or "the coach"
        coach_line = [(f"{coach_name}: ", "bold"), f"{co.get('decisions', 0)} decisions, {co.get('steps', 0)} steps, "
                      f"{co.get('milestones', 0)} milestones, {money(co.get('cost_usd')) or '$0'}"]
        if co.get("last_decision"):
            coach_line.append(f", last {ago(co['last_decision'])}")
        if co.get("news"):
            coach_line += ["   ", (f"{co['news']} news waiting for the coach", "accent")]
        if co.get("objection"):
            coach_line += ["   ", ("the Gatekeeper objects (Coach)", "warning")]
        needs = [
            ui.item(f"{n['kind']}:{n['ref']}:{i}", n["text"][:160], {"operator": "you: do it, then mark it done",
                     "customer": "answer the coach (Customer)", "guard": "the safeguards held a run",
                     "plan": "the plan"}.get(n["kind"], n["kind"]),
                    "warning" if n["kind"] == "customer" else None)
            for i, n in enumerate(o.get("needs_you", []))
        ]
        services = None
        if o.get("services"):
            spans: list[Any] = [("Services: ", "dim")]
            for i, sv in enumerate(o["services"]):
                st = sv.get("state", "")
                spans += ([", "] if i else []) + [(sv["name"], "bold"), " ",
                          (st, "ok" if st == "running" else "warning" if st in ("waiting", "missing") else "error")]
                if sv.get("error"):
                    spans.append((f" ({sv['error']})", "dim"))
            services = ui.text(spans, wrap=True)
        running = [self._run_row(r) for r in o.get("running", [])]
        latest = [self._run_row(r) for r in o.get("latest", [])[:10]]
        cols = [ui.column("When", width=12), ui.column("Who", width=10), ui.column("What", grow=3),
                ui.column("Result", width=12), ui.column("Cost", width=7, align="right")]
        return ui.vbox(
            ui.text(head),
            self.banner() if self.d.error else None,
            goal,
            ui.text(coach_line, wrap=True),
            ui.text(counts),
            services,
            ui.hbox(
                ui.panel(f"Needs you ({len(needs)})",
                         ui.listbox("needs", needs, on_activate=self._need, empty="Nothing waits for you."), grow=1),
                ui.panel(f"Working now ({len(running)})",
                         ui.table("running", cols, running, empty="Nobody is running.", on_activate=self._open_run),
                         grow=1),
                grow=1,
            ),
            ui.panel("Latest runs", ui.table("latest", cols, latest, empty="No runs yet.", on_activate=self._open_run),
                     grow=1),
            ui.text(
                [("r", "accent"), " a step   ", ("R", "accent"), " five steps   ", ("t", "accent"), " talk to the coach   ",
                 ("d", "accent"), " what the coach reads   ", ("Ctrl+P", "accent"), " myteam: ... for the rest   ",
                 (f"{money(o.get('cost_usd'))} in all runs" if o.get("cost_usd") else "", "dim")],
                "dim",
            ),
        )

    def _run_row(self, r: dict[str, Any]) -> ui.Node:
        text, style = run_result(r)
        return ui.row(
            r["name"],
            [ago(r.get("started", "")), "@" + r.get("member", ""), run_what(r)[:200], {"text": text, "style": style},
             money(r.get("cost_usd"))],
            selection=run_item(self.m, r),
        )

    def _open_run(self, key: str) -> None:
        self.rcp.open_view("myteam.run", key)

    def _need(self, key: str) -> None:
        kind, ref, _ = key.split(":", 2)
        if kind == "customer":
            self.rcp.open_view("myteam.customer")
        elif kind == "guard":
            self.rcp.open_view("myteam.run", ref)
        else:
            self.rcp.open_view("myteam.plan")

    @plugin.command("myteam.overview.step")
    def step(self) -> None:
        run_step(self.rcp)

    @plugin.command("myteam.overview.steps")
    def steps(self) -> None:
        run_steps(self.rcp)

    @plugin.command("myteam.overview.talk")
    def talk(self) -> None:
        self.rcp.open_view("myteam.customer")

    @plugin.command("myteam.overview.digest")
    def digest(self) -> None:
        open_digest(self.rcp)


def run_what(r: dict[str, Any]) -> str:
    what = r.get("what") or ""
    kind = r.get("kind")
    if kind == "check":
        return f"checks {what[5:]}" + (f" {r['description'][len('Check ' + what[5:] + ':'):].strip()}" if r.get("description") else "")
    if kind in ("coach", "review", "council"):
        return r.get("description") or what
    if r.get("description"):
        return f"{what} {r['description']}".strip()
    return what or r.get("name", "")[16:]


def run_item(m: Model, r: dict[str, Any]) -> Item:
    text, _ = run_result(r)
    return Item(
        "myteam.run",
        f"{run_what(r)} (@{r.get('member', '')})",
        Path(r["dir"]) / "result.md" if r.get("dir") else None,
        0,
        props=[("Run", r["name"]), ("Member", "@" + r.get("member", "")), ("Model", r.get("model", "")),
               ("Kind", r.get("kind", "")), ("Result", text), ("Started", r.get("started", "")),
               ("Took", duration(r.get("started", ""), r.get("finished", ""), r.get("state") == "running")), ("Cost", money(r.get("cost_usd"))),
               ("Turns", r.get("turns") or ""), ("Commits", r.get("commits") or ""),
               ("Safeguards", r.get("guard") or ""), ("Summary", r.get("summary", ""))],
    )


@plugin.view("myteam.plan")
class PlanView(TeamView):
    def init(self) -> None:
        self.closed: set[str] = set()
        self.hide_done = False
        self.rows: list[dict[str, Any]] = []

    def title(self) -> str:
        n = len(self.d.plan.get("tasks", []))
        return f"Plan ({n})" if n else "Plan"

    def _flatten(self) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        tasks = self.d.plan.get("tasks", [])
        section = None
        skip_below: int | None = None
        for i, t in enumerate(tasks):
            if t.get("section") != section:
                section = t.get("section")
                skip_below = None
                if section:
                    out.append({"_section": section, "key": f"s:{section}"})
            depth = t["depth"] + (1 if section else 0)
            if skip_below is not None and depth > skip_below:
                continue
            skip_below = None
            if section and f"s:{section}" in self.closed:
                continue
            if self.hide_done and t["status"] in ("done", "cancelled"):
                skip_below = depth
                continue
            has_kids = i + 1 < len(tasks) and tasks[i + 1]["depth"] > t["depth"]
            out.append({**t, "_depth": depth, "_kids": has_kids})
            if has_kids and t["key"] in self.closed:
                skip_below = depth
        return out

    def render(self) -> ui.Node:
        self.rows = self._flatten()
        p = self.d.plan
        head = [("Goal: ", "dim"), (p["goal"], "bold")] if p.get("goal") else []
        return ui.vbox(
            self.banner(),
            ui.text(head) if head else None,
            ui.table(
                "tasks",
                [ui.column("Task", grow=5), ui.column("Status", width=12), ui.column("Who", width=11),
                 ui.column("Pri", width=4), ui.column("Est", width=5), ui.column("", grow=2)],
                ui.lazy_rows(len(self.rows), self._row),
                tree_column=0,
                on_activate=self._open,
                on_expand=self._expand,
                on_collapse=self._collapse,
                empty="No tasks." if self.m.loaded else "",
                autofocus=True,
                grow=1,
            ),
            ui.text([("Enter", "accent"), " open in the editor   ", ("d", "accent"), " mark an @operator task done   ",
                     ("D", "accent"), " decline it   ",
                     ("h", "accent"), " show finished tasks" if self.hide_done else " hide finished tasks"], "dim"),
        )

    def _row(self, i: int) -> ui.Node:
        t = self.rows[i]
        if "_section" in t:
            return ui.row(t["key"], [{"text": t["_section"], "style": "title"}, "", "", "", "", ""], depth=0,
                          expandable=True, expanded=t["key"] not in self.closed)
        st = t["status"]
        info = []
        if t.get("waiting_on"):
            info.append("after " + " ".join("^" + x for x in t["waiting_on"]))
        if t.get("blockers"):
            info.append("on " + " ".join("^" + x for x in t["blockers"]))
        crit = t.get("criteria", [])
        if crit:
            ok = sum(1 for c in crit if c["status"] == "verified")
            bad = sum(1 for c in crit if c["status"] == "failed")
            info.append(f"{ok}/{len(crit)} criteria" + (f", {bad} failed" if bad else ""))
        tags = [x for x in t.get("tags", []) if not x.startswith("p")]
        if tags:
            info.append(" ".join("#" + x for x in tags))
        desc = t["description"]
        if t.get("id"):
            desc = [(desc, STATUS_STYLE.get(st, "normal") if st in ("done", "cancelled") else "normal"),
                    (f"  ^{t['id']}", "dim")]
        return ui.row(
            t["key"],
            [desc, {"text": STATUS_WORD.get(st, st), "style": STATUS_STYLE.get(st, "normal")},
             "@" + t["assignee"] if t.get("assignee") else "",
             f"p{t['priority']}" if t.get("priority") is not None else "", t.get("estimate", ""), "; ".join(info)],
            depth=t["_depth"],
            expandable=t["_kids"],
            expanded=t["key"] not in self.closed,
            selection=task_item(self.d.plan, t),
        )

    def _find(self, key: str) -> dict[str, Any] | None:
        return next((t for t in self.rows if t.get("key") == key), None)

    def _open(self, key: str) -> None:
        t = self._find(key)
        if t and "_section" not in t:
            self.rcp.open_in_editor(self.d.plan["path"], t.get("line") or 1)
        elif t:
            (self._expand if key in self.closed else self._collapse)(key)

    def _expand(self, key: str) -> None:
        self.closed.discard(key)

    def _collapse(self, key: str) -> None:
        self.closed.add(key)

    @plugin.command("myteam.plan.hideDone")
    def toggle_done(self) -> None:
        self.hide_done = not self.hide_done

    @plugin.command("myteam.plan.done")
    def mark_done(self) -> None:
        t = self._find(self.focus_key or "")
        if not t or "_section" in t:
            return
        if t.get("assignee") != "operator":
            self.rcp.status("Only @operator tasks are yours to finish; the crew finishes its own.", "error")
            return
        Done.task = t
        Done.declining = False
        self.rcp.open_view("myteam.done")

    @plugin.command("myteam.plan.decline")
    def decline(self) -> None:
        t = self._find(self.focus_key or "")
        if not t or "_section" in t:
            return
        if t.get("assignee") != "operator":
            self.rcp.status("Only @operator tasks are yours to decline.", "error")
            return
        Done.task = t
        Done.declining = True
        self.rcp.open_view("myteam.done")


def task_item(plan: dict[str, Any], t: dict[str, Any]) -> Item:
    props: list[tuple[str, Any]] = [("Task", t["key"]), ("Status", STATUS_WORD.get(t["status"], t["status"]))]
    for k, label in (("assignee", "Assignee"), ("estimate", "Estimate"), ("due", "Due")):
        if t.get(k):
            props.append((label, ("@" if k == "assignee" else "") + str(t[k])))
    if t.get("priority") is not None:
        props.append(("Priority", f"p{t['priority']}"))
    if t.get("depends"):
        props.append(("Depends on", " ".join("^" + x for x in t["depends"])))
    if t.get("blocked_by"):
        props.append(("Blocked by", " ".join("^" + x for x in t["blocked_by"])))
    for n, c in enumerate(t.get("criteria", []), 1):
        mark = {"verified": "(x)", "failed": "(!)"}.get(c["status"], "( )")
        props.append((f"Criterion {n}", f"{mark} {c['text']}" + (f"  [{c['evidence']}]" if c.get("evidence") else "")))
    for n, note in enumerate(t.get("notes", []), 1):
        props.append((f"Note {n}", note))
    return Item("myteam.task", f"{t['key']} {t['description']}", plan.get("path"), t.get("line") or 0, props=props)


@plugin.view("myteam.done")
class Done(View):
    """Finish an @operator task with a note (a small form)."""

    task: dict[str, Any] | None = None
    declining = False

    def setup(self) -> None:
        self.m = shared(self.rcp)
        self.t = Done.task
        self.declining = Done.declining

    def title(self) -> str:
        return "Decline" if self.declining else "Mark done"

    def render(self) -> ui.Node:
        if not self.t:
            return ui.text("Choose an @operator task in the Plan and press d (done) or D (decline).", "dim")
        if self.declining:
            return ui.vbox(
                ui.text([("Decline: ", "bold"), self.t["description"]], wrap=True),
                ui.text("Why not? The team reads this; what waited for the task is held for a decision, not released.",
                        "dim", wrap=True),
                ui.field("note", placeholder="e.g. nothing is installed; Ghostscript's txtwrite extracts PDF text",
                         on_submit=self._done),
                ui.hbox(ui.button("ok", "Decline", on_press=lambda: self._done(self.form.get("note", ""))),
                        ui.button("cancel", "Cancel", on_press=self.close)),
            )
        return ui.vbox(
            ui.text([("Done: ", "bold"), self.t["description"]], wrap=True),
            ui.text("What did you do? The team reads this note.", "dim"),
            ui.field("note", placeholder="e.g. the API key is in .env as API_KEY", on_submit=self._done),
            ui.hbox(ui.button("ok", "Mark done", on_press=lambda: self._done(self.form.get("note", ""))),
                    ui.button("cancel", "Cancel", on_press=self.close)),
        )

    def _done(self, note: str) -> None:
        if self.declining and not note.strip():
            self.rcp.status("Say why: the team reads it.", "error")
            return
        args = ["done", self.t["key"]] + ([note] if note.strip() else []) + (["--declined"] if self.declining else [])
        myteam_job(self.rcp, self.m, args, f"{'Declined' if self.declining else 'Done'} {self.t['key']}")
        self.close()


# ---------------------------------------------------------------------------------
# The crew


@plugin.view("myteam.team")
class Crew(TeamView):
    def title(self) -> str:
        return f"Crew {self.d.team.get('name', '')}".strip()

    def render(self) -> ui.Node:
        rows = []
        for mem in self.d.team.get("members", []):
            running = mem.get("running")
            if running:
                now = {"text": "running " + run_what(running)[:80], "style": "accent"}
            elif mem.get("in_progress"):
                now = " ".join(mem["in_progress"])
            else:
                now = {"text": "idle", "style": "dim"}
            kind = mem.get("kind", "expert")
            role = KIND_WORD.get(kind, kind) if kind != "expert" else mem.get("role", "")
            memory = mem.get("memory") or {}
            lessons = memory.get("lessons") or []
            props: list[tuple[str, Any]] = [
                ("Role", role), ("Unit", (mem.get("unit") or "the org") + "/"), ("Does", mem.get("summary", "")),
                ("Model", mem["models"]["standard"]), ("Open tasks", mem.get("open", 0)),
                ("In review", mem.get("review", 0)), ("Done", mem.get("done", 0)), ("Runs", mem.get("runs", 0)),
                ("Cost", money(mem.get("cost_usd")))]
            if memory.get("coaching"):
                props.append(("From the coach", memory["coaching"]))
            if memory.get("notes"):
                props.append(("Notes", memory["notes"][:1500]))
            for n, lesson in enumerate(reversed(lessons[-8:]), 1):
                props.append((f"Lesson {n}", lesson))
            rows.append(ui.row(
                mem["handle"],
                [[(mem.get("name", ""), "bold"), (f"  @{mem['handle']}", "dim")],
                 {"text": role, "style": "accent" if kind == "coach" else "warning" if kind == "gatekeeper" else "normal"},
                 mem.get("unit", ""), now, str(mem.get("open", 0)), str(mem.get("review", 0)), str(len(lessons)),
                 str(mem.get("runs", 0)), money(mem.get("cost_usd"))],
                selection=Item("myteam.member", f"{mem.get('name')} (@{mem['handle']})",
                               memory.get("file") or None, 1, props=props),
            ))
        return ui.vbox(
            self.banner(),
            ui.table("members", [ui.column("Member", grow=2), ui.column("Role", width=18), ui.column("Unit", width=11),
                                 ui.column("Now", grow=3), ui.column("Open", width=5, align="right"),
                                 ui.column("Rev", width=4, align="right"), ui.column("Learned", width=8, align="right"),
                                 ui.column("Runs", width=5, align="right"), ui.column("Cost", width=8, align="right")],
                     rows, on_activate=self._open, sortable=True, empty="", grow=1),
            ui.text([("Enter", "accent"), " the member's runs   ", ("m", "accent"), " its memory   ",
                     ("e", "accent"), " its role prompt"], "dim"),
        )

    def _member(self) -> dict[str, Any] | None:
        return next((x for x in self.d.team.get("members", []) if x["handle"] == self.focus_key), None)

    def _open(self, key: str) -> None:
        Runs.wanted_filter = "@" + key
        self.rcp.open_view("myteam.runs")

    @plugin.command("myteam.team.memory")
    def memory(self) -> None:
        if (mem := self._member()) and (f := (mem.get("memory") or {}).get("file")):
            Path(f).parent.mkdir(parents=True, exist_ok=True)
            if not Path(f).exists():
                Path(f).write_text("")
            self.rcp.open_in_editor(f, 1, on_close=lambda _w: self.m.reload())

    @plugin.command("myteam.team.prompt")
    def prompt(self) -> None:
        if (mem := self._member()) and mem.get("file"):
            self.rcp.open_in_editor(mem["file"], 1, on_close=lambda _w: self.m.reload())


@plugin.view("myteam.runs")
class Runs(TeamView):
    wanted_filter: str | None = None
    KINDS = ["all", "task", "check", "coach", "review", "council", "repair", "scrum"]

    def init(self) -> None:
        self.filter = ""
        self.kind = "all"

    def title(self) -> str:
        running = sum(1 for r in self.d.runs if r.get("state") == "running")
        return f"Runs ({running} running)" if running else "Runs"

    def _visible(self) -> list[dict[str, Any]]:
        if Runs.wanted_filter is not None:
            self.filter, Runs.wanted_filter = Runs.wanted_filter, None
        f = self.filter.lower().strip()
        out = []
        for r in self.d.runs:
            if self.kind != "all" and r.get("kind") != self.kind:
                continue
            if f:
                hay = f"@{r.get('member', '')} {run_what(r)} {r.get('summary', '')} {r.get('model', '')}".lower()
                if not all(w in hay for w in f.split()):
                    continue
            out.append(r)
        return out

    def render(self) -> ui.Node:
        runs = self._visible()
        rows = []
        for r in runs:
            text, style = run_result(r)
            notes = []
            if r.get("commits"):
                notes.append(f"{r['commits']} commit" + ("s" if r["commits"] > 1 else ""))
            if r.get("guard"):
                notes.append(f"{r['guard']} safeguard note" + ("s" if r["guard"] > 1 else ""))
            rows.append(ui.row(
                r["name"],
                [ago(r.get("started", "")), "@" + r.get("member", ""), r.get("kind", ""), run_what(r)[:200],
                 {"text": text, "style": style}, duration(r.get("started", ""), r.get("finished", ""), r.get("state") == "running"),
                 money(r.get("cost_usd")), ", ".join(notes)],
                selection=run_item(self.m, r),
            ))
        total = sum(float(r.get("cost_usd") or 0) for r in runs)
        return ui.vbox(
            self.banner(),
            ui.hbox(
                ui.field("filter", self.filter, placeholder="Filter: @member, task, words…", on_submit=self._filter, grow=1),
                ui.button("kind", f"Kind: {self.kind}", on_press=self._next_kind),
                ui.text(f"{len(runs)} runs, {money(total) or '$0'}", "dim"),
                grow=0,
            ),
            ui.table("runs", [ui.column("When", width=16), ui.column("Who", width=10), ui.column("Kind", width=9),
                              ui.column("What", grow=4), ui.column("Result", width=12), ui.column("Took", width=11),
                              ui.column("Cost", width=7, align="right"), ui.column("Notes", grow=1)],
                     rows, on_activate=self._open, sortable=True, empty="No runs." if self.m.loaded else "",
                     autofocus=True, grow=1),
            ui.text([("Enter", "accent"), " the run   ", ("k", "accent"), " kind   ", ("u", "accent"),
                     " undo a task run"], "dim"),
        )

    def _filter(self, text: str) -> None:
        self.filter = text

    @plugin.command("myteam.runs.kind")
    def _next_kind(self) -> None:
        self.kind = self.KINDS[(self.KINDS.index(self.kind) + 1) % len(self.KINDS)]

    def _open(self, key: str) -> None:
        self.rcp.open_view("myteam.run", key)

    @plugin.command("myteam.runs.undo")
    def undo(self) -> None:
        if self.focus_key:
            Undo.run = next((r for r in self.d.runs if r["name"] == self.focus_key), None)
            self.rcp.open_view("myteam.undo")


@plugin.view("myteam.undo")
class Undo(View):
    run: dict[str, Any] | None = None

    def setup(self) -> None:
        self.m = shared(self.rcp)
        self.r = Undo.run

    def title(self) -> str:
        return "Undo a run"

    def render(self) -> ui.Node:
        if not self.r or self.r.get("kind") != "task":
            return ui.text("Only task runs can be undone: choose one in Runs and press u.", "dim")
        return ui.vbox(
            ui.text([("Undo ", "bold"), run_what(self.r), f"  by @{self.r.get('member')}"], wrap=True),
            ui.text("The files the run changed go back to how they were before it (files changed since are kept), "
                    "and the task goes back to [ ]. Commits stay; myteam says how to revert them.", wrap=True),
            ui.hbox(ui.button("ok", "Undo the run", on_press=self._undo), ui.button("cancel", "Cancel", on_press=self.close)),
        )

    def _undo(self) -> None:
        myteam_job(self.rcp, self.m, ["undo", self.r["name"]], f"Undo {self.r['what']}")
        self.close()


@plugin.view("myteam.run")
class RunView(TeamView):
    """One run: what it was for, its report, the safeguards, its commits, and its events (live)."""

    def init(self) -> None:
        self.name = self.arg or ""
        self.detail: dict[str, Any] = {}
        self.events: list[dict[str, Any]] = []
        self.error = ""
        self.loading = False
        self.again = False
        self.m.follow_run(self.name, self._changed)
        self._changed()

    def dispose(self) -> None:
        super().dispose()
        self.m.unfollow_run(self.name)

    def title(self) -> str:
        what = self.detail.get("what") or self.name[16:]
        return f"Run: {what} @{self.detail.get('member', '')}" if self.detail else "Run"

    def _changed(self) -> None:
        if self.loading:
            self.again = True
            return
        self.loading = True
        name, frm = self.name, len(self.events)

        def fetch() -> tuple[dict[str, Any], dict[str, Any]]:
            return self.m.json("run", name), self.m.json("events", name, "--from", str(frm))

        background(self.rcp, fetch, self._fetched)

    def _fetched(self, res: Any, err: Exception | None) -> None:
        self.loading = False
        if err:
            self.error = str(err)
        else:
            self.detail, ev = res
            # A restarted count (the stream was rewritten): start over.
            if ev.get("from", 0) != len(self.events):
                self.events = []
            self.events += ev.get("events", [])
            self.error = ""
        self.invalidate()
        if self.again:
            self.again = False
            self._changed()

    def _event_lines(self) -> list[Any]:
        lines: list[Any] = []
        for e in self.events:
            k, text, tool = e.get("kind"), e.get("text", ""), e.get("tool", "")
            if k == "text":
                lines += wrapped(text)
                lines.append("")
            elif k == "tool_call":
                first, *rest = (text or "").splitlines() or [""]
                label = "$ " if tool == "Bash" else f"{tool} "
                lines.append([(label, "accent"), (first[:WRAP * 2], "bold")])
                for r in rest[:6]:
                    lines.append([("    " + r[:WRAP * 2], "dim")])
                if len(rest) > 6:
                    lines.append([(f"    … {len(rest) - 6} more lines", "dim")])
            elif k == "tool_result":
                if text and text != "ok":
                    lines += wrapped(text[:600], "error", "    ")
            elif k == "error":
                lines += wrapped(text, "error")
            elif k == "usage":
                lines.append([(text, "dim")])
        return lines

    def render(self) -> ui.Node:
        d = self.detail
        if not d:
            return ui.text(self.error or "Loading the run…", "error" if self.error else "dim")
        text, style = run_result(d)
        rec = d.get("record") or {}
        rep = rec.get("report") or {}
        head = ui.text([
            (run_what(d), "title"), "   ", (f"@{d.get('member', '')}", "bold"), f"  {d.get('model', '')}   ",
            (text, style), f"   {ago(d.get('started', ''))}",
            f", took {duration(d.get('started', ''), d.get('finished', ''), d.get('state') == 'running')}"
            if d.get("finished") or d.get("state") == "running" else "",
            f"   {money(d.get('cost_usd'))}" if d.get("cost_usd") else "",
            f", {d.get('turns')} turns" if d.get("turns") else "",
        ], wrap=True)
        report: list[Any] = []
        if rep.get("status"):
            report.append([("Reported: ", "dim"), (rep["status"], "bold")])
        if rep.get("summary"):
            report += wrapped(rep["summary"])
        for h in rep.get("handoffs", []) or []:
            report.append([("Handoff to ", "dim"), (f"@{h.get('to', '')}", "bold"), ": " + (h.get("instructions", "")[:200])])
        for b in rep.get("blockers", []) or []:
            report.append([("Blocker: ", "error"), str(b.get("text", b) if isinstance(b, dict) else b)[:200]])
        for c in rec.get("plan_changes", []) or []:
            report.append([("Plan: ", "dim"), c])
        for p in rec.get("problems", []) or []:
            report.append([("Problem: ", "error"), p])
        if not report and d.get("result"):
            report = wrapped(d["result"][-3000:])
        if not report and d.get("state") == "running":
            report = [[("Working…", "accent")]]
        guard = (d.get("guard_record") or {}).get("guard") or []
        commits = rec.get("commits") or []
        commit_rows = [
            ui.item(c["commit"], f"{c['commit'][:10]}  {c.get('summary', '')}",
                    f"{c.get('branch', '')}, {len(c.get('files', []))} files", "ok")
            for c in commits
        ]
        err = d.get("stderr", "").strip() if not d.get("ok") and d.get("state") != "running" else ""
        return ui.vbox(
            head,
            ui.text(self.error, "error") if self.error else None,
            ui.panel("Report", ui.log("report", report or [[("No report.", "dim")]]), height=min(14, len(report) + 2) if report else 3),
            ui.panel(f"Safeguards ({len(guard)})", ui.log("guard", [[(g, "warning")] for g in guard]),
                     height=min(8, len(guard) + 2)) if guard else None,
            ui.panel(f"Commits ({len(commits)})", ui.listbox("commits", commit_rows, on_activate=self._show_commit),
                     height=min(7, len(commits) + 2)) if commits else None,
            ui.panel("stderr", ui.log("stderr", err.splitlines()[-12:]), height=8) if err else None,
            ui.panel(f"What the agent did ({len(self.events)} events)",
                     ui.log("events", self._event_lines() or [[("No events yet.", "dim")]]), grow=1),
            ui.text([("p", "accent"), " the prompt it got   ", ("o", "accent"), " the final message   ",
                     ("Enter", "accent"), " on a commit: git show"], "dim"),
        )

    def _show_commit(self, sha: str) -> None:
        self.rcp.run_in_console(["git", "show", "--stat", "-p", sha], cwd=self.m.project, title=f"git show {sha[:10]}",
                                name="myteam git show")

    @plugin.command("myteam.run.prompt")
    def prompt(self) -> None:
        if self.detail.get("prompt_path"):
            self.rcp.open_in_editor(self.detail["prompt_path"])

    @plugin.command("myteam.run.result")
    def result(self) -> None:
        if self.detail.get("dir"):
            self.rcp.open_in_editor(Path(self.detail["dir"]) / "result.md")


# ---------------------------------------------------------------------------------
# Customer: you and the coach


@plugin.view("myteam.customer")
class Customer(TeamView):
    """You are the customer: the coach is your only contact with the crew."""

    def init(self) -> None:
        self.sending = False
        self.reply = ""  # what the reply field is set to (the workbench owns what is typed)

    def title(self) -> str:
        return "Customer (the coach wrote)" if self.d.conversation.get("waiting_for_you") else "Customer"

    def render(self) -> ui.Node:
        conv = self.d.conversation
        coach = member_name(self.m, self.d.team.get("coach", "")) or "the coach"
        lines: list[Any] = []
        for msg in conv.get("messages", []):
            you = msg.get("who") == "customer"
            lines.append([("You" if you else coach, "accent" if you else "ok"), (f"  {msg.get('when', '')}", "dim")]
                         + ([("  not read yet", "warning")] if you and not msg.get("seen") else []))
            lines += wrapped(msg.get("text", ""), "normal", "  ")
            lines.append("")
        if self.sending:
            lines.append([("Sending…", "accent")])
        state = ""
        if conv.get("unseen_by_coach"):
            state = f"{coach} reads your message at the next decision (r runs a step now)."
        elif conv.get("waiting_for_you"):
            state = f"{coach} wrote last: your answer, or what you want next."
        return ui.vbox(
            self.banner(),
            ui.panel(f"You and {coach}", ui.log("messages", lines or [[(f"No messages yet. Tell {coach} what you want.", "dim")]]),
                     grow=1),
            ui.text(state, "warning" if conv.get("waiting_for_you") else "dim") if state else None,
            ui.hbox(
                ui.field("reply", self.reply, placeholder=f"To {coach}…", on_submit=self._send, grow=1),
                ui.button("send", "Send", on_press=lambda: self._send(self.form.get("reply", ""))),
                grow=0,
            ),
            ui.text([("Enter", "accent"), " send   ", ("r", "accent"), " a step now (the coach answers when it decides)   ",
                     f"{coach} turns what you say into tasks; the experts never see this conversation"], "dim"),
        )

    def _clear_reply(self, sent: str) -> None:
        """Empty the field after sending. The workbench applies a field's value only when
        it changes, so echo what was sent, then clear it: a moment later, as the host sends
        one update for renders that come together."""
        self.reply = sent
        self.invalidate()

        def clear() -> None:
            self.reply = ""
            self.invalidate()

        threading.Timer(0.2, lambda: self.rcp.call_soon(clear)).start()

    def _send(self, text: str) -> None:
        sent = text
        text = text.strip()
        if not text:
            return
        self._clear_reply(sent)
        self.sending = True

        def work(_j: Job) -> tuple[bool, str]:
            return command(self.m.exe, self.m.project, "say", text)

        def done(j: Job) -> None:
            self.sending = False
            ok, said = j.result if j.ok else (False, str(j.error))
            if not ok:
                self.rcp.status(f"Not sent: {said.splitlines()[-1] if said else 'failed'}", "error")
            self.m.reload()

        self.rcp.run_job("Tell the coach", work, on_done=done)

    @plugin.command("myteam.customer.step")
    def step(self) -> None:
        run_step(self.rcp)


# ---------------------------------------------------------------------------------
# The coach's desk: what it sees, what the Gatekeeper holds, what it decided


@plugin.view("myteam.coach")
class CoachDesk(TeamView):
    def title(self) -> str:
        return "Coach (the Gatekeeper objects)" if self.d.coach.get("objection") else "Coach"

    def render(self) -> ui.Node:
        c = self.d.coach
        coach = member_name(self.m, c.get("coach", "")) or "the coach"
        gate = member_name(self.m, c.get("gatekeeper", "")) or "the Gatekeeper"
        head = ui.text([(coach, "title"), f" decides, {gate} checks.   ",
                        f"{c.get('decisions', 0)} decisions, {c.get('steps', 0)} steps, {c.get('milestones', 0)} milestones, "
                        f"{money(c.get('cost_usd')) or '$0'}",
                        f", last decision {ago(c['last_decision'])}" if c.get("last_decision") else ""], wrap=True)
        objection = None
        if c.get("objection"):
            lines = wrapped(c["objection"], "warning") + ["", [("Held decisions (not applied):", "dim")]]
            lines += [[(l, "dim")] for l in (c.get("held") or "").splitlines()]
            objection = ui.panel(f"{gate} objects: {coach} revises or overrules at the next decision",
                                 ui.log("objection", lines), height=min(16, len(lines) + 2))
        news = []
        for n in c.get("news", []):
            first, *rest = n.splitlines()
            news += wrapped(first, "normal", "- ")
            news += [[("  " + r.strip(), "dim")] for r in rest]
        notes = wrapped(c.get("notes", "")) if c.get("notes") else [[("No notes yet.", "dim")]]
        decisions = [r for r in self.d.runs if r.get("kind") in ("coach", "review")][:30]
        rows = []
        for r in decisions:
            text, style = run_result(r)
            rows.append(ui.row(r["name"], [ago(r.get("started", "")), "@" + r.get("member", ""), run_what(r),
                                           {"text": text, "style": style}, money(r.get("cost_usd"))],
                               selection=run_item(self.m, r)))
        return ui.vbox(
            self.banner(),
            head,
            objection,
            ui.hbox(
                ui.panel(f"News waiting for {coach} ({len(c.get('news', []))})",
                         ui.log("news", news or [[("Nothing new since the last decision.", "dim")]]), grow=1),
                ui.panel(f"{coach}'s notes", ui.log("notes", notes), grow=1),
                grow=1,
            ),
            ui.panel("Decisions and reviews",
                     ui.table("decisions", [ui.column("When", width=14), ui.column("Who", width=10), ui.column("What", grow=3),
                                            ui.column("Result", width=10), ui.column("Cost", width=7, align="right")],
                              rows, on_activate=lambda k: self.rcp.open_view("myteam.run", k),
                              empty="No decisions yet: r runs a step."), height=min(14, len(rows) + 3)),
            ui.text([("Enter", "accent"), " the run (its answer: o)   ", ("d", "accent"), " what the coach reads now   ",
                     ("r", "accent"), " a step   ", ("c", "accent"), " the councils"], "dim"),
        )

    @plugin.command("myteam.coach.digest")
    def digest(self) -> None:
        open_digest(self.rcp)

    @plugin.command("myteam.coach.step")
    def step(self) -> None:
        run_step(self.rcp)

    @plugin.command("myteam.coach.council")
    def council(self) -> None:
        self.rcp.open_view("myteam.council")


@plugin.view("myteam.council")
class Council(TeamView):
    """The coach's questions to the experts, and their views (the Gatekeeper's last)."""

    def init(self) -> None:
        self.current: str | None = None

    def title(self) -> str:
        n = len(self.d.council)
        return f"Council ({n})" if n else "Council"

    def render(self) -> ui.Node:
        items = [ui.item(c["name"], c.get("topic", ""), f"{ago(c.get('started', ''))}, {len(c.get('views', []))} views")
                 for c in self.d.council]
        cur = next((c for c in self.d.council if c["name"] == self.current), self.d.council[0] if self.d.council else None)
        lines: list[Any] = []
        if cur:
            lines += [[("Question", "bold")]] + wrapped(cur.get("question", ""), "normal", "  ") + [""]
            for v in cur.get("views", []):
                lines.append([("@" + v.get("who", ""), "accent"), (f"  {member_name(self.m, v.get('who', ''))}", "dim")])
                lines += wrapped(v.get("text", ""), "normal", "  ")
                lines.append("")
        return ui.vbox(
            self.banner(),
            ui.hbox(
                ui.panel("Questions", ui.listbox("councils", items, on_select=self._select, on_activate=self._open,
                                                 empty="No council yet: the coach asks one when a decision crosses units."),
                         width=40),
                ui.panel(cur.get("topic", "") if cur else "Views", ui.log("views", lines or [[("", "dim")]]), grow=1),
                grow=1,
            ),
            ui.text([("Enter", "accent"), " the council's file"], "dim"),
        )

    def _select(self, key: str) -> None:
        self.current = key

    def _open(self, key: str) -> None:
        c = next((x for x in self.d.council if x["name"] == key), None)
        if c:
            self.rcp.open_in_editor(c["path"], 1)


# ---------------------------------------------------------------------------------
# Skills: what the crew learned to do; feedback: what got in its way


@plugin.view("myteam.skills")
class Skills(TeamView):
    def title(self) -> str:
        n = len(self.d.skills)
        return f"Skills ({n})" if n else "Skills"

    def render(self) -> ui.Node:
        rows = []
        for s in self.d.skills:
            who = ", ".join("@" + x for x in s.get("members", [])) or "everyone"
            origin = s.get("source", "")
            rows.append(ui.row(s["slug"], [[(s.get("name", ""), "bold")], who, ", ".join(s.get("triggers", [])), origin,
                                           s.get("description", "")],
                               selection=Item("myteam.skill", s.get("name", ""), s.get("file"), 1, props=[
                                   ("Skill", s["slug"]), ("For", who), ("Triggers", ", ".join(s.get("triggers", []))),
                                   ("From", origin), ("When", s.get("description", "")), ("How", s.get("body", ""))])))
        return ui.vbox(
            self.banner(),
            ui.text("What the crew learned to do. A task gets the skills its words call for (the triggers). The coach "
                    "makes them from the experts' lessons, and commits each in git; a skill that works around a tool's "
                    "gap belongs in the tool's backlog.", "dim", wrap=True),
            ui.table("skills", [ui.column("Skill", grow=2), ui.column("For", width=14), ui.column("Triggers", grow=1),
                                ui.column("From", grow=1), ui.column("When to use it", grow=3)],
                     rows, on_activate=self._open, sortable=True,
                     empty="No skills yet: the coach makes them from what the experts learn.",
                     autofocus=True, grow=1),
            ui.text([("Enter", "accent"), " open the skill in the editor"], "dim"),
        )

    def _open(self, key: str) -> None:
        s = next((x for x in self.d.skills if x["slug"] == key), None)
        if s and s.get("file"):
            self.rcp.open_in_editor(s["file"], 1, on_close=lambda _w: self.m.reload())


@plugin.view("myteam.feedback")
class Feedback(TeamView):
    def title(self) -> str:
        n = sum(1 for f in self.d.feedback if f.get("status") == "new")
        return f"Feedback for myteam ({n})" if n else "Feedback for myteam"

    def render(self) -> ui.Node:
        rows = [ui.row(f["id"], [f.get("area", ""), [(f.get("title", ""), "bold")], f.get("status", ""), str(f.get("reports", 0)),
                                 ", ".join("@" + r for r in f.get("reporters", []))],
                       selection=Item("myteam.feedback", f.get("title", ""), self.m.project / ".myteam" / "feedback.md", 1,
                                      props=[("Area", f.get("area", "")), ("Status", f.get("status", ""))]
                                      + [(f"Note {n}", x) for n, x in enumerate(f.get("notes", []), 1)]))
                for f in self.d.feedback]
        return ui.vbox(
            self.banner(),
            ui.text("What in the way the crew works got in the experts' way: for myteam's developers.", "dim"),
            ui.table("feedback", [ui.column("Area", width=11), ui.column("Issue", grow=4), ui.column("Status", width=8),
                                  ui.column("Reports", width=8, align="right"), ui.column("Who", grow=1)],
                     rows, on_activate=lambda _k: self.rcp.open_in_editor(self.m.project / ".myteam" / "feedback.md", 1),
                     sortable=True, empty="No feedback.", grow=1),
        )


# ---------------------------------------------------------------------------------
# Activity: the changelog, newest first


@plugin.view("myteam.activity")
class Activity(TeamView):
    def title(self) -> str:
        return "Activity"

    def render(self) -> ui.Node:
        rows = []
        for e in self.d.activity:
            items = e["items"]
            result = (items.get("Result") or [""])[0]
            style = ("ok" if "completed" in result or "[x]" in result
                     else "error" if "fail" in result or "blocked" in result else "normal")
            extra = []
            for k in ("Commit", "Guard", "Problem", "Feedback for myteam"):
                if items.get(k):
                    extra.append(f"{len(items[k])} {k.lower()}")
            who = (items.get("Team") or [""])[0].partition("member: ")[2].split(",")[0]
            rows.append(ui.row(
                str(e["line"]),
                [ago(e["time"]), e["title"][:200], who, {"text": result[:60], "style": style}, ", ".join(extra)],
                selection=Item("myteam.activity", e["title"][:80], self.m.project / "CHANGELOG.md", e["line"],
                               props=[(k, "; ".join(v)) for k, v in items.items()]
                               + ([("Summary", " ".join(e["body"])[:500])] if e["body"] else [])),
            ))
        return ui.vbox(
            ui.table("activity", [ui.column("When", width=16), ui.column("What", grow=3), ui.column("Who", width=18),
                                  ui.column("Result", width=22), ui.column("", width=24)],
                     rows, on_activate=self._open, empty="Nothing has happened yet.", grow=1),
        )

    def _open(self, key: str) -> None:
        self.rcp.open_in_editor(self.m.project / "CHANGELOG.md", int(key))
