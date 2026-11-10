# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Saku Rantamäki
"""Gantt chart generator for TaskML documents.

TaskML has estimates (``~4h``, ``~2d``, ``~1w``), dependencies (``depends:``,
``blocked-by:``), assignees and due dates, but no start dates. This module
therefore does two things:

1. :func:`schedule` computes a plan: every leaf task starts as soon as its
   dependencies (including those inherited from its parent tasks) are done
   and, with resource levelling, when its assignee is free. Tasks are
   processed in document order. Parent tasks span their subtasks.
2. :func:`render_html` draws the plan as a self-contained, interactive HTML
   page (inline SVG, hover/focus tooltips, dependency highlighting, light and
   dark mode, and an accessible table view).

Command line::

    python -m taskml.gantt plan.taskml -o plan.html --start 2026-01-05
"""

from __future__ import annotations

import argparse
import datetime as _dt
import html
import json
import math
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence

from . import Document, TaskStatus, parse

__all__ = ["ScheduleOptions", "Row", "Schedule", "schedule", "render_html", "generate", "main"]


# ---------------------------------------------------------------------------
# Scheduling
# ---------------------------------------------------------------------------


@dataclass
class ScheduleOptions:
    """How estimates are turned into dates."""

    #: First day of the project. Default: the ``start=`` option of a
    #: ``---view:timeline`` fence, else the ``@created`` directive, else today.
    start: Optional[_dt.date] = None
    #: Working hours per day, used to convert ``~Nh`` estimates.
    hours_per_day: float = 8.0
    #: Skip Saturdays and Sundays.
    skip_weekends: bool = True
    #: A person works on one task at a time (tasks inherit the assignee of
    #: their parent task when they have none).
    level_resources: bool = True
    #: Duration (working days) assumed for leaf tasks without an estimate.
    default_days: float = 1.0


@dataclass
class Row:
    """One row of the chart: a section header or a scheduled task."""

    kind: str  # "section" or "task"
    title: str
    depth: int = 0
    task: object = None  # taskml.Task for task rows
    status: Optional[str] = None
    assignee: Optional[str] = None  # effective (possibly inherited)
    is_summary: bool = False
    estimated: bool = True
    start: float = 0.0  # working-day offsets from the project start
    end: float = 0.0
    start_date: Optional[_dt.date] = None
    end_date: Optional[_dt.date] = None  # inclusive last working day
    due: Optional[_dt.date] = None
    late_days: int = 0
    duration_days: float = 0.0
    predecessors: List[int] = field(default_factory=list)  # row indices


@dataclass
class Schedule:
    document: Document
    options: ScheduleOptions
    start: _dt.date
    rows: List[Row]
    warnings: List[str]
    _workdays: List[_dt.date] = field(default_factory=list, repr=False)

    # -- calendar --------------------------------------------------------

    def workday(self, k: int) -> _dt.date:
        """The k-th working day (0-based) of the project."""
        days = self._workdays
        if not days:
            d = self.start
            while self.options.skip_weekends and d.weekday() >= 5:
                d += _dt.timedelta(days=1)
            days.append(d)
        while len(days) <= k:
            d = days[-1] + _dt.timedelta(days=1)
            while self.options.skip_weekends and d.weekday() >= 5:
                d += _dt.timedelta(days=1)
            days.append(d)
        return days[k]

    def position(self, offset: float, is_end: bool = False) -> float:
        """Calendar-day position (from ``start``) of a working-day offset."""
        k = int(math.floor(offset + 1e-9))
        frac = offset - k
        if is_end and frac < 1e-9 and k > 0:
            return (self.workday(k - 1) - self.start).days + 1.0
        return (self.workday(k) - self.start).days + max(frac, 0.0)

    def date_of(self, offset: float, is_end: bool = False) -> _dt.date:
        k = int(math.floor(offset + 1e-9))
        if is_end and offset - k < 1e-9 and k > 0:
            return self.workday(k - 1)
        return self.workday(k)

    @property
    def end(self) -> _dt.date:
        ends = [r.end_date for r in self.rows if r.end_date]
        return max(ends) if ends else self.start

    @property
    def task_rows(self) -> List[Row]:
        return [r for r in self.rows if r.kind == "task"]


_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def _parse_date(s: Optional[str]) -> Optional[_dt.date]:
    if not s or not _DATE_RE.match(s.strip()):
        return None
    try:
        return _dt.date.fromisoformat(s.strip())
    except ValueError:
        return None


def _default_start(doc: Document) -> _dt.date:
    if doc.view is not None and doc.view.type == "timeline":
        d = _parse_date(doc.view.options.get("start"))
        if d:
            return d
    d = _parse_date(doc.directives.get("created"))
    return d or _dt.date.today()


def _estimate_days(estimate: Optional[str], opts: ScheduleOptions) -> Optional[float]:
    if not estimate:
        return None
    m = re.fullmatch(r"(\d+)([hdw])", estimate)
    if not m:
        return None
    n, unit = int(m.group(1)), m.group(2)
    if unit == "h":
        return n / opts.hours_per_day
    if unit == "d":
        return float(n)
    return float(n * (5 if opts.skip_weekends else 7))


