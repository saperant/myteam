"""What the views show, from `myorg json` (see myteam's include/myorg/org.hpp).

One Model per org, shared by the views. A watcher thread looks at the files myorg and
the crew write (ORG.md, .myorg/state.json, the tick log, the coach's state, each unit's
config) once a second and reloads when they change, so a `myorg run` anywhere (a
Console here, a shell, tmux) shows up as it happens.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import threading
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from myrcp import Context

PLUGIN_DIR = Path(__file__).resolve().parents[1]
POLL_SECONDS = 1.0
STATE_FILE = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state") / "myrcp" / "myorg.json"


def _sibling(name: str) -> str | None:
    for rel in (f"../../../../../build/{name}", f"../../../../../build-release/{name}"):
        p = (PLUGIN_DIR / rel).resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return None


def find_myorg(rcp: Context | None) -> str:
    """The myorg executable: the preference, $MYORG, $PATH, else the build/ of the myteam checkout this comes from."""
    pref = str(rcp.pref("myorg.command") or "") if rcp else ""
    for cand in (pref, os.environ.get("MYORG", "")):
        if cand:
            return cand
    return shutil.which("myorg") or _sibling("myorg") or "myorg"


def find_myteam(myorg: str) -> str:
    """The myteam that goes with it (for `myteam say`): next to myorg, $MYTEAM, $PATH."""
    beside = Path(myorg).resolve().parent / "myteam"
    if beside.is_file() and os.access(beside, os.X_OK):
        return str(beside)
    return os.environ.get("MYTEAM") or shutil.which("myteam") or _sibling("myteam") or "myteam"


def is_org(d: Path) -> bool:
    return (d / "ORG.md").is_file() and (d / ".myorg").is_dir()


def saved_org() -> Path | None:
    try:
        p = Path(json.loads(STATE_FILE.read_text()).get("org", ""))
    except (OSError, ValueError, AttributeError):
        return None
    return p if str(p) and is_org(p) else None


_chosen: Path | None = None


def save_org(p: Path) -> None:
    global _chosen
    _chosen = p
    try:
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(json.dumps({"org": str(p)}) + "\n")
    except OSError:
        pass


def find_org(rcp: Context | None) -> Path:
    """The preference; else the one chosen in this session; else the nearest org from the
    cwd up (a unit's directory is inside its org); else the one chosen last; else the cwd."""
    pref = str(rcp.pref("myorg.org") or "") if rcp else ""
    if not pref and _chosen is not None and is_org(_chosen):
        return _chosen
    start = Path(pref).expanduser() if pref else Path.cwd()
    for d in (start, *start.parents):
        if is_org(d):
            return d
    if not pref and (s := saved_org()):
        return s
    return start


def candidates(limit: int = 40) -> list[Path]:
    """Orgs nearby: below the cwd, beside it, beside its parent, in ~/src."""
    cwd = Path.cwd()
    found: list[Path] = []
    for place in (cwd, cwd.parent, cwd.parent.parent, Path.home() / "src", Path.home()):
        try:
            kids = sorted(place.iterdir())
        except OSError:
            continue
        for k in [place, *kids]:
            try:
                if k.is_dir() and is_org(k) and k not in found:
                    found.append(k)
            except OSError:
                continue
    if (s := saved_org()) and s not in found:
        found.insert(0, s)
    return found[:limit]


class MyorgError(RuntimeError):
    pass


def call(exe: str, org: Path, *args: str, timeout: float = 60) -> Any:
    """myorg json ...: the parsed answer; MyorgError with myorg's message."""
    try:
        out = subprocess.run([exe, "json", *args, "-d", str(org)], capture_output=True, text=True,
                             timeout=timeout, check=False)
    except FileNotFoundError as e:
        raise MyorgError(f"myorg is not installed ({exe}); set the preference myorg.command") from e
    except subprocess.TimeoutExpired as e:
        raise MyorgError(f"myorg json {' '.join(args)} timed out") from e
    try:
        data = json.loads(out.stdout) if out.stdout.strip() else None
    except json.JSONDecodeError:
        data = None
    if out.returncode != 0 or data is None:
        err = out.stderr.strip().splitlines()
        raise MyorgError(err[-1].removeprefix("myorg: ") if err else f"myorg json {args[0]} failed")
    if isinstance(data, dict) and data.get("error"):
        raise MyorgError(str(data["error"]))
    return data


def command(exe: str, org: Path, *args: str, timeout: float = 3600) -> tuple[bool, str]:
    """A myorg command for its effect (sync, myteam build, ...): ok, and what it said."""
    try:
        out = subprocess.run([exe, *args, "-d", str(org)], capture_output=True, text=True, timeout=timeout, check=False)
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    return out.returncode == 0, (out.stdout + ("\n" + out.stderr if out.stderr.strip() else "")).strip()


@dataclass
class Snapshot:
    overview: dict[str, Any] = field(default_factory=dict)
    ticks: list[dict[str, Any]] = field(default_factory=list)
    error: str = ""

    @property
    def units(self) -> list[dict[str, Any]]:
        return self.overview.get("units", [])

    def unit(self, handle: str) -> dict[str, Any] | None:
        return next((u for u in self.units if u.get("handle") == handle), None)


class Model:
    """The org as the views see it; subscribe to hear about changes."""

    _models: dict[Path, Model] = {}
    _lock = threading.Lock()

    @classmethod
    def get(cls, rcp: Context) -> Model:
        org = find_org(rcp)
        with cls._lock:
            m = cls._models.get(org)
            if m is None:
                m = cls._models[org] = Model(rcp, org)
            return m

    def __init__(self, rcp: Context, org: Path) -> None:
        self.rcp = rcp
        self.org = org
        self.exe = find_myorg(rcp)
        self.myteam = find_myteam(self.exe)
        self.data = Snapshot(error="Loading…")
        self.loaded = False
        self._subs: list[Callable[[], Any]] = []
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._force = False
        self._thread = threading.Thread(target=self._watch, name="myorg-watch", daemon=True)
        self._thread.start()

    def subscribe(self, fn: Callable[[], Any]) -> None:
        self._subs.append(fn)

    def unsubscribe(self, fn: Callable[[], Any]) -> None:
        if fn in self._subs:
            self._subs.remove(fn)

    def reload(self) -> None:
        self._force = True
        self._wake.set()

    def load(self) -> Snapshot:
        s = Snapshot()
        if not is_org(self.org):
            s.error = f"No myorg org in {self.org}"
            return s
        try:
            s.overview = call(self.exe, self.org, "overview")
        except MyorgError as e:
            s.error = str(e)
        try:
            s.ticks = call(self.exe, self.org, "log", "-n", "300").get("ticks", [])
        except MyorgError as e:
            s.error = s.error or str(e)
        return s

    def _signature(self) -> tuple:
        o = self.org
        paths = [o / "ORG.md", o / "PLAN.md", o / ".myorg" / "state.json", o / ".myorg" / "config.yaml",
                 o / ".myorg" / "log" / "ticks.jsonl", o / ".myorg" / "stop", o / ".myteam" / "config.yaml",
                 o / ".myteam" / "coach" / "state.json", o / ".myteam" / "customer" / "conversation.md"]
        for u in self.data.units:
            d = o / u.get("dir", "")
            paths += [d / ".myteam" / "config.yaml", d / ".myteam" / "project.md"]
        return tuple(_stamp(p) for p in paths)

    def _watch(self) -> None:
        last: tuple | None = None
        while not self._stop.is_set():
            sig = self._signature()
            if sig != last or self._force:
                self._force = False
                snap = self.load()
                last = self._signature_with(snap)
                self.rcp.call_soon(lambda snap=snap: self._apply(snap))
            self._wake.wait(POLL_SECONDS)
            self._wake.clear()

    def _signature_with(self, snap: Snapshot) -> tuple:
        # The units may have changed with this load: watch theirs from now on.
        old, self.data = self.data, snap
        try:
            return self._signature()
        finally:
            self.data = old

    def _apply(self, snap: Snapshot) -> None:
        self.data = snap
        self.loaded = True
        for fn in list(self._subs):
            fn()


def _stamp(p: Path) -> tuple:
    try:
        st = p.stat()
        return (str(p), st.st_mtime_ns, st.st_size)
    except OSError:
        return (str(p), 0, -1)
