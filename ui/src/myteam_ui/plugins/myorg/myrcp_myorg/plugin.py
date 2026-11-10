"""myorg in the workbench: watch one crew work across an org's units, and run it.

The views read `myorg json` through one shared Model (model.py), which follows the
org's files. Running the org (a tick, a run until it needs you) is myorg in a Console;
sync runs as a job. The crew's work (the plan, the runs, the coach, the conversation)
is in the myteam plugin's views, on the org: one Enter away.
"""

from __future__ import annotations

import datetime as dt
import sys
import weakref
from pathlib import Path
from typing import Any

from myrcp import Context, Item, Job, Marker, Plugin, View, ui

from .model import PLUGIN_DIR, Model, candidates, command, find_org, is_org, save_org

plugin = Plugin()

MODE_STYLE = {"collaboration": "accent", "x-as-a-service": "ok", "facilitating": "warning", "": "dim"}


# ---------------------------------------------------------------------------------
# helpers


def shared(rcp: Context) -> Model:
    m = Model.get(rcp)
    if not getattr(m, "_markers_on", False):
        m._markers_on = True  # type: ignore[attr-defined]
        m.subscribe(lambda: update_markers(m))
    return m


def update_markers(m: Model) -> None:
    org_md = m.org / "ORG.md"
    markers = []
    for u in m.data.units:
        if not u.get("repository") and u.get("dir") != "integration" and not (m.org / u.get("dir", "") / ".myteam").is_dir():
            markers.append(Marker(f"{u['handle']} is not set up yet (myorg sync)", org_md, severity="info", source="myorg"))
        if not u.get("experts"):
            markers.append(Marker(f"{u['handle']} has no expert (a crew member with unit: {u.get('dir', '')})", org_md,
                                  severity="warning", source="myorg"))
    if m.data.ticks:  # the newest tick's problems: what the last pass ran into
        for p in m.data.ticks[0].get("problems", []):
            markers.append(Marker(p, severity="warning", source="myorg run", view="myorg.activity"))
    m.rcp.set_markers("myorg", markers)


def parse_time(s: str) -> dt.datetime | None:
    try:
        return dt.datetime.strptime((s or "").strip(), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc)
    except ValueError:
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


def money(x: Any) -> str:
    try:
        v = float(x or 0)
    except (TypeError, ValueError):
        return ""
    return f"${v:.2f}" if v else ""


def short(paths: list[str]) -> str:
    return ", ".join(p.rstrip("/").rsplit("/", 1)[-1] if p.startswith("..") else p for p in paths)


def unit_item(m: Model, u: dict[str, Any]) -> Item:
    v = u.get("view", {})
    props = [("Unit", u["handle"]), ("Type", u.get("type", "")), ("Purpose", u.get("purpose", "")),
             ("Directory", u.get("dir", "")), ("Experts", ", ".join("@" + e for e in u.get("experts", []))),
             ("Sees", ", ".join(v.get("readable", []))), ("Writes outside it", ", ".join(v.get("writable", []))),
             ("Hidden from it", ", ".join(v.get("hide", [])))]
    props += [(f"Works with {n}", x) for n, x in enumerate(v.get("notes", []), 1)]
    props += [(f"Rule {n}", x) for n, x in enumerate(u.get("rules", []), 1)]
    brief = m.org / u.get("dir", "") / ".myteam" / "project.md"
    return Item("myorg.unit", u["handle"], brief if brief.exists() else None, 1, props=props)


def open_in_myteam(rcp: Context, path: str, view: str = "myteam.overview") -> None:
    """Show this project (the org) in the myteam plugin's views."""
    try:
        root = str((PLUGIN_DIR.parent / "myteam").resolve())
        if root not in sys.path:
            sys.path.insert(0, root)
        from myrcp_myteam.plugin import choose_project  # the myteam plugin's own choice

        choose_project(rcp, path)
        rcp.open_view(view)
    except Exception as e:  # noqa: BLE001 - shown to the user
        rcp.status(f"the myteam plugin cannot show {path}: {e}", "error")


def myorg_console(rcp: Context, m: Model, args: list[str], title: str) -> None:
    rcp.run_in_console([m.exe, *args, "-d", str(m.org)], cwd=m.org, title=title, name="myorg run")


def myorg_job(rcp: Context, m: Model, args: list[str], title: str) -> None:
    def work(_j: Job) -> tuple[bool, str]:
        return command(m.exe, m.org, *args)

    def done(j: Job) -> None:
        ok, said = j.result if j.ok else (False, str(j.error))
        last = said.strip().splitlines()[-1] if said.strip() else ("done" if ok else "failed")
        rcp.status(f"{title}: {last}", "ok" if ok else "error")
        m.reload()

    rcp.run_job(title, work, on_done=done)