class _Node:
    __slots__ = ("row", "task", "parent", "children", "assignee", "deps", "done", "finish", "leaves",
                 "resource", "share")

    def __init__(self, row: int, task, parent: Optional["_Node"]):
        self.row = row
        self.task = task
        self.parent = parent
        self.children: List[_Node] = []
        self.assignee: Optional[str] = None
        self.deps: List[_Node] = []
        self.done = False
        self.finish = 0.0
        self.leaves: List[_Node] = []
        self.resource: Optional[str] = None  # who is busy while this runs
        self.share: Optional[float] = None  # duration share of a parent's estimate


def schedule(doc: Document, options: Optional[ScheduleOptions] = None) -> Schedule:
    """Compute start and end dates for every task in ``doc``."""
    opts = options or ScheduleOptions()
    start = opts.start or _default_start(doc)
    sched = Schedule(document=doc, options=opts, start=start, rows=[], warnings=[])
    rows = sched.rows
    nodes: List[_Node] = []
    by_id: Dict[str, _Node] = {}

    # 1. Flatten the document into rows (document order) and build the tree.
    def add_tasks(tasks, depth: int, parent: Optional[_Node]) -> None:
        for t in tasks:
            row = Row(kind="task", title=t.description or "(untitled)", depth=depth, task=t,
                      status=t.status.label, due=_parse_date(t.due))
            rows.append(row)
            node = _Node(len(rows) - 1, t, parent)
            node.assignee = t.assignee or (parent.assignee if parent else None)
            row.assignee = node.assignee
            nodes.append(node)
            if parent:
                parent.children.append(node)
            if t.id:
                by_id.setdefault(t.id, node)
            add_tasks(t.subtasks, depth + 1, node)

    add_tasks(doc.tasks, 0, None)
    for s in doc.sections:
        rows.append(Row(kind="section", title=s.title))
        add_tasks(s.tasks, 0, None)

    # 2. Resolve dependencies (own + inherited from ancestors).
    def ancestors(n: _Node):
        p = n.parent
        while p:
            yield p
            p = p.parent

    for n in nodes:
        ids = list(n.task.depends_on) + list(n.task.blocked_by)
        for a in ancestors(n):
            ids += list(a.task.depends_on) + list(a.task.blocked_by)
        anc = set(id(a) for a in ancestors(n))
        seen = set()
        for dep_id in ids:
            if dep_id in seen:
                continue
            seen.add(dep_id)
            dep = by_id.get(dep_id)
            if dep is None:
                sched.warnings.append(f"{rows[n.row].title!r}: unknown dependency ^{dep_id} ignored")
                continue
            if dep is n or id(dep) in anc:
                continue  # depending on yourself or an ancestor is meaningless
            n.deps.append(dep)

    # Leaves under each node (a leaf's own list is itself).
    for n in reversed(nodes):
        n.leaves = [n] if not n.children else [lf for c in n.children for lf in c.leaves]

    # Resources: the assignee; unassigned subtasks share a virtual owner per
    # top-level task (so they run one after another), and unassigned
    # top-level leaves run in parallel.
    for n in nodes:
        if n.assignee:
            n.resource = "@" + n.assignee
        elif n.parent is not None:
            root = n
            while root.parent is not None:
                root = root.parent
            n.resource = f"#tree{root.row}"

    # A parent's estimate is shared by its unestimated children.
    for n in nodes:
        if not n.children:
            continue
        own = _estimate_days(n.task.estimate, opts)
        if own is None and n.share is not None:
            own = n.share
        if own is None:
            continue
        unest = [c for c in n.children if _estimate_days(c.task.estimate, opts) is None]
        if not unest:
            continue
        known = sum(_estimate_days(c.task.estimate, opts) or 0.0 for c in n.children)
        remaining = own - known
        if remaining > 0:
            for c in unest:
                c.share = remaining / len(unest)

    def dep_ready(dep: _Node, me: _Node) -> bool:
        # A dependency on a task that contains `me` can never be satisfied.
        return all(lf.done or lf is me for lf in dep.leaves)

    def dep_finish(dep: _Node) -> float:
        return max((lf.finish for lf in dep.leaves if lf.done), default=0.0)

    # 3. List scheduling of leaves in document order.
    pending = [n for n in nodes if not n.children]
    free: Dict[str, float] = {}
    while pending:
        pick = next((n for n in pending if all(dep_ready(d, n) for d in n.deps)), None)
        if pick is None:  # dependency cycle: relax the first blocked task
            pick = pending[0]
            stuck = [d for d in pick.deps if not dep_ready(d, pick)]
            sched.warnings.append(
                f"{rows[pick.row].title!r}: circular dependency on "
                + ", ".join("^" + (d.task.id or "?") for d in stuck) + " ignored")
            pick.deps = [d for d in pick.deps if d not in stuck]
        pending.remove(pick)

        row = rows[pick.row]
        est = _estimate_days(pick.task.estimate, opts)
        if est is None and pick.share is not None:
            est = pick.share
        row.estimated = est is not None
        dur = est if est is not None else opts.default_days
        begin = max([0.0] + [dep_finish(d) for d in pick.deps])
        cancelled = pick.task.status == TaskStatus.CANCELLED
        res = pick.resource if opts.level_resources and not cancelled else None
        if res:
            begin = max(begin, free.get(res, 0.0))
        pick.finish = begin + dur
        pick.done = True
        if res:
            free[res] = pick.finish
        row.start, row.end, row.duration_days = begin, pick.finish, dur

    # 4. Parent spans, dates, lateness, predecessor links.
    for n in reversed(nodes):  # children before parents
        row = rows[n.row]
        if n.children:
            row.is_summary = True
            row.start = min(rows[c.row].start for c in n.children)
            row.end = max(rows[c.row].end for c in n.children)
            row.duration_days = row.end - row.start
            est = _estimate_days(n.task.estimate, opts)
            row.estimated = all(rows[c.row].estimated for c in n.children)
            if est is not None:
                row.estimated = True
        row.start_date = sched.date_of(row.start)
        row.end_date = sched.date_of(row.end, is_end=True)
        if row.end_date < row.start_date:
            row.end_date = row.start_date
        if row.due and row.end_date > row.due and n.task.status not in (TaskStatus.COMPLETED, TaskStatus.CANCELLED):
            row.late_days = (row.end_date - row.due).days
        own = list(n.task.depends_on) + list(n.task.blocked_by)
        row.predecessors = [d.row for d in n.deps if d.task.id in own]
    return sched


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

