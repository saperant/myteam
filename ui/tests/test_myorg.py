"""The myorg plugin over the real protocol, on a small org made by the real myorg."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from conftest import Peer

MYORG = os.environ.get("MYORG") or shutil.which("myorg") or str(
    Path(__file__).resolve().parents[2] / "build" / "myorg"
)
pytestmark = pytest.mark.skipif(not os.access(MYORG, os.X_OK), reason="myorg is not built")


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


def myorg(org: Path, *args: str) -> str:
    out = subprocess.run([MYORG, *args, "-d", str(org)], capture_output=True, text=True, check=True)
    return out.stdout


@pytest.fixture(scope="module")
def org(tmp_path_factory) -> Path:
    o = tmp_path_factory.mktemp("org") / "suite"
    o.mkdir()
    for k, v in (("user.name", "Test"), ("user.email", "test@example.invalid")):
        subprocess.run(["git", "init", "-q", str(o)], check=True)
        subprocess.run(["git", "-C", str(o), "config", k, v], check=True)
    myorg(o, "init", "-t", "crew", "--name", "suite")
    myorg(o, "add", "api", "--purpose", "the HTTP API")
    myorg(o, "add", "web", "--purpose", "the web front end")
    myorg(o, "add", "cli", "--purpose", "the command-line client")
    myorg(o, "link", "api", "web", "--mode", "collaboration", "--until", "2", "--provider", "web")
    # An expert in api (web and cli have none yet).
    member = o / ".myteam" / "team" / "members" / "apidev.md"
    member.parent.mkdir(parents=True)
    member.write_text("---\nname: Apidev\nrole: API architect\nunit: api\n---\nAPIs.\n")
    (o / ".myorg" / "log").mkdir(exist_ok=True)
    (o / ".myorg" / "log" / "ticks.jsonl").write_text(json.dumps({
        "time": "2026-11-10T10:00:00Z", "tick": 1, "milestone": 1, "waiting": True,
        "waiting_for": "the crew waits for you: ^key", "rate_limited": False, "reached_milestone": False,
        "cost": 0.42, "notes": ["^schema by @apidev: [?] review", "the coach decided"], "problems": []}) + "\n")
    return o


def open_view(p, view, pred, timeout=15):
    tree = p.request("view.open", {"view": view})["tree"]
    if pred(tree):
        return tree
    return p.wait_notify("view.update", lambda u: u["view"] == view and pred(u["tree"]), timeout)["tree"]


def peer(org: Path, plugins: str = "myorg") -> Peer:
    env = {"MYRCP_HOST_PLUGINS": plugins, "MYORG": MYORG, "XDG_STATE_HOME": str(org.parent / "state")}
    p = Peer(org, env=env)
    p.request("initialize", {"protocol": 1})
    return p


def test_json_is_what_the_plugin_reads(org):
    o = json.loads(myorg(org, "json"))
    assert o["name"] == "suite" and o["milestone"] == 1
    units = {u["handle"]: u for u in o["units"]}
    assert units["api"]["experts"] == ["apidev"] and units["web"]["experts"] == []
    assert "../web" in units["api"]["view"]["readable"] and "../cli" in units["api"]["view"]["hide"]
    x = o["interactions"][0]
    assert x["now"] == "collaboration" and x["until"] == 2
    log = json.loads(myorg(org, "json", "log"))
    assert log["ticks"][0]["cost"] == 0.42


def test_the_org_its_units_and_their_boundaries(org):
    p = peer(org)
    try:
        tree = open_view(p, "myorg.overview", lambda t: (widget(t, "units") or {}).get("total", 0) == 3)
        rows = {r["key"]: r for r in widget(tree, "units")["rows"]}
        assert rows["api"]["cells"][2] == "@apidev"
        assert rows["web"]["cells"][2] == {"text": "nobody", "style": "warning"}
        assert "web" in rows["api"]["cells"][4]          # api sees web (collaboration)
        assert "toward milestone 1" in str(tree)
        assert "the crew waits for you" in str(tree)     # the last tick's wait

        tree = open_view(p, "myorg.topology", lambda t: (widget(t, "topology.interactions") or {}).get("total", 0) == 1)
        x = widget(tree, "topology.interactions")["rows"][0]
        assert x["cells"][1] == {"text": "collaboration", "style": "accent"}
        assert x["cells"][4] == "milestone 2, then x-as-a-service"

        tree = open_view(p, "myorg.units", lambda t: len((widget(t, "units") or {}).get("items", [])) == 4)
        items = widget(tree, "units")["items"]
        assert items[0]["key"] == "org" and [i["key"] for i in items[1:]] == ["api", "web", "cli"]

        tree = open_view(p, "myorg.activity", lambda t: (widget(t, "activity") or {}).get("total", 0) == 1)
        row = widget(tree, "activity")["rows"][0]
        assert row["cells"][1] == "1" and row["cells"][2] == "$0.42"
        assert "the crew waits for you" in str(row["cells"][3])

        markers = p.wait_notify("markers.set", lambda m: any("has no expert" in x["message"] for x in m["markers"]))
        assert markers["set"] == "myorg"
    finally:
        log = p.close()
    assert "Traceback" not in log, log


def test_outside_an_org_the_views_offer_one(org, tmp_path):
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    env = {"MYRCP_HOST_PLUGINS": "myorg", "MYORG": MYORG, "XDG_STATE_HOME": str(tmp_path / "state")}
    p = Peer(elsewhere, env=env)
    try:
        p.request("initialize", {"protocol": 1})
        tree = p.request("view.open", {"view": "myorg.overview"})["tree"]
        assert "No org in " in str(tree)
        p.notify("view.event", {"view": "myorg.overview", "widget": "orgs", "kind": "activate", "value": str(org)})
        p.wait_notify("view.update", lambda u: u["view"] == "myorg.overview" and "toward milestone" in str(u["tree"]),
                      timeout=15)
    finally:
        log = p.close()
    assert "Traceback" not in log, log
