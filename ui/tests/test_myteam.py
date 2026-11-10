"""The myteam companions over the real protocol, on a small crew and the real myteam."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from conftest import Peer

MYTEAM = os.environ.get("MYTEAM") or shutil.which("myteam") or str(
    Path(__file__).resolve().parents[2] / "build" / "myteam"
)
pytestmark = pytest.mark.skipif(not os.access(MYTEAM, os.X_OK), reason="myteam is not built")


def find(tree, pred):
    if isinstance(tree, dict):
        if pred(tree):
            return tree
        kids = tree.get("children", []) + [tree[k] for k in ("child", "dialog") if k in tree]
        for c in kids:
            if (hit := find(c, pred)) is not None:
                return hit
    return None


def widget(tree, id):
    return find(tree, lambda n: n.get("id") == id)


TEAM = {
    "team.md": "---\nid: crew\nname: Testers\ncoach: coach\ngatekeeper: gatekeeper\nmodels: {default: claude:sonnet}\n---\n",
    "members/coach.md": "---\nname: Coach\nrole: coach\n---\nDecide.\n",
    "members/gatekeeper.md": "---\nname: Gatekeeper\nrole: gatekeeper\n---\nDoubt.\n",
    "members/dev.md": "---\nname: Dev\nrole: developer\nunit: app\nsummary: writes the code\n---\nCode.\n",
}
PLAN = """---
project: tally
goal: Count the lines
---

# tally

## Work

- [~] Count lines @dev ^count ~2h #p1
  - ( ) It prints 2
  - (x) A test covers it
- [ ] Add words @dev ^words
  - depends: ^count
- [ ] Get the API key @operator ^key
- [x] Set up @dev ^setup
"""
STREAM = [
    {"type": "assistant", "message": {"content": [{"type": "text", "text": "Looking at the code."}]}},
    {"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t1", "name": "Bash",
                                                   "input": {"command": "python3 -m pytest -q"}}]}},
    {"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t1", "content": "1 passed"}]}},
]
CONVERSATION = """# The customer and the coach

### Customer · 2026-11-10T08:00:00Z

Count the lines, please.

### Coach (Coach) · 2026-11-10T08:05:00Z

Dev counts them; a word count after. Is a word anything between spaces?
"""
COACH_STATE = {
    "news": ["^setup accepted by the Gatekeeper: the script runs"],
    "customer_seen": 2, "seen_tasks": [], "objection": "No criterion for the word count.",
    "held": "<task to=\"dev\" id=\"words2\">\nCount words\n</task>\n", "idle": 0, "steps": 4, "decisions": 2,
    "milestones": 0, "cost": 1.25, "last_decision": "2026-11-10T08:05:00Z",
}
COUNCIL = """# Council: words

2026-11-10T08:04:00Z

## The question

What is a word?

## @dev

Anything between spaces.

## @gatekeeper

