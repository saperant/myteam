---
id: crew
name: Crew
summary: "A flat crew: a coach who decides, a skeptical Gatekeeper who checks every decision and every piece of work, and experts who do the work in their units"
topology: stream-aligned
coach: coach
gatekeeper: gatekeeper
repairer: coach
models: {default: claude:sonnet}
---

A flat crew (methodology: coach). The coach is the customer's only contact; it decides what
the crew works on, who does it and what counts as done. The Gatekeeper is the coach's
skeptical right arm. The experts do the work, each in its unit, and learn from it.

A project gives the crew its own experts: add members (members/<handle>.md, with `unit:` for
the unit an expert works in) in a project-local copy of this team (.myteam/teams/crew/), or
in the project's overlay (.myteam/team/members/).