_STATUS_LABEL = {
    "pending": "Pending",
    "in_progress": "In progress",
    "completed": "Completed",
    "blocked": "Blocked",
    "cancelled": "Cancelled",
    "review": "Review",
}
_STATUS_ORDER = ["completed", "in_progress", "review", "blocked", "pending", "cancelled"]

_ROW_H = 30
_BAR_H = 16
_SUMMARY_H = 6
_AXIS_H = 44
_PAD_X = 12


def _fmt_date(d: Optional[_dt.date]) -> str:
    return d.strftime("%a %d %b %Y") if d else "—"


def _fmt_days(x: float, hours_per_day: float) -> str:
    if x <= 0:
        return "0d"
    if x < 1:
        return f"{round(x * hours_per_day, 1):g}h"
    return f"{round(x, 1):g}d"


def _esc(s) -> str:
    return html.escape(str(s), quote=True)


_CSS = """
.gantt { color-scheme: light;
  --surface-1:#fcfcfb; --page:#f9f9f7; --text-primary:#0b0b0b; --text-secondary:#52514e;
  --text-muted:#898781; --grid:#e1e0d9; --axis:#c3c2b7; --border:rgba(11,11,11,0.10);
  --weekend:rgba(11,11,11,0.035); --summary:#898781;
  --s-in_progress:#2a78d6; --s-completed:#1baf7a; --s-review:#eda100; --s-blocked:#d03b3b;
  --s-pending:#b5b3ab; --s-cancelled:#d6d4cc; --warning:#fab219; --link:#898781; }
@media (prefers-color-scheme: dark) { :root:where(:not([data-theme="light"])) .gantt { color-scheme: dark;
  --surface-1:#1a1a19; --page:#0d0d0d; --text-primary:#f0efec; --text-secondary:#c3c2b7;
  --text-muted:#898781; --grid:#2c2c2a; --axis:#383835; --border:rgba(255,255,255,0.10);
  --weekend:rgba(255,255,255,0.035); --summary:#898781;
  --s-in_progress:#3987e5; --s-completed:#199e70; --s-review:#c98500; --s-blocked:#d03b3b;
  --s-pending:#62615c; --s-cancelled:#3d3d3a; --link:#77756f; } }
:root[data-theme="dark"] .gantt { color-scheme: dark;
  --surface-1:#1a1a19; --page:#0d0d0d; --text-primary:#f0efec; --text-secondary:#c3c2b7;
  --text-muted:#898781; --grid:#2c2c2a; --axis:#383835; --border:rgba(255,255,255,0.10);
  --weekend:rgba(255,255,255,0.035); --summary:#898781;
  --s-in_progress:#3987e5; --s-completed:#199e70; --s-review:#c98500; --s-blocked:#d03b3b;
  --s-pending:#62615c; --s-cancelled:#3d3d3a; --link:#77756f; }
body { margin:0; background:var(--page, #f9f9f7); }
.gantt { background:var(--page); color:var(--text-primary); min-height:100vh; box-sizing:border-box;
  padding:24px 16px; font:14px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif; }
.gantt * { box-sizing:border-box; }
.g-head { max-width:1400px; margin:0 auto 16px; }
.g-head h1 { font-size:20px; font-weight:600; margin:0 0 4px; }
.g-sub { color:var(--text-secondary); margin:0; }
.g-stats { display:flex; flex-wrap:wrap; gap:8px 28px; margin:16px 0 0; padding:0; list-style:none; }
.g-stats li { display:flex; flex-direction:column; }
.g-stats .k { color:var(--text-secondary); font-size:12px; }
.g-stats .v { font-size:18px; font-weight:600; }
.g-legend { display:flex; flex-wrap:wrap; gap:6px 18px; margin:16px 0 0; padding:0; list-style:none;
  color:var(--text-secondary); font-size:12px; }
.g-legend li { display:flex; align-items:center; gap:6px; }
.sw { width:12px; height:12px; border-radius:3px; display:inline-block; flex:none; }
.sw.noest { background:var(--s-in_progress); box-shadow:inset 0 0 0 2px var(--s-in_progress), inset 0 0 0 6px color-mix(in srgb, var(--surface-1) 55%, transparent); }
.sw.summary { height:5px; border-radius:2px; background:var(--summary); }
.sw.due { width:9px; height:9px; border-radius:1px; transform:rotate(45deg); background:var(--text-primary); }
.sw.late { width:9px; height:9px; border-radius:1px; transform:rotate(45deg); background:var(--warning); }
.g-card { max-width:1400px; margin:0 auto; background:var(--surface-1); border:1px solid var(--border);
  border-radius:8px; overflow:hidden; }
.g-scroll { overflow:auto; max-height:80vh; position:relative; }
.g-grid { display:grid; grid-template-columns:var(--label-w) max-content; position:relative; }
.g-labels { position:sticky; left:0; z-index:2; background:var(--surface-1); border-right:1px solid var(--grid); }
.g-corner { position:sticky; top:0; z-index:3; height:44px; background:var(--surface-1);
  border-bottom:1px solid var(--axis); display:flex; align-items:flex-end; padding:0 12px 6px;
  color:var(--text-muted); font-size:12px; }
.g-axis { position:sticky; top:0; z-index:1; background:var(--surface-1); display:block; }
.g-label { height:30px; display:flex; align-items:center; gap:6px; padding-right:10px; white-space:nowrap;
  overflow:hidden; font-size:13px; border-bottom:1px solid transparent; }
.g-label .name { overflow:hidden; text-overflow:ellipsis; min-width:0; flex:1 1 auto; }
.g-label.summary .name { font-weight:600; }
.g-label.section { font-weight:600; font-size:12px; text-transform:uppercase; letter-spacing:.04em;
  color:var(--text-secondary); padding-left:12px; }
.g-label .who { color:var(--text-secondary); font-size:12px; flex:none; }
.g-label .flag { color:var(--text-secondary); font-size:12px; flex:none; }
.g-label .flag b { color:var(--warning); font-weight:700; }
.g-label.cancelled .name { text-decoration:line-through; color:var(--text-muted); }
.g-label.hl, .g-label.hl-dep { background:var(--weekend); }
.g-chart text { font-family:inherit; }
.bar { cursor:default; outline:none; }
.bar:focus-visible rect.hit { stroke:var(--text-primary); stroke-width:2; }
.link { fill:none; stroke:var(--link); stroke-width:1; opacity:.45; }
.link.on { opacity:1; stroke:var(--text-primary); stroke-width:1.5; }
.g-chart.focus .bar:not(.on) { opacity:.35; }
.g-tip { position:fixed; z-index:10; pointer-events:none; background:var(--surface-1); color:var(--text-primary);
  border:1px solid var(--border); border-radius:8px; box-shadow:0 4px 16px rgba(0,0,0,.12);
  padding:10px 12px; font-size:12px; max-width:340px; display:none; }
.g-tip .t { font-weight:600; font-size:13px; margin-bottom:6px; }
.g-tip dl { display:grid; grid-template-columns:auto 1fr; gap:2px 12px; margin:0; }
.g-tip dt { color:var(--text-secondary); }
.g-tip dd { margin:0; font-variant-numeric:tabular-nums; }
.g-tip .st { display:inline-flex; align-items:center; gap:6px; }
.g-warn { max-width:1400px; margin:12px auto 0; color:var(--text-secondary); font-size:12px; }
details.g-table { max-width:1400px; margin:16px auto 0; }
details.g-table summary { cursor:pointer; color:var(--text-secondary); font-size:13px; }
.g-table table { border-collapse:collapse; width:100%; margin-top:8px; font-size:12px; background:var(--surface-1); }
.g-table th, .g-table td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--grid); white-space:nowrap; }
.g-table th { color:var(--text-secondary); font-weight:600; }
.g-table td.num { font-variant-numeric:tabular-nums; }
"""