_views: weakref.WeakSet[OrgView] = weakref.WeakSet()


def choose_org(rcp: Context, path: str) -> None:
    p = Path(path)
    if not is_org(p):
        rcp.status(f"{p} is not an org (no ORG.md and .myorg/): myorg init makes one", "error")
        return
    save_org(p)
    for v in list(_views):
        v.rebind()
    rcp.status(f"myorg: {p}", "ok")


def org_picker(rcp: Context, here: Path) -> ui.Node:
    items = [ui.item(str(p), p.name, str(p.parent)) for p in candidates()]
    return ui.vbox(
        ui.text([("No org in ", "warning"), (str(here), "bold"),
                 (". Choose one (remembered), or start myrcp in an org:", "warning")], wrap=True),
        ui.listbox("orgs", items, on_activate=lambda key: choose_org(rcp, key),
                   empty="No orgs found nearby: myorg init in a directory makes one.", grow=1),
        grow=1,
    )


class OrgView(View):
    """A view of the shared model: rendered again whenever it changes."""

    def setup(self) -> None:
        self.m = shared(self.rcp)
        self.m.subscribe(self.invalidate)
        _views.add(self)
        self.init()

    def rebind(self) -> None:
        if find_org(self.rcp) == self.m.org:
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

    @property
    def o(self) -> dict[str, Any]:
        return self.m.data.overview

    def banner(self) -> ui.Node | None:
        if not is_org(self.m.org):
            return org_picker(self.rcp, self.m.org)
        if not self.m.loaded:
            return ui.text(f"Loading {self.m.org}…", "dim")
        if self.d.error:
            return ui.text(f"myorg: {self.d.error}", "error", wrap=True)
        return None


# ---------------------------------------------------------------------------------
# palette


@plugin.command("myorg.show")
def show(rcp: Context) -> None:
    rcp.open_view("myorg.overview")


@plugin.command("myorg.org.choose")
def choose(rcp: Context) -> None:
    rcp.open_view("myorg.orgs")


for _vid in ("topology", "activity"):

    def _opener(rcp: Context, _v: str = _vid) -> None:
        rcp.open_view(f"myorg.{_v}")

    plugin.command(f"myorg.view.{_vid}")(_opener)


@plugin.command("myorg.tick")
def tick(rcp: Context) -> None:
    myorg_console(rcp, shared(rcp), ["run", "-n", "1"], "myorg run -n 1")


@plugin.command("myorg.run")
def run(rcp: Context) -> None:
    myorg_console(rcp, shared(rcp), ["run"], "myorg run")


@plugin.command("myorg.stop")
def stop(rcp: Context) -> None:
    m = shared(rcp)
    (m.org / ".myorg" / "stop").touch()
    rcp.status("myorg stops after the tick it is in (.myorg/stop)", "ok")
    m.reload()


@plugin.command("myorg.sync")
def sync(rcp: Context) -> None:
    myorg_job(rcp, shared(rcp), ["sync"], "myorg sync")


@plugin.command("myorg.talk")
def talk(rcp: Context) -> None:
    open_in_myteam(rcp, str(shared(rcp).org), "myteam.customer")


@plugin.command("myorg.crew")
def crew(rcp: Context) -> None:
    open_in_myteam(rcp, str(shared(rcp).org), "myteam.overview")


@plugin.view("myorg.orgs")
class Orgs(View):
    def title(self) -> str:
        return "Orgs"

    def render(self) -> ui.Node:
        now = find_org(self.rcp)
        items = [ui.item(str(p), p.name + ("  (shown now)" if p == now else ""), str(p.parent),
                         "accent" if p == now else None) for p in candidates()]
        return ui.vbox(
            ui.listbox("orgs", items, on_activate=self._choose, empty="No orgs found nearby.", grow=1),
            ui.text([("Enter", "accent"), " show this org in the myorg views (remembered)"], "dim"),
        )

    def _choose(self, key: str) -> None:
        choose_org(self.rcp, key)
        self.rcp.open_view("myorg.overview")
        self.close()


# ---------------------------------------------------------------------------------
# The org


