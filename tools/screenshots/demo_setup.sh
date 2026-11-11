#!/bin/sh
# The made-up org of the screenshots (run inside sandbox.sh): ~/src/pantry, a food bank's
# stock app in three units, a day into its first milestone. No agent runs: the crew's
# runs, the coach's state and the conversation are written here as myteam would leave them.
set -e
find "$HOME" -mindepth 1 -delete
git config --global user.name "Me"; git config --global user.email "me@example.org"
git config --global init.defaultBranch main
at() { date -u -d "$1" +%Y-%m-%dT%H:%M:%SZ; }       # a time relative to now
id() { date -u -d "$1" +%Y%m%d-%H%M%S; }

O="$HOME/src/pantry"; mkdir -p "$O"; cd "$O"; git init -q
myorg init -t crew --name pantry >/dev/null
myorg add api --purpose "the HTTP API: stock, donations, pickups" >/dev/null
myorg add web --purpose "the volunteers' web app" >/dev/null
myorg add cli --purpose "the warehouse's command-line client" >/dev/null
myorg link api web --mode collaboration --until 2 --provider api >/dev/null
myorg link api cli --mode x-as-a-service --provider api >/dev/null
python3 - <<'PY'   # unit types, the org's rules and its brief
import re
p = "ORG.md"; s = open(p).read()
for unit, kind in (("api", "platform"), ("web", "stream-aligned"), ("cli", "stream-aligned")):
    s = s.replace(f"  {unit}:\n    team: \n", f"  {unit}:\n    team: \n    type: {kind}\n")
s = s.replace("units:\n", "rules:\n  - \"Every change to interfaces/api bumps its version.\"\n"
              "  - \"Dates are ISO 8601 between units.\"\n\nunits:\n", 1)
s = s.replace("## System\n", "## System\n\nA food bank's stock: the API keeps it, volunteers use the web app on\n"
              "their phones, the warehouse uses the command-line client.\n")
open(p, "w").write(s)
PY

S=.myteam; M=$S/team/members; mkdir -p "$M"
member() { printf -- '---\nname: %s\nrole: %s\nunit: %s\nsummary: "%s"\n---\n%s\n' "$2" "$3" "$4" "$5" "$6" > "$M/$1.md"; }
member aino Aino "API engineer" api "builds the stock and donations API" "You build the HTTP API."
member olli Olli "web engineer" web "builds the volunteers' web app" "You build the web app."
member veera Veera "CLI engineer" cli "builds the warehouse client" "You build the command-line client."

cat > PLAN.md <<EOF
---
project: pantry
team: crew
goal: Volunteers see the stock and log donations from a phone; the warehouse counts it from a terminal
---

# pantry

## Milestone 1: stock you can see

- [x] The stock API: items, quantities, best-before dates @aino ^stock-api #p1 ~4h
  - (x) GET /items lists every item with its quantity
  - (x) Tests run in CI
- [~] Log a donation @aino ^donations #p1 ~3h
  - depends: ^stock-api
  - (x) POST /donations adds to the stock
  - ( ) A donation past its best-before date is refused
- [?] The stock page @olli ^stock-page #p1 ~4h
  - depends: ^stock-api
  - (x) It lists the items, shortest best-before first
  - (x) It works on a phone (360 px)
- [ ] The donation form @olli ^donation-form #p2 ~3h
  - depends: ^donations, ^stock-page
- [~] pantry count: the stock in a terminal @veera ^count #p2 ~2h
  - depends: ^stock-api
  - ( ) Reads interfaces/api
  - ( ) --low lists what runs out this week
- [ ] A test server for the volunteers @operator ^server #p1
  - notes: a small VPS or the food bank's own machine; the API needs a public address

## Later

- [ ] Pickups: book a time to collect a parcel @aino ^pickups #p3
- [ ] Sign-in for volunteers @olli ^sign-in #p3
EOF

mkdir -p $S/customer $S/coach $S/council $S/memory
cat > $S/customer/conversation.md <<EOF
# The customer and the coach

### Customer · $(at '-26 hours')