Unproven: punctuation is not covered.
"""


def project(tmp_path: Path) -> Path:
    team = tmp_path / "team"
    for rel, text in TEAM.items():
        (team / rel).parent.mkdir(parents=True, exist_ok=True)
        (team / rel).write_text(text)
    work = tmp_path / "work"
    state = work / ".myteam"
    (state / "customer").mkdir(parents=True)
    (state / "config.yaml").write_text(f"team: {team}\nmethodology: coach\n")
    (work / "app" / ".myteam").mkdir(parents=True)
    (work / "PLAN.md").write_text(PLAN)
    (work / "CHANGELOG.md").write_text(
        "# Changelog\n\n## 2026-11-06T22:52:36Z · Count lines (^count)\n\n- Team: Testers; member: Dev (@dev), "
        "claude:sonnet\n- Result: [~] in progress\n- Commit: `abc` on main: Count (1 files)\n"
    )
    (state / "customer" / "conversation.md").write_text(CONVERSATION)
    (state / "coach").mkdir()
    (state / "coach" / "state.json").write_text(json.dumps(COACH_STATE))
    (state / "council").mkdir()
    (state / "council" / "20261110-080400-words.md").write_text(COUNCIL)
    (state / "memory").mkdir()
    (state / "memory" / "dev.md").write_text(
        "## From the coach\n\nTest first.\n\n## Lessons\n\n- 2026-11-10 ^setup: use the venv\n")
    (state / "memory" / "coach.md").write_text("Words come after lines.\n")
    # A run in progress: its runner (this test's process) is alive, it has not finished.
    run = state / "runs" / "20261106-225223-dev-count-lines"
    run.mkdir(parents=True)
    (run / "meta.json").write_text(json.dumps({"runner": "claude", "model": "sonnet", "member": "dev",
                                               "what": "^count", "started": "2026-11-06T22:52:23Z",
                                               "pid": os.getpid()}))
    (run / "stream.jsonl").write_text("".join(json.dumps(x) + "\n" for x in STREAM[:2]))
    # The coach's last decision, and the Gatekeeper's review of it.
    for name, member, what, desc in (("20261110-080500-coach-coach-decisions", "coach", "coach", "Coach decisions"),
                                     ("20261110-080510-gatekeeper-the-gatekeeper-reviews", "gatekeeper", "gatekeeper",
                                      "The Gatekeeper reviews the coach's decisions")):
        d = state / "runs" / name
        d.mkdir()
        (d / "meta.json").write_text(json.dumps({"runner": "claude", "model": "opus", "member": member, "what": what,
                                                 "started": "2026-11-10T08:05:00Z", "finished": "2026-11-10T08:05:09Z",
                                                 "ok": True, "cost_usd": 0.4}))
        (d / "run.json").write_text(json.dumps({"task": what, "description": desc, "member": member, "ok": True,
                                                "model": "claude:opus", "usage": {"cost_usd": 0.4, "turns": 1},
                                                "report": {"summary": "decided"}}))
    return work


def open_view(p, view, pred, timeout=10):
    tree = p.request("view.open", {"view": view})["tree"]
    if pred(tree):
        return tree
    return p.wait_notify("view.update", lambda u: u["view"] == view and pred(u["tree"]), timeout)["tree"]


def test_overview_plan_runs_and_a_live_run(tmp_path):
    work = project(tmp_path)
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.overview", lambda t: widget(t, "needs") is not None)
        needs = widget(tree, "needs")["items"]
        assert [n["text"] for n in needs][0] == "Get the API key"
        assert "the coach wrote to you" in needs[1]["text"]
        running = widget(tree, "running")
        assert running["total"] == 1 and running["rows"][0]["cells"][1] == "@dev"
        assert "1 in progress" in str(tree) and "2 pending" in str(tree)
        assert "Count the lines" in str(tree)                   # the goal
        assert "the Gatekeeper objects" in str(tree)             # the coach's state
        assert "1 news waiting for the coach" in str(tree)

        tree = open_view(p, "myteam.plan", lambda t: (widget(t, "tasks") or {}).get("total", 0) > 0)
        rows = widget(tree, "tasks")["rows"]
        assert [r["key"] for r in rows] == ["s:Work", "^count", "^words", "^key", "^setup"]
        count = rows[1]
        assert count["cells"][1] == {"text": "in progress", "style": "accent"}
        assert "1/2 criteria" in count["cells"][5] and "after ^count" in rows[2]["cells"][5]
        p.notify("command.run", {"command": "myteam.plan.hideDone", "view": "myteam.plan"})
        p.wait_notify("view.update", lambda u: widget(u["tree"], "tasks")["total"] == 4)

        # Runs by kind: the task, the coach's decision, the Gatekeeper's review.
        tree = open_view(p, "myteam.runs", lambda t: (widget(t, "runs") or {}).get("total", 0) == 3)
        kinds = sorted(r["cells"][2] for r in widget(tree, "runs")["rows"])
        assert kinds == ["coach", "review", "task"]

        tree = open_view(p, "myteam.run#20261106-225223-dev-count-lines", lambda t: "Looking at the code." in str(t))
        assert "python3 -m pytest -q" in str(widget(tree, "events"))
        stream = work / ".myteam/runs/20261106-225223-dev-count-lines/stream.jsonl"
        with stream.open("a") as f:
            f.write(json.dumps(STREAM[2]) + "\n")
            f.write(json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": "All done."}]}})
                    + "\n")
        p.wait_notify("view.update", lambda u: "All done." in str(u["tree"]), timeout=10)

        markers = p.wait_notify("markers.set")
        assert markers["set"] == "myteam"
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def test_the_coach_desk_and_the_council(tmp_path):
    work = project(tmp_path)
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.coach", lambda t: widget(t, "objection") is not None)
        assert "No criterion for the word count." in str(widget(tree, "objection"))
        assert "Count words" in str(widget(tree, "objection"))      # the held decision
        assert "accepted by the Gatekeeper" in str(widget(tree, "news"))
        assert "Words come after lines." in str(widget(tree, "notes"))
        assert widget(tree, "decisions")["total"] == 2
        tree = open_view(p, "myteam.council", lambda t: "Unproven" in str(t))
        assert widget(tree, "councils")["items"][0]["text"] == "words"
        assert "What is a word?" in str(widget(tree, "views"))
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def test_customer_and_crew(tmp_path):
    work = project(tmp_path)
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.customer", lambda t: "Count the lines, please." in str(t))
        assert "Is a word anything between spaces?" in str(widget(tree, "messages"))
        assert "Coach wrote last" in str(tree)
        assert widget(tree, "reply")["t"] == "field"

        tree = open_view(p, "myteam.team", lambda t: (widget(t, "members") or {}).get("total", 0) == 3)
        rows = {r["key"]: r for r in widget(tree, "members")["rows"]}
        assert rows["coach"]["cells"][1]["text"] == "coach" and rows["gatekeeper"]["cells"][1]["text"] == "Gatekeeper"
        dev = rows["dev"]
        assert dev["cells"][2] == "app" and dev["cells"][6] == "1"        # its unit, one lesson
        assert dev["cells"][3]["style"] == "accent" and "running" in dev["cells"][3]["text"]
        props = dict((k, v) for k, v in dev["sel"][0]["props"])
        assert props["From the coach"] == "Test first." and "use the venv" in props["Lesson 1"]

        tree = open_view(p, "myteam.activity", lambda t: (widget(t, "activity") or {}).get("total", 0) == 1)
        row = widget(tree, "activity")["rows"][0]
        assert row["cells"][1] == "Count lines (^count)" and row["cells"][4] == "1 commit"
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def fake_myteam(tmp_path: Path) -> tuple[str, Path]:
    """The real myteam for json; anything else only records what it was asked (no agent runs)."""
    log = tmp_path / "asked.txt"
    exe = tmp_path / "myteam"
    exe.write_text(f'#!/bin/sh\nif [ "$1" = json ]; then exec "{MYTEAM}" "$@"; fi\n'
                   f'printf "%s\\n" "$*" >> "{log}"\n')
    exe.chmod(0o755)
    return str(exe), log


def test_telling_the_coach_says_it_and_empties_the_field(tmp_path):
    work = project(tmp_path)
    exe, log = fake_myteam(tmp_path)
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": exe})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.customer", lambda t: widget(t, "reply") is not None)
        assert widget(tree, "reply")["value"] == ""
        for how in ("press", "submit"):  # the Send button, and Enter
            text = f"Yes, between spaces ({how})"
            if how == "press":
                p.notify("view.event", {"view": "myteam.customer", "widget": "send", "kind": "press",
                                        "value": {"form": {"reply": text}}})
            else:
                p.notify("view.event", {"view": "myteam.customer", "widget": "reply", "kind": "submit", "value": text})
            p.wait_notify("view.update", lambda u, text=text: widget(u["tree"], "reply")["value"] == text)
            p.wait_notify("view.update", lambda u: widget(u["tree"], "reply")["value"] == "")
            p.wait_notify("job.end")
        asked = log.read_text().splitlines()
        assert asked[0].startswith("say Yes, between spaces (press)")
        assert "Yes, between spaces (submit)" in asked[1]
    finally:
        out = p.close()
    assert "Traceback" not in out, out


def test_json_is_what_the_plugin_reads(tmp_path):
    work = project(tmp_path)

    def j(*what):
        return json.loads(subprocess.run([MYTEAM, "json", *what, "-d", str(work)], capture_output=True, text=True,
                                         check=True).stdout)

    o = j("overview")
    assert o["project"] == "tally" and o["counts"]["in_progress"] == 1 and o["goal"] == "Count the lines"
    assert [r["state"] for r in o["running"]] == ["running"]
    assert o["coach"]["objection"] and o["coach"]["news"] == 1
    assert j("conversation")["waiting_for_you"] is True
    assert j("council")["council"][0]["views"][1]["who"] == "gatekeeper"
    team = j("team")
    assert team["coach"] == "coach" and sorted(m["kind"] for m in team["members"]) == ["coach", "expert", "gatekeeper"]
    assert "# How to decide" in j("digest")["text"]


def test_outside_a_project_the_views_offer_one_and_remember_it(tmp_path):
    work = project(tmp_path)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    env = {"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM, "XDG_STATE_HOME": str(tmp_path / "state")}
    p = Peer(elsewhere, env=env)
    try:
        p.request("initialize", {"protocol": 1})
        tree = p.request("view.open", {"view": "myteam.overview"})["tree"]
        items = widget(tree, "projects")["items"]
        assert [i["key"] for i in items][0] == str(work)
        assert "No myteam project in " in str(tree)
        p.notify("view.event", {"view": "myteam.overview", "widget": "projects", "kind": "activate", "value": str(work)})
        p.wait_notify("view.update", lambda u: u["view"] == "myteam.overview" and "Needs you" in str(u["tree"]),
                      timeout=10)
    finally:
        log = p.close()
    assert "Traceback" not in log, log
    p = Peer(elsewhere, env=env)
    try:
        p.request("initialize", {"protocol": 1})
        open_view(p, "myteam.overview", lambda t: "Needs you" in str(t))
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def test_one_part_that_fails_leaves_the_others_shown(tmp_path):
    work = project(tmp_path)
    # The team is gone: what needs it fails, the plan and the runs still show.
    (work / ".myteam" / "config.yaml").write_text(f"team: {tmp_path / 'no-such-team'}\nmethodology: coach\n")
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.plan", lambda t: (widget(t, "tasks") or {}).get("total", 0) > 0)
        assert widget(tree, "tasks")["total"] == 5
        assert "myteam: " in str(tree)  # what failed, said
        open_view(p, "myteam.customer", lambda t: "Count the lines, please." in str(t))
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def test_skills_and_decline(tmp_path):
    work = project(tmp_path)
    skill = work / ".myteam" / "skills" / "read-a-pdf" / "SKILL.md"
    skill.parent.mkdir(parents=True)
    skill.write_text("---\nname: \"Read a PDF\"\ndescription: \"When a task needs a manual\"\n"
                     "triggers: [\"pdf\"]\nmembers: []\nsource: \"the coach, 2026-11-10\"\n---\n\nUse gs txtwrite.\n")
    p = Peer(work, env={"MYRCP_HOST_PLUGINS": "myteam", "MYTEAM": MYTEAM})
    try:
        p.request("initialize", {"protocol": 1})
        tree = open_view(p, "myteam.skills", lambda t: (widget(t, "skills") or {}).get("total", 0) == 1)
        row = widget(tree, "skills")["rows"][0]
        assert row["key"] == "read-a-pdf" and row["cells"][1] == "everyone" and row["cells"][2] == "pdf"
        assert row["cells"][3] == "the coach, 2026-11-10"

        tree = open_view(p, "myteam.plan", lambda t: (widget(t, "tasks") or {}).get("total", 0) >= 4)
        key = next(r["key"] for r in widget(tree, "tasks")["rows"] if "Get the API key" in str(r["cells"][0]))
        p.notify("command.run", {"command": "myteam.plan.decline", "view": "myteam.plan", "widget": "tasks", "key": key})
        p.wait_notify("view.show", lambda u: u["view"] == "myteam.done")
        tree = open_view(p, "myteam.done", lambda t: "Decline: " in str(t))
        assert "held for a decision" in str(tree) and "Get the API key" in str(tree)
    finally:
        log = p.close()
    assert "Traceback" not in log, log
