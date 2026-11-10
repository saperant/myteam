# SPDX-License-Identifier: MIT
import datetime as dt

import taskml
from taskml import gantt
from taskml.gantt import ScheduleOptions, schedule

MON = dt.date(2026, 1, 5)  # a Monday


def plan(text, **kw):
    kw.setdefault("start", MON)
    s = schedule(taskml.parse_or_raise(text), ScheduleOptions(**kw))
    return s, {r.title: r for r in s.task_rows}


def test_estimates_and_units():
    s, r = plan("[ ] a ~4h\n[ ] b ~2d\n[ ] c ~1w\n[ ] d")
    assert r["a"].duration_days == 0.5
    assert r["b"].duration_days == 2
    assert r["c"].duration_days == 5
    assert r["d"].duration_days == 1 and not r["d"].estimated
    # Unassigned top-level tasks run in parallel.
    assert all(x.start == 0 for x in r.values())


def test_same_assignee_is_sequential_and_weekends_skipped():
    s, r = plan("[ ] a ~3d @ann\n[ ] b ~3d @ann\n[ ] c ~3d @bob")
    assert (r["a"].start, r["b"].start, r["c"].start) == (0, 3, 0)
    assert r["a"].end_date == dt.date(2026, 1, 7)
    assert r["b"].start_date == dt.date(2026, 1, 8)
    assert r["b"].end_date == dt.date(2026, 1, 12)  # Thu, Fri, (weekend), Mon
    s2, r2 = plan("[ ] a ~3d @ann\n[ ] b ~3d @ann", skip_weekends=False)
    assert r2["b"].end_date == dt.date(2026, 1, 10)
    s3, r3 = plan("[ ] a ~3d @ann\n[ ] b ~3d @ann", level_resources=False)
    assert r3["b"].start == 0


def test_dependencies_including_forward_and_inherited():
    s, r = plan(
        "[ ] deploy ~1d ^deploy\n    depends: ^build\n    [ ] smoke test ~1d\n"
        "[ ] build ~2d ^build\n"
    )
    assert r["build"].start == 0
    assert r["smoke test"].start == 2  # inherits parent's dependency
    assert r["deploy"].is_summary and r["deploy"].start == 2
    idx = {row.title: i for i, row in enumerate(s.rows)}
    assert r["deploy"].predecessors == [idx["build"]]


def test_parent_estimate_is_shared_by_unestimated_children():
    s, r = plan("[ ] parent ~2d\n  [ ] x\n  [ ] y\n  [ ] z ~1d\n")
    assert r["x"].duration_days == 0.5 and r["y"].duration_days == 0.5
    # unassigned subtasks of one tree run sequentially
    assert (r["x"].start, r["y"].start, r["z"].start) == (0, 0.5, 1.0)
    assert r["parent"].end == 2.0


def test_late_and_cycles_and_unknown_deps():
    s, r = plan("[ ] a ~5d !2026-01-07\n[x] b ~5d !2026-01-07")
    assert r["a"].late_days == 2 and r["b"].late_days == 0  # completed tasks are never late
    s, r = plan("[ ] a ^a\n    depends: ^b\n[ ] b ^b\n    depends: ^a\n    depends: ^nope")
    assert any("circular" in w for w in s.warnings)
    assert any("unknown dependency ^nope" in w for w in s.warnings)


def test_default_start_from_view_and_created():
    doc = taskml.parse_or_raise("@created 2026-03-02\n[ ] a\n")
    assert schedule(doc).start == dt.date(2026, 3, 2)
    doc = taskml.parse_or_raise("@created 2026-03-02\n[ ] a\n---view:timeline start=2026-04-01\n")
    assert schedule(doc).start == dt.date(2026, 4, 1)


def test_render_html():
    text = (
        "@project Demo <&>\n== Phase 1 ==\n[x] Design ~2d @ann ^d\n"
        "[ ] Build ~3d @bob ^b !2026-01-06\n    depends: ^d\n    [ ] sub\n[-] Dropped\n"
    )
    out = gantt.generate(text, options=ScheduleOptions(start=MON), today=MON)
    assert out.startswith("<!doctype html>")
    assert "Demo &lt;&amp;&gt;" in out
    assert out.count('class="bar"') == 4
    assert 'class="link"' in out
    assert "late 3d" in out
    assert "Table view" in out and "PHASE 1" not in out  # uppercase is CSS only


def test_cli(tmp_path):
    src = tmp_path / "p.taskml"
    src.write_text("[ ] a ~1d\n", encoding="utf-8")
    assert gantt.main([str(src), "--start", "2026-01-05"]) == 0
    assert (tmp_path / "p.html").read_text(encoding="utf-8").count('class="bar"') == 1