We run a small food bank. Volunteers should see from their phones what is in stock and
log donations as they come in; the warehouse counts stock from a laptop in the back.

### Coach (Coach) · $(at '-25 hours')

Three units: an API (Aino), a web app for the volunteers (Olli) and a command-line
client for the warehouse (Veera). The first milestone is stock you can see; donations
come right after. Should a donation past its best-before date be refused or flagged?

### Customer · $(at '-24 hours')

Refused. We cannot hand it out.

### Coach (Coach) · $(at '-3 hours')

The stock page is in review. One thing only you can give: a server the volunteers can
reach. It is ^server in the plan.
EOF

cat > $S/coach/state.json <<EOF
{"news": ["^stock-api accepted by the Gatekeeper: GET /items lists 42 items; CI is green",
          "^stock-page reported done by @olli: the stock page, shortest best-before first"],
 "seen_tasks": [], "customer_seen": 3,
 "objection": "^count has no criterion for an empty stock: what should the warehouse see then?",
 "held": "<task to=\"veera\" id=\"count-json\">\npantry count --json, for the spreadsheet\n</task>\n",
 "idle": 0, "urgent": 0, "quiet": 0, "steps": 9, "decisions": 6, "milestones": 0,
 "cost": 3.84, "last_decision": "$(at '-40 minutes')"}
EOF
cat > $S/memory/coach.md <<'EOF'
The customer refuses donations past their best-before date: no flagging.
Volunteers use phones; test every page at 360 px.
The warehouse laptop has no browser it trusts: the CLI is its only way in.
EOF
cat > $S/memory/aino.md <<'EOF'
## From the coach

Keep interfaces/api current: Olli and Veera build on it.

## Lessons

- ^stock-api: dates are ISO 8601 in the API, local time only in the web app
EOF
cat > $S/memory/olli.md <<'EOF'
## From the coach

Test every page at 360 px.

## Lessons

- ^stock-page: the API's best-before dates are strings; sort after parsing them
EOF
cat > "$S/council/$(id '-5 hours')-best-before.md" <<EOF
# Council: best-before

$(at '-5 hours')

## The question

A donation past its best-before date: where is it refused, and what does the volunteer see?

## @aino

The API refuses it (422) with the date, so every client gets the same rule.

## @olli

The form shows the API's reason next to the date field; no second check in the browser.

## @gatekeeper

Agreed, if ^donations gets a criterion that says so; today nothing tests the refusal.
EOF