@plugin.view("myorg.overview")
class Overview(OrgView):
    def title(self) -> str:
        return f"Org: {self.o.get('name', '')}" if self.o else "Org"

    def render(self) -> ui.Node:
        if (b := self.banner()) and not self.o:
            return ui.vbox(b)
        o = self.o
        c = o.get("coach", {})
        head = ui.text([(o.get("name", ""), "title"), f"   toward milestone {o.get('milestone', 1)}   ",
                        f"{o.get('ticks', 0)} ticks, {c.get('decisions', 0)} coach decisions, {money(c.get('cost_usd')) or '$0'}   ",
                        (str(self.m.org), "dim")], wrap=True)
        state: list[Any] = []
        if c.get("objection"):
            state += [("The Gatekeeper objects to the coach's decisions", "warning"), "   "]
        if c.get("news"):
            state += [(f"{c['news']} news waiting for the coach", "accent"), "   "]
        if o.get("stop_requested"):
            state += [("stop requested: myorg stops after this tick", "warning")]
        last = self.d.ticks[0] if self.d.ticks else None
        if last and last.get("waiting"):
            state += [("Waiting: ", "dim"), (last.get("waiting_for", ""), "warning")]
        cols = [ui.column("Unit", width=12), ui.column("Type", width=14), ui.column("Experts", grow=2),
                ui.column("Purpose", grow=4), ui.column("Sees", grow=2), ui.column("Hidden", width=7, align="right")]
        rows = []
        for u in o.get("units", []):
            v = u.get("view", {})
            rows.append(ui.row(u["handle"], [[(u["handle"], "bold")], u.get("type", ""),
                                             " ".join("@" + e for e in u.get("experts", [])) or {"text": "nobody", "style": "warning"},
                                             u.get("purpose", ""), short(v.get("readable", [])), str(len(v.get("hide", [])))],
                               selection=unit_item(self.m, u)))
        rules = [[("- ", "dim"), r] for r in o.get("rules", [])]
        return ui.vbox(
            head,
            self.banner() if self.d.error else None,
            ui.text(state, wrap=True) if state else None,
            ui.panel(f"Units ({len(rows)})", ui.table("units", cols, rows, on_activate=self._open,
                                                      empty="No units: myorg add, or edit ORG.md (e)."), grow=2),
            ui.panel("The org's rules (every expert reads them)", ui.log("rules", rules or [[("No rules.", "dim")]]),
                     height=min(10, len(rules) + 2)),
            ui.text([("t", "accent"), " one tick   ", ("r", "accent"), " run until it needs you   ", ("x", "accent"),
                     " stop after this tick   ", ("s", "accent"), " sync   ", ("e", "accent"), " edit ORG.md   ",
                     ("w", "accent"), " the crew's work (myteam)   ", ("Enter", "accent"), " the unit's brief"], "dim"),
        )

    def _open(self, key: str) -> None:
        u = self.d.unit(key)
        brief = self.m.org / (u or {}).get("dir", key) / ".myteam" / "project.md"
        if brief.exists():
            self.rcp.open_in_editor(brief, 1, on_close=lambda _w: self.m.reload())

    @plugin.command("myorg.overview.tick")
    def tick_(self) -> None:
        tick(self.rcp)

    @plugin.command("myorg.overview.run")
    def run_(self) -> None:
        run(self.rcp)

    @plugin.command("myorg.overview.stop")
    def stop_(self) -> None:
        stop(self.rcp)

    @plugin.command("myorg.overview.sync")
    def sync_(self) -> None:
        sync(self.rcp)

    @plugin.command("myorg.overview.edit")
    def edit(self) -> None:
        self.rcp.open_in_editor(self.m.org / "ORG.md", 1, on_close=lambda _w: self.m.reload())

    @plugin.command("myorg.overview.crew")
    def crew_(self) -> None:
        crew(self.rcp)


# ---------------------------------------------------------------------------------
# Topology: what each unit's experts see, and why