_JS = """
(() => {
  const root = document.currentScript.closest('.gantt');
  const data = JSON.parse(root.querySelector('script[type="application/json"]').textContent);
  const tip = root.querySelector('.g-tip');
  const chart = root.querySelector('.g-chart');
  const labels = root.querySelectorAll('.g-label[data-i]');
  const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
  function show(i, x, y) {
    const r = data[i];
    const rows = [['Status', `<span class="st"><span class="sw" style="background:var(--s-${r.status})"></span>${esc(r.statusLabel)}</span>`],
                  ['Start', esc(r.start)], ['End', esc(r.end)], ['Duration', esc(r.duration)]];
    if (r.estimate) rows.push(['Estimate', esc(r.estimate)]);
    else if (!r.summary) rows.push(['Estimate', 'none (assumed ' + esc(r.duration) + ')']);
    if (r.assignee) rows.push(['Assignee', '@' + esc(r.assignee) + (r.inherited ? ' (from parent)' : '')]);
    if (r.priority !== null) rows.push(['Priority', 'p' + r.priority]);
    if (r.due) rows.push(['Due', esc(r.due) + (r.late ? ` <b style="color:var(--warning)">⚠</b> late by ${r.late}d` : '')]);
    if (r.deps.length) rows.push(['After', r.deps.map(esc).join(', ')]);
    if (r.id) rows.push(['ID', '^' + esc(r.id)]);
    tip.innerHTML = `<div class="t">${esc(r.title)}</div><dl>${rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('')}</dl>`;
    tip.style.display = 'block';
    place(x, y);
  }
  function place(x, y) {
    const w = tip.offsetWidth, h = tip.offsetHeight;
    let left = x + 14, top = y + 14;
    if (left + w > innerWidth - 8) left = x - w - 14;
    if (top + h > innerHeight - 8) top = y - h - 14;
    tip.style.left = Math.max(8, left) + 'px'; tip.style.top = Math.max(8, top) + 'px';
  }
  function highlight(i, on) {
    const r = data[i];
    chart.classList.toggle('focus', on);
    const related = new Set([i, ...r.pred, ...r.succ]);
    chart.querySelectorAll('.bar').forEach(b => b.classList.toggle('on', on && related.has(+b.dataset.i)));
    chart.querySelectorAll('.link').forEach(l => l.classList.toggle('on', on && (+l.dataset.to === i || +l.dataset.from === i)));
    labels.forEach(l => { const j = +l.dataset.i; l.classList.toggle('hl', on && j === i); l.classList.toggle('hl-dep', on && j !== i && related.has(j)); });
  }
  function hide(i) { tip.style.display = 'none'; highlight(i, false); }
  chart.querySelectorAll('.bar').forEach(b => {
    const i = +b.dataset.i;
    b.addEventListener('pointerenter', e => { highlight(i, true); show(i, e.clientX, e.clientY); });
    b.addEventListener('pointermove', e => place(e.clientX, e.clientY));
    b.addEventListener('pointerleave', () => hide(i));
    b.addEventListener('focus', () => { const r = b.getBoundingClientRect(); highlight(i, true); show(i, r.right, r.top); });
    b.addEventListener('blur', () => hide(i));
  });
  labels.forEach(l => {
    const i = +l.dataset.i;
    l.addEventListener('pointerenter', e => { highlight(i, true); show(i, e.clientX, e.clientY); });
    l.addEventListener('pointermove', e => place(e.clientX, e.clientY));
    l.addEventListener('pointerleave', () => hide(i));
  });
})();
"""