# The runs: decisions, reviews and work, and one in progress (its pid is set by the
# screenshot scripts, which keep a stand-in process alive).
run() {  # run NAME MEMBER WHAT DESCRIPTION START-AGO SECONDS COST [MODEL]
  d=$S/runs/$(id "$5")-$1; mkdir -p "$d"; s=$(at "$5"); f=$(date -u -d "$5 + $6 seconds" +%Y-%m-%dT%H:%M:%SZ)
  printf '{"runner": "claude", "model": "%s", "member": "%s", "what": "%s", "started": "%s", "finished": "%s", "ok": true, "cost_usd": %s}\n' \
    "${8:-sonnet}" "$2" "$3" "$s" "$f" "$7" > "$d/meta.json"
  printf '{"task": "%s", "description": "%s", "member": "%s", "ok": true, "model": "claude:%s", "usage": {"cost_usd": %s, "turns": 12}, "report": {"summary": "%s"}}\n' \
    "$3" "$4" "$2" "${8:-sonnet}" "$7" "$4" > "$d/run.json"
}
run coach-coach-decisions coach coach "Coach decisions" '-25 hours' 70 0.52 opus
run gatekeeper-the-gatekeeper-reviews gatekeeper gatekeeper "The Gatekeeper reviews the coach's decisions" '-25 hours + 2 minutes' 40 0.31 opus
run aino-the-stock-api aino ^stock-api "The stock API: items, quantities, best-before dates" '-23 hours' 1980 1.12
run gatekeeper-check-stock-api gatekeeper ^stock-api "The Gatekeeper checks ^stock-api" '-22 hours' 300 0.28 opus
run coach-coach-decisions coach coach "Coach decisions" '-5 hours' 95 0.61 opus
run olli-the-stock-page olli ^stock-page "The stock page" '-4 hours' 2400 1.37
run coach-coach-decisions coach coach "Coach decisions" '-40 minutes' 80 0.48 opus
run gatekeeper-the-gatekeeper-reviews gatekeeper gatekeeper "The Gatekeeper reviews the coach's decisions" '-38 minutes' 45 0.33 opus
live() {  # live NAME MEMBER WHAT START-AGO
  d=$S/runs/$(id "$4")-$1; mkdir -p "$d"
  printf '{"runner": "claude", "model": "sonnet", "member": "%s", "what": "%s", "started": "%s", "pid": 0}\n' "$2" "$3" "$(at "$4")" > "$d/meta.json"
}
live aino-log-a-donation aino ^donations '-12 minutes'
cat > "$S/runs/$(id '-12 minutes')-aino-log-a-donation/stream.jsonl" <<'EOF'
{"type": "assistant", "message": {"content": [{"type": "text", "text": "Reading interfaces/api and the stock model first."}]}}
{"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t1", "name": "Read", "input": {"file_path": "api/src/stock.py"}}]}}
{"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t1", "content": "class Item: ..."}]}}
{"type": "assistant", "message": {"content": [{"type": "text", "text": "POST /donations adds to an item's quantity. Now the refusal: a best-before date before today is a 422 with the date."}]}}
{"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t2", "name": "Edit", "input": {"file_path": "api/src/donations.py"}}]}}
{"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t2", "content": "ok"}]}}
{"type": "assistant", "message": {"content": [{"type": "tool_use", "id": "t3", "name": "Bash", "input": {"command": "python -m pytest -q tests/test_donations.py"}}]}}
{"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": "t3", "content": "5 passed, 1 failed"}]}}
{"type": "assistant", "message": {"content": [{"type": "text", "text": "The expired case returns 400, not 422. Fixing the status code."}]}}
EOF
live veera-pantry-count veera ^count '-7 minutes'

cat > CHANGELOG.md <<EOF
# Changelog

## $(at '-21 hours') · The stock API (^stock-api)

- Team: Crew; member: Aino (@aino), claude:sonnet
- Result: [x] done (accepted by the Gatekeeper)
- Commit: \`3f1c2ab\` on main: The stock API (6 files)

## $(at '-3 hours') · The stock page (^stock-page)

- Team: Crew; member: Olli (@olli), claude:sonnet
- Result: [?] review
- Commit: \`9be04d1\` on main: The stock page (4 files)
EOF

mkdir -p .myorg/log
tick() {  # tick N AGO COST WAITING-FOR NOTES-JSON
  printf '{"time": "%s", "tick": %s, "milestone": 1, "waiting": %s, "waiting_for": "%s", "rate_limited": false, "reached_milestone": false, "cost": %s, "notes": %s, "problems": []}\n' \
    "$(at "$2")" "$1" "$([ -n "$4" ] && echo true || echo false)" "$4" "$3" "$5" >> .myorg/log/ticks.jsonl
}
tick 1 '-25 hours' 0.83 "the coach asked you a question" '["the coach decided: 4 tasks, 3 units", "the Gatekeeper accepted the decisions"]'
tick 2 '-21 hours' 1.40 "" '["^stock-api by @aino: [x] done", "the Gatekeeper accepted ^stock-api"]'
tick 3 '-3 hours' 1.98 "" '["^stock-page by @olli: [?] review", "the coach decided: ^server for the operator"]'
tick 4 '-38 minutes' 0.81 "" '["the Gatekeeper objects: ^count has no criterion for an empty stock"]'
printf '{"milestone": 1, "ticks": 4}\n' > .myorg/state.json
myorg sync >/dev/null
git add -A >/dev/null 2>&1 && git commit -qm "pantry" >/dev/null 2>&1 || true
myteam status -d "$O" >/dev/null
echo "demo org ready"
