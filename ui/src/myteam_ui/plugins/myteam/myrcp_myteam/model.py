"""What the views show, from `myteam json` (see myteam's include/myteam/inspect.hpp).

The crew (methodology: coach): the coach decides, the Gatekeeper checks, the experts work
in their units; you are the customer, and talk to the coach.

One Model per project, shared by the views of a host. A watcher thread looks at
the files myteam writes (the plan, the changelog, .myteam/...) once a second and
reloads when they change; a run's view also follows that run's event stream.
Everything the views read is replaced on the main thread, whole, so a render
never sees half an update.
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


def find_myteam(rcp: Context | None) -> str:
    """The myteam executable: the preference, $MYTEAM, $PATH, else the build/ of the myteam checkout this comes from."""
    pref = str(rcp.pref("myteam.command") or "") if rcp else ""
    for cand in (pref, os.environ.get("MYTEAM", "")):
        if cand:
            return cand
    if found := shutil.which("myteam"):
        return found
    for rel in ("../../../../../build/myteam", "../../../../../build-release/myteam"):
        p = (PLUGIN_DIR / rel).resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return "myteam"


STATE_FILE = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state") / "myrcp" / "myteam.json"


def is_project(d: Path) -> bool:
    return (d / ".myteam").is_dir()


def saved_project() -> Path | None:
    """The project chosen last (Choose the project), when it is still one."""
    try:
        p = Path(json.loads(STATE_FILE.read_text()).get("project", ""))
    except (OSError, ValueError, AttributeError):
        return None
    return p if str(p) and is_project(p) else None


_chosen: Path | None = None  # chosen in this session: it wins over the cwd's


def save_project(p: Path) -> None:
    global _chosen
    _chosen = p
    try:
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(json.dumps({"project": str(p)}) + "\n")
    except OSError:
        pass


def find_project(rcp: Context | None) -> Path:
    """The project: the preference; else the one chosen in this session; else the nearest
    directory with .myteam/ from the cwd up; else the one chosen last; else the cwd (which
    is not one: the views offer a choice)."""
    pref = str(rcp.pref("myteam.project") or "") if rcp else ""
    if not pref and _chosen is not None and is_project(_chosen):
        return _chosen
    start = Path(pref).expanduser() if pref else Path.cwd()
    for d in (start, *start.parents):
        if is_project(d):
            return d
    if not pref and (s := saved_project()):
        return s
    return start


def candidates(limit: int = 40) -> list[Path]:
    """myteam projects nearby: below the cwd, beside it, beside its parent, in ~/src."""
    cwd = Path.cwd()
    places = [cwd, cwd.parent, cwd.parent.parent, Path.home() / "src", Path.home()]
    found: list[Path] = []
    for place in places:
        try:
            kids = sorted(place.iterdir())
        except OSError:
            continue
        for k in kids:
            try:
                if k.is_dir() and is_project(k) and k not in found:
                    found.append(k)
            except OSError:
                continue
    if (s := saved_project()) and s not in found:
        found.insert(0, s)
    return found[:limit]


class MyteamError(RuntimeError):
    pass


def call(exe: str, project: Path, *args: str, timeout: float = 60) -> Any:
    """myteam json ...: the parsed answer; MyteamError with myteam's message."""
    try:
        out = subprocess.run(
            [exe, "json", *args, "-d", str(project)],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError as e:
        raise MyteamError(f"myteam is not installed ({exe}); set the preference myteam.command") from e
    except subprocess.TimeoutExpired as e:
        raise MyteamError(f"myteam json {' '.join(args)} timed out") from e
    text = out.stdout.strip()
    data = None
    if text:
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            pass
    if out.returncode != 0 or data is None:
        if isinstance(data, dict) and data.get("error"):
            raise MyteamError(str(data["error"]))
        err = out.stderr.strip().splitlines()
        raise MyteamError(err[-1].removeprefix("myteam: ") if err else f"myteam json {args[0]} failed")
    return data


def command(exe: str, project: Path, *args: str, timeout: float = 3600) -> tuple[bool, str]:
    """A myteam command for its effect (approve, done, ask, ...): ok, and what it said."""
    try:
        out = subprocess.run(
            [exe, *args, "-d", str(project)], capture_output=True, text=True, timeout=timeout, check=False
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    said = (out.stdout + ("\n" + out.stderr if out.stderr.strip() else "")).strip()
    return out.returncode == 0, said


@dataclass
class Snapshot:
    """Everything but the event streams, as one load got it."""

    overview: dict[str, Any] = field(default_factory=dict)
    plan: dict[str, Any] = field(default_factory=dict)
    team: dict[str, Any] = field(default_factory=dict)
    runs: list[dict[str, Any]] = field(default_factory=list)
    conversation: dict[str, Any] = field(default_factory=dict)
    coach: dict[str, Any] = field(default_factory=dict)
    council: list[dict[str, Any]] = field(default_factory=list)
    feedback: list[dict[str, Any]] = field(default_factory=list)
    activity: list[dict[str, Any]] = field(default_factory=list)
    skills: list[dict[str, Any]] = field(default_factory=list)
    error: str = ""


def parse_changelog(path: Path, limit: int = 300) -> list[dict[str, Any]]:
    """The changelog's entries, newest first: `## <time> · <title>` and its `- Key: value` lines."""
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return []
    entries: list[dict[str, Any]] = []
    cur: dict[str, Any] | None = None
    for n, line in enumerate(lines, 1):
        if line.startswith("## "):
            head = line[3:]
            when, _, title = head.partition(" · ")
            cur = {"time": when.strip(), "title": title.strip() or head, "line": n, "items": {}, "body": []}
            entries.append(cur)
        elif cur is not None:
            if line.startswith("- ") and ": " in line:
                k, _, v = line[2:].partition(": ")
                cur["items"].setdefault(k, []).append(v)
            elif line.strip():
                cur["body"].append(line)
    entries.reverse()
    return entries[:limit]


class Model:
    """The project as the views see it; subscribe to hear about changes."""

    _models: dict[Path, Model] = {}
    _lock = threading.Lock()

    @classmethod
    def get(cls, rcp: Context) -> Model:
        project = find_project(rcp)
        with cls._lock:
            m = cls._models.get(project)
            if m is None:
                m = cls._models[project] = Model(rcp, project)
            return m

    def __init__(self, rcp: Context, project: Path) -> None:
        self.rcp = rcp
        self.project = project
        self.exe = find_myteam(rcp)
        self.data = Snapshot(error="Loading…")
        self.loaded = False
        self._subs: list[Callable[[], Any]] = []
        self._streams: dict[str, Callable[[], Any]] = {}  # run name -> notify
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread = threading.Thread(target=self._watch, name="myteam-watch", daemon=True)
        self._thread.start()

    # -- subscribers ----------------------------------------------------------------
    def subscribe(self, fn: Callable[[], Any]) -> None:
        self._subs.append(fn)

    def unsubscribe(self, fn: Callable[[], Any]) -> None:
        if fn in self._subs:
            self._subs.remove(fn)

    def follow_run(self, name: str, fn: Callable[[], Any]) -> None:
        """fn (on the main thread) when the run's stream or record changes."""
        self._streams[name] = fn
        self._wake.set()

    def unfollow_run(self, name: str) -> None:
        self._streams.pop(name, None)

    def reload(self) -> None:
        """Load again now (after an action of the user's)."""
        self._force = True
        self._wake.set()

    # -- reading --------------------------------------------------------------------
    def json(self, *args: str) -> Any:
        return call(self.exe, self.project, *args)

    def load(self) -> Snapshot:
        s = Snapshot()
        if not is_project(self.project):
            s.error = f"No myteam project in {self.project}"
            return s
        # Each part on its own: one that fails (a file that does not parse, say)
        # leaves the others shown, and says what failed.
        errors: list[str] = []

        def part(what: str, *args: str, key: str | None = None, default: Any = None) -> Any:
            try:
                data = self.json(what, *args)
                return data[key] if key else data
            except (MyteamError, KeyError, TypeError) as e:
                errors.append(f"{what}: {e}")
                return default

        s.overview = part("overview", default={})
        s.plan = part("plan", default={})
        s.team = part("team", default={})
        s.runs = part("runs", "--max", "500", key="runs", default=[])
        s.conversation = part("conversation", default={})
        s.coach = part("coach", default={})
        s.council = part("council", key="council", default=[])
        s.feedback = part("feedback", key="feedback", default=[])
        s.skills = part("skills", key="skills", default=[])
        if errors:
            # All of them failing the same way (myteam missing, say): one message.
            s.error = errors[0] if len({e.split(": ", 1)[-1] for e in errors}) == 1 else "; ".join(errors)
        s.activity = parse_changelog(self.project / "CHANGELOG.md")
        return s

    # -- watching -------------------------------------------------------------------
    def _signature(self) -> tuple:
        state = self.project / ".myteam"
        paths = [
            self.project / "PLAN.md",
            self.project / "CHANGELOG.md",
            state / "config.yaml",
            state / "coach" / "state.json",
            state / "customer" / "conversation.md",
            state / "feedback.md",
            state / "memory",
            state / "council",
            state / "runs",
            state / "skills",
        ]
        try:
            paths += sorted((state / "memory").glob("*.md"))
            paths += sorted((state / "council").glob("*.md"))
            paths += sorted((state / "skills").glob("*/SKILL.md"))
            runs = sorted((state / "runs").iterdir(), reverse=True)[:6] if (state / "runs").is_dir() else []
        except OSError:
            runs = []
        for r in runs:
            paths += [r / "meta.json", r / "run.json", r / "result.md"]
        return tuple(_stamp(p) for p in paths)

    def _stream_signature(self) -> dict[str, tuple]:
        out = {}
        for name in list(self._streams):
            r = self.project / ".myteam" / "runs" / name
            out[name] = (_stamp(r / "stream.jsonl"), _stamp(r / "meta.json"), _stamp(r / "run.json"))
        return out

    _force = False

    def _watch(self) -> None:
        last: tuple | None = None
        last_streams: dict[str, tuple] = {}
        while not self._stop.is_set():
            sig = self._signature()
            if sig != last or self._force:
                self._force = False
                last = sig
                snap = self.load()
                self.rcp.call_soon(lambda snap=snap: self._apply(snap))
            streams = self._stream_signature()
            for name, s in streams.items():
                if last_streams.get(name) != s and (fn := self._streams.get(name)):
                    self.rcp.call_soon(fn)
            last_streams = streams
            self._wake.wait(POLL_SECONDS)
            self._wake.clear()

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