def render_html(sched: Schedule, *, title: Optional[str] = None, today: Optional[_dt.date] = None,
                label_width: int = 340, px_per_day: Optional[float] = None,
                show_links: bool = True) -> str:
    """Render a schedule as a standalone HTML page."""
    doc = sched.document
    rows = sched.rows
    task_rows = sched.task_rows
    today = today or _dt.date.today()

    # Time range (calendar days from sched.start).
    end_pos = max([sched.position(r.end, True) for r in task_rows] + [1.0])
    # Due dates extend the axis forward (up to a year past the plan).
    due_pos = [(r.due - sched.start).days + 1 for r in task_rows
               if r.due and 0 <= (r.due - sched.start).days <= end_pos + 365]
    view_end = None
    if doc.view is not None and doc.view.type == "timeline":
        view_end = _parse_date(doc.view.options.get("end"))
    span_end = max([end_pos] + due_pos + ([(view_end - sched.start).days + 1] if view_end else []))
    total_days = int(math.ceil(span_end)) + 2
    # Start the axis on a Monday for clean week ticks.
    lead = sched.start.weekday()
    origin = sched.start - _dt.timedelta(days=lead)
    total_days += lead
    if px_per_day is None:
        px_per_day = max(6.0, min(160.0, 1000.0 / max(total_days, 1)))
    ppd = float(px_per_day)

    def X(cal_pos: float) -> float:  # calendar position (from sched.start) -> px
        return _PAD_X + (cal_pos + lead) * ppd

    width = int(_PAD_X * 2 + total_days * ppd)
    height = len(rows) * _ROW_H + 8

    # --- axis (sticky header SVG) -------------------------------------------
    ax: List[str] = []
    ax.append(f'<line x1="0" x2="{width}" y1="{_AXIS_H - 0.5}" y2="{_AXIS_H - 0.5}" stroke="var(--axis)"/>')
    day_labels = ppd >= 20
    for i in range(total_days + 1):
        d = origin + _dt.timedelta(days=i)
        x = _PAD_X + i * ppd
        if d.day == 1 or i == 0:
            label = d.strftime("%b %Y")
            ax.append(f'<text x="{x + 4:.1f}" y="14" font-size="12" font-weight="600" fill="var(--text-primary)">{label}</text>')
            if d.day == 1:
                ax.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="4" y2="{_AXIS_H}" stroke="var(--axis)"/>')
        if day_labels and i < total_days:
            ax.append(f'<text x="{x + ppd / 2:.1f}" y="39" font-size="11" text-anchor="middle" '
                      f'fill="var(--text-muted)">{d.day}</text>')
        elif d.weekday() == 0 and i < total_days:
            ax.append(f'<text x="{x + 3:.1f}" y="39" font-size="11" fill="var(--text-muted)">{d.day}</text>')
    tpos = (today - sched.start).days
    today_x = X(tpos) if -lead <= tpos <= total_days - lead else None
    if today_x is not None:
        ax.append(f'<line x1="{today_x:.1f}" x2="{today_x:.1f}" y1="20" y2="{_AXIS_H}" stroke="var(--text-secondary)"/>'
                  f'<text x="{today_x + 4:.1f}" y="27" font-size="10" fill="var(--text-secondary)">Today</text>')
    axis_svg = (f'<svg class="g-axis" width="{width}" height="{_AXIS_H}" role="presentation" '
                f'aria-hidden="true">{"".join(ax)}</svg>')

    # --- body ---------------------------------------------------------------
    g: List[str] = []
    # weekends and week gridlines
    for i in range(total_days):
        d = origin + _dt.timedelta(days=i)
        x = _PAD_X + i * ppd
        if sched.options.skip_weekends and d.weekday() >= 5:
            g.append(f'<rect x="{x:.1f}" y="0" width="{ppd:.1f}" height="{height}" fill="var(--weekend)"/>')
        if d.weekday() == 0:
            g.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="0" y2="{height}" stroke="var(--grid)"/>')
    # section separators
    for idx, r in enumerate(rows):
        if r.kind == "section" and idx > 0:
            y = idx * _ROW_H + 0.5
            g.append(f'<line x1="0" x2="{width}" y1="{y}" y2="{y}" stroke="var(--grid)"/>')
    if today_x is not None:
        g.append(f'<line x1="{today_x:.1f}" x2="{today_x:.1f}" y1="0" y2="{height}" '
                 f'stroke="var(--text-secondary)" stroke-width="1"/>')

    def bar_box(i: int, r: Row):
        x0 = X(sched.position(r.start))
        x1 = X(sched.position(r.end, True))
        if x1 - x0 < 3:
            x1 = x0 + 3
        h = _SUMMARY_H if r.is_summary else _BAR_H
        y = i * _ROW_H + (_ROW_H - h) / 2
        return x0, x1, y, h

    # dependency links (drawn under bars)
    succ: Dict[int, List[int]] = {i: [] for i in range(len(rows))}
    if show_links:
        for i, r in enumerate(rows):
            for p in r.predecessors:
                succ[p].append(i)
                _, px1, _, _ = bar_box(p, rows[p])
                sx0, _, _, _ = bar_box(i, r)
                py = p * _ROW_H + _ROW_H / 2
                sy = i * _ROW_H + _ROW_H / 2
                elbow = px1 + 6
                if sx0 - 6 >= elbow:
                    path = f"M{px1:.1f},{py:.1f} H{elbow:.1f} V{sy:.1f} H{sx0 - 1:.1f}"
                else:  # successor starts before the predecessor ends (e.g. parent link)
                    mid = (p * _ROW_H + _ROW_H) if sy > py else p * _ROW_H
                    path = (f"M{px1:.1f},{py:.1f} H{elbow:.1f} V{mid:.1f} H{sx0 - 8:.1f} "
                            f"V{sy:.1f} H{sx0 - 1:.1f}")
                g.append(f'<path class="link" data-from="{p}" data-to="{i}" d="{path}" '
                         f'marker-end="url(#g-arrow)"/>')

    data = []
    for i, r in enumerate(rows):
        if r.kind != "task":
            data.append(None)
            continue
        t = r.task
        x0, x1, y, h = bar_box(i, r)
        status = r.status or "pending"
        name = f"{r.title} — {_STATUS_LABEL[status]}, {_fmt_date(r.start_date)} to {_fmt_date(r.end_date)}"
        if r.late_days:
            name += f", late by {r.late_days} days"
        parts = [f'<g class="bar" data-i="{i}" tabindex="0" role="img" aria-label="{_esc(name)}">',
                 # generous hit target, invisible
                 f'<rect class="hit" x="{x0 - 2:.1f}" y="{i * _ROW_H + 3}" width="{x1 - x0 + 4:.1f}" '
                 f'height="{_ROW_H - 6}" rx="5" fill="transparent"/>']
        if r.is_summary:
            parts.append(f'<rect class="fill" x="{x0:.1f}" y="{y:.1f}" width="{x1 - x0:.1f}" height="{h}" '
                         f'rx="2" fill="var(--summary)"/>')
            # end caps make the span read as a bracket
            for cx in (x0, x1 - 2):
                parts.append(f'<rect x="{cx:.1f}" y="{y:.1f}" width="2" height="{h + 4}" fill="var(--summary)"/>')
        else:
            fill = f"var(--s-{status})"
            parts.append(f'<rect class="fill" x="{x0:.1f}" y="{y:.1f}" width="{x1 - x0:.1f}" height="{h}" '
                         f'rx="4" fill="{fill}"/>')
            if not r.estimated:
                # unestimated: a surface-coloured inset marks the assumed length
                parts.append(f'<rect x="{x0 + 2:.1f}" y="{y + 2:.1f}" width="{max(x1 - x0 - 4, 0):.1f}" '
                             f'height="{h - 4}" rx="2" fill="var(--surface-1)" opacity=".55"/>')
        due_in_range = r.due and -lead <= (r.due - sched.start).days < total_days - lead
        if due_in_range:
            dx = X((r.due - sched.start).days + 1)  # end of the due day
            cy = i * _ROW_H + _ROW_H / 2
            col = "var(--warning)" if r.late_days else "var(--text-primary)"
            parts.append(f'<rect x="{dx - 4:.1f}" y="{cy - 4:.1f}" width="8" height="8" rx="1" fill="{col}" '
                         f'stroke="var(--surface-1)" stroke-width="2" transform="rotate(45 {dx:.1f} {cy:.1f})"/>')
        parts.append("</g>")
        g.append("".join(parts))

        deps = [rows[p].task.id and "^" + rows[p].task.id or rows[p].title for p in r.predecessors]
        data.append({
            "title": r.title, "status": status, "statusLabel": _STATUS_LABEL[status],
            "start": _fmt_date(r.start_date), "end": _fmt_date(r.end_date),
            "duration": _fmt_days(r.duration_days, sched.options.hours_per_day),
            "estimate": t.estimate, "summary": r.is_summary,
            "assignee": r.assignee, "inherited": bool(r.assignee and not t.assignee),
            "priority": t.priority, "due": _fmt_date(r.due) if r.due else None, "late": r.late_days,
            "deps": deps, "id": t.id, "pred": r.predecessors, "succ": succ[i],
        })

    defs = ('<defs><marker id="g-arrow" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="6" markerHeight="6" '
            'orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="var(--link)"/></marker></defs>')
    body_svg = (f'<svg class="g-chart" width="{width}" height="{height}" aria-label="Gantt chart" '
                f'role="group">{defs}{"".join(g)}</svg>')

    # --- label column ---------------------------------------------------------
    lab: List[str] = []
    for i, r in enumerate(rows):
        if r.kind == "section":
            lab.append(f'<div class="g-label section">{_esc(r.title)}</div>')
            continue
        status = r.status or "pending"
        cls = "g-label" + (" summary" if r.is_summary else "") + (" cancelled" if status == "cancelled" else "")
        indent = 12 + r.depth * 16
        who = f'<span class="who">@{_esc(r.assignee)}</span>' if r.task.assignee else ""
        flag = (f'<span class="flag"><b aria-hidden="true">⚠</b> late {r.late_days}d</span>'
                if r.late_days else "")
        lab.append(
            f'<div class="{cls}" data-i="{i}" style="padding-left:{indent}px" '
            f'title="{_esc(r.title)} — {_STATUS_LABEL[status]}">'
            f'<span class="sw" style="background:var(--s-{status})" aria-hidden="true"></span>'
            f'<span class="name">{_esc(r.title)}</span>{who}{flag}</div>')

    # --- header, legend, stats ------------------------------------------------
    project = title or doc.directives.get("project") or "Project plan"
    sub_bits = []
    if doc.directives.get("sprint"):
        sub_bits.append(_esc(doc.directives.get("sprint")))
    sub_bits.append(f"{_fmt_date(sched.start)} – {_fmt_date(sched.end)}")
    leaves = [r for r in task_rows if not r.is_summary]
    counts = {s: sum(1 for r in leaves if r.status == s) for s in _STATUS_ORDER}
    done_pct = round(100 * counts["completed"] / max(len(leaves) - counts["cancelled"], 1))
    late = [r for r in task_rows if r.late_days]
    unestimated = sum(1 for r in leaves if not r.estimated)
    stats = [("Tasks", str(len(leaves))), ("Completed", f"{done_pct}%"),
             ("Planned finish", sched.end.strftime("%d %b %Y")), ("Late vs due date", str(len(late)))]
    stats_html = "".join(f'<li><span class="k">{k}</span><span class="v">{_esc(v)}</span></li>' for k, v in stats)
    legend = [f'<li><span class="sw" style="background:var(--s-{s})"></span>{_STATUS_LABEL[s]} ({counts[s]})</li>'
              for s in _STATUS_ORDER if counts[s]]
    legend.append('<li><span class="sw summary"></span>Parent task span</li>')
    if any(r.due for r in task_rows):
        legend.append('<li><span class="sw due"></span>Due date</li>')
    if late:
        legend.append('<li><span class="sw late"></span>Due date missed by plan</li>')
    if unestimated:
        legend.append(f'<li><span class="sw noest"></span>No estimate '
                      f'(assumed {_fmt_days(sched.options.default_days, sched.options.hours_per_day)})</li>')

    warn_html = ""
    if sched.warnings:
        warn_html = ('<div class="g-warn"><strong>Scheduling notes:</strong> '
                     + "; ".join(_esc(w) for w in sched.warnings) + "</div>")

    # --- table view -------------------------------------------------------------
    trs = []
    for r in rows:
        if r.kind == "section":
            trs.append(f'<tr><th colspan="9">{_esc(r.title)}</th></tr>')
            continue
        t = r.task
        deps = ", ".join("^" + (rows[p].task.id or "?") for p in r.predecessors)
        trs.append(
            "<tr>"
            f'<td style="padding-left:{8 + 14 * r.depth}px">{_esc(r.title)}</td>'
            f"<td>{_STATUS_LABEL[r.status]}</td><td>{_esc('@' + r.assignee) if r.assignee else ''}</td>"
            f'<td class="num">{r.start_date.isoformat()}</td><td class="num">{r.end_date.isoformat()}</td>'
            f'<td class="num">{_fmt_days(r.duration_days, sched.options.hours_per_day)}</td>'
            f'<td class="num">{r.due.isoformat() if r.due else ""}</td>'
            f'<td class="num">{f"{r.late_days}d" if r.late_days else ""}</td><td>{_esc(deps)}</td></tr>')
    table = ('<details class="g-table"><summary>Table view</summary><table><thead><tr>'
             '<th>Task</th><th>Status</th><th>Assignee</th><th>Start</th><th>End</th><th>Duration</th>'
             '<th>Due</th><th>Late</th><th>After</th></tr></thead><tbody>' + "".join(trs) + "</tbody></table></details>")

    json_data = json.dumps(data).replace("</", "<\\/")
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{_esc(project)} – Gantt</title>
<style>{_CSS}</style>
</head>
<body>
<div class="gantt" style="--label-w:{int(label_width)}px">
  <header class="g-head">
    <h1>{_esc(project)}</h1>
    <p class="g-sub">{" · ".join(sub_bits)}</p>
    <ul class="g-stats">{stats_html}</ul>
    <ul class="g-legend" aria-label="Legend">{"".join(legend)}</ul>
  </header>
  <div class="g-card">
    <div class="g-scroll">
      <div class="g-grid">
        <div class="g-labels"><div class="g-corner">Task</div>{"".join(lab)}</div>
        <div>{axis_svg}{body_svg}</div>
      </div>
    </div>
  </div>
  {warn_html}
  {table}
  <div class="g-tip" role="tooltip"></div>
  <script type="application/json">{json_data}</script>
  <script>{_JS}</script>