@plugin.view("myorg.topology")
class Topology(OrgView):
    def render(self) -> ui.Node:
        o = self.o
        units = o.get("units", [])
        ucols = [ui.column("Unit", width=12), ui.column("Type", width=14), ui.column("Sees", grow=2),
                 ui.column("Writes outside it", grow=2), ui.column("Hidden from it", grow=2)]
        urows = [ui.row(u["handle"], [[(u["handle"], "bold")], u.get("type", ""), short(u.get("view", {}).get("readable", [])),
                                      ", ".join(u.get("view", {}).get("writable", [])), short(u.get("view", {}).get("hide", []))],
                        selection=unit_item(self.m, u)) for u in units]
        icols = [ui.column("Between", width=22), ui.column("Now", width=15), ui.column("Provider", width=10),
                 ui.column("Via", grow=2), ui.column("Until", width=16), ui.column("Note", grow=3)]
        irows = []
        for i, x in enumerate(o.get("interactions", [])):
            now = x.get("now", "")
            until = ""
            if x.get("until"):
                until = f"milestone {x['until']}" + (f", then {x['then']}" if now == x.get("mode") else " (over)")
            irows.append(ui.row(str(i), [" ⇄ ".join(x.get("between", [])),
                                         {"text": now or "ended", "style": MODE_STYLE.get(now, "dim")},
                                         x.get("provider", ""), ", ".join(x.get("via", [])), until, x.get("note", "")]))
        detail = None
        picked = self.rcp.selection.of_type("myorg.unit")
        u = self.d.unit(picked[0].label) if picked else None
        if u and u.get("view", {}).get("notes"):
            lines: list[Any] = [[(f"What {u['handle']}'s experts are told about the others", "bold")]]
            lines += [[("- ", "accent"), n] for n in u["view"]["notes"]]
            detail = ui.panel(u["handle"], ui.log("told", lines), height=min(12, 2 + 2 * len(u["view"]["notes"])))
        return ui.vbox(
            self.banner(),
            ui.panel(f"Units ({len(units)})", ui.table("topology.units", ucols, urows,
                                                       empty="No units: myorg add, or edit ORG.md."), grow=2),
            ui.panel("Interactions", ui.table("topology.interactions", icols, irows,
                                              empty="No interactions: the units see only the contracts."), grow=1),
            detail,
            ui.text([("e", "accent"), " edit ORG.md (myorg sets up what it lists)   ",
                     "collaboration: each reads the other's repository · x-as-a-service: the consumer sees what the "
                     "provider publishes · facilitating: the helper sees the other"], "dim"),
        )

    @plugin.command("myorg.topology.edit")
    def edit(self) -> None:
        self.rcp.open_in_editor(self.m.org / "ORG.md", 1, on_close=lambda _w: self.m.reload())


# ---------------------------------------------------------------------------------
# Units (side) and the ticks (bottom)


@plugin.view("myorg.units")
class Units(OrgView):
    def title(self) -> str:
        return f"Units ({len(self.d.units)})" if self.d.units else "Units"

    def render(self) -> ui.Node:
        items = [ui.item("org", self.o.get("name", "the org") if self.o else "the org", "the crew's work (myteam)", "accent")]
        for u in self.d.units:
            experts = " ".join("@" + e for e in u.get("experts", [])) or "no expert"
            items.append(ui.item(u["handle"], u["handle"], f"{u.get('type', '') or 'unit'} · {experts}",
                                 None if u.get("experts") else "warning", selection=unit_item(self.m, u)))
        return ui.vbox(
            self.banner(),
            ui.listbox("units", items, on_activate=self._open, empty="No units.", grow=1),
            ui.text([("Enter", "accent"), " the org: the myteam views; a unit: its brief"], "dim"),
        )

    def _open(self, key: str) -> None:
        if key == "org":
            crew(self.rcp)
            return
        u = self.d.unit(key)
        brief = self.m.org / (u or {}).get("dir", key) / ".myteam" / "project.md"
        if brief.exists():
            self.rcp.open_in_editor(brief, 1)


@plugin.view("myorg.activity")
class Activity(OrgView):
    def title(self) -> str:
        return "Org activity"

    def render(self) -> ui.Node:
        cols = [ui.column("When", width=12), ui.column("Milestone", width=9, align="right"),
                ui.column("Cost", width=7, align="right"), ui.column("What happened", grow=1)]
        rows = []
        for i, t in enumerate(self.d.ticks):
            notes, problems = t.get("notes", []), t.get("problems", [])
            what: list[Any] = []
            if t.get("reached_milestone"):
                what.append(("milestone reached", "ok"))
            if t.get("waiting"):
                what.append((t.get("waiting_for") or "waiting", "warning"))
            elif problems:
                what.append((problems[0], "error"))
            if notes:
                if what:
                    what.append(("  ", "dim"))
                what.append(("; ".join(notes[:4]) + (f" (+{len(notes) - 4})" if len(notes) > 4 else ""),
                             "normal" if len(what) == 0 else "dim"))
            props = [("When", t.get("time", "")), ("Milestone", t.get("milestone", "")), ("Cost", money(t.get("cost")))]
            props += [(f"Note {n + 1}", x) for n, x in enumerate(notes)]
            props += [(f"Problem {n + 1}", x) for n, x in enumerate(problems)]
            rows.append(ui.row(str(i), [ago(t.get("time", "")), str(t.get("milestone", "")), money(t.get("cost")), what],
                               selection=Item("myorg.tick", f"tick {t.get('time', '')}", None, 0, props=props)))
        return ui.vbox(
            self.banner(),
            ui.table("activity", cols, rows, grow=1, empty="myorg has not run yet: t in the Org view runs one tick."),
        )
