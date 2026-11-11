"""Screenshots of myteam and myorg in the terminal workbench (run inside sandbox.sh):
argv[1] the out dir. With --explore, steps from argv[2:] ("key:<escaped>", "wait:<text>",
"dump") instead of the screenshots, printing the screen."""
import fcntl, json, os, pty, struct, subprocess, sys, termios, threading, time
from pathlib import Path
import pyte
sys.path.insert(0, os.path.dirname(__file__))
from term_svg import render

ORG = Path.home() / "src/pantry"
OUT = Path(sys.argv[1]); COLS, ROWS = 180, 44

# The runs in progress belong to a live process, as a real run's runner would be.
standin = subprocess.Popen(["sleep", "600"])
for meta in ORG.glob(".myteam/runs/*/meta.json"):
    m = json.loads(meta.read_text())
    if "pid" in m:
        m["pid"] = standin.pid; meta.write_text(json.dumps(m))
for state in (Path.home() / ".local/state").glob("**/workbench.state"):
    state.unlink()

env = dict(os.environ, TERM="xterm-256color", MYRCP_PLUGIN_PATH="")
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
proc = subprocess.Popen(["myteam-ui"], stdin=slave, stdout=slave, stderr=slave, env=env, cwd=ORG,
                        start_new_session=True)
os.close(slave)
buf = bytearray(); lock = threading.Lock()
def reader():
    while True:
        try: d = os.read(master, 65536)
        except OSError: return
        if not d: return
        with lock: buf.extend(d)
threading.Thread(target=reader, daemon=True).start()
def screen():
    s = pyte.Screen(COLS, ROWS)
    with lock: pyte.ByteStream(s).feed(bytes(buf))
    return s
def text(): return "\n".join(screen().display)
def wait(needle, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if needle in text(): return
        time.sleep(0.1)
    raise TimeoutError(needle + "\n" + text())
def key(k, pause=0.4): os.write(master, k.encode()); time.sleep(pause)
def palette(command, wait_for):
    key("\x10"); wait("Commands"); key(command, 0.6); key("\r", 1.0); wait(wait_for)
def shot(name, title):
    time.sleep(1.0); (OUT / name).write_text(render(screen(), title))
    (OUT / (name[:-4] + ".txt")).write_text(text())
DOWN, UP, TAB = "\x1b[B", "\x1b[A", "\t"
try:
    if "--explore" in sys.argv:
        for step in sys.argv[3:]:
            kind, _, arg = step.partition(":")
            if kind == "key": key(arg.encode().decode("unicode_escape"), 0.8)
            elif kind == "wait": wait(arg)
            elif kind == "sleep": time.sleep(float(arg))
            elif kind == "dump": print(text()); print("-" * COLS)
        sys.exit(0)
    wait("No views open")
    palette("perspective myteam", "Needs you")
    shot("rcp-overview.svg", "myteam — pantry")
    key("\x1b2"); wait("Milestone 1"); shot("rcp-plan.svg", "myteam — pantry: the plan")
    key("\x1b5"); wait("Gatekeeper objects:"); shot("rcp-coach.svg", "myteam — pantry: the coach's desk")
    key("\x1b4"); wait("You and Coach"); shot("rcp-customer.svg", "myteam — pantry: you and the coach")
    key("\x1b3"); wait("running"); key(DOWN); key("\r"); wait("Fixing the status code")
    shot("rcp-run.svg", "myteam — pantry: a run as it happens")
    palette("perspective myorg", "Interactions")
    key("\x1b1"); wait("Experts"); shot("rcp-org.svg", "myorg — pantry")
    key("\x1b2"); wait("Hidden from it"); shot("rcp-topology.svg", "myorg — pantry: the topology")
finally:
    proc.kill(); standin.kill()