</div>
</body>
</html>
"""


def generate(text: str, *, options: Optional[ScheduleOptions] = None, **render_kwargs) -> str:
    """Parse TaskML text, schedule it, and return the Gantt chart HTML."""
    doc = parse(text).document
    return render_html(schedule(doc, options), **render_kwargs)


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _date_arg(s: str) -> _dt.date:
    try:
        return _dt.date.fromisoformat(s)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected YYYY-MM-DD, got {s!r}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="taskml-gantt",
        description="Generate an interactive HTML Gantt chart from a TaskML file.")
    ap.add_argument("input", help="TaskML file ('-' for stdin)")
    ap.add_argument("-o", "--output", help="output HTML file (default: <input>.html, or stdout for stdin)")
    ap.add_argument("--start", type=_date_arg,
                    help="project start date (default: ---view:timeline start=, @created, or today)")
    ap.add_argument("--today", type=_date_arg, help="date of the 'Today' marker (default: today)")
    ap.add_argument("--hours-per-day", type=float, default=8.0, help="working hours per day (default 8)")
    ap.add_argument("--default-days", type=float, default=1.0,
                    help="assumed duration of tasks without an estimate (default 1)")
    ap.add_argument("--include-weekends", action="store_true", help="schedule work on Saturdays and Sundays")
    ap.add_argument("--no-leveling", action="store_true",
                    help="let one assignee work on several tasks at the same time")
    ap.add_argument("--no-links", action="store_true", help="do not draw dependency arrows")
    ap.add_argument("--day-width", type=float, help="pixels per day (default: fit ~1000px)")
    ap.add_argument("--title", help="chart title (default: @project)")
    args = ap.parse_args(argv)

    if args.input == "-":
        text = sys.stdin.read()
    else:
        with open(args.input, encoding="utf-8") as f:
            text = f.read()
    result = parse(text)
    for d in result.errors:
        print(d.format(text), file=sys.stderr)

    opts = ScheduleOptions(start=args.start, hours_per_day=args.hours_per_day,
                           skip_weekends=not args.include_weekends,
                           level_resources=not args.no_leveling, default_days=args.default_days)
    sched = schedule(result.document, opts)
    for w in sched.warnings:
        print(f"warning: {w}", file=sys.stderr)
    out = render_html(sched, title=args.title, today=args.today, px_per_day=args.day_width,
                      show_links=not args.no_links)

    target = args.output or (None if args.input == "-" else re.sub(r"\.(taskml|tml)$", "", args.input) + ".html")
    if target is None or target == "-":
        sys.stdout.write(out)
    else:
        with open(target, "w", encoding="utf-8") as f:
            f.write(out)
        n = len(sched.task_rows)
        print(f"wrote {target}: {n} tasks, {sched.start} – {sched.end}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
