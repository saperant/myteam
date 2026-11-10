---
id: software
name: Software
product_owner: lead
repairer: developer
members: [lead, developer, tester, writer]

# For the catalog an org picks teams from (myorg catalog):
summary: "A small software team: requirements, implementation, testing, documentation"
topology: stream-aligned
domain: software
delivers: [applications, services, documentation]

# Every member uses these unless its own file sets models:
models:
  default: claude:sonnet
  premium: claude:opus     # #p0 tasks
  economy: claude:haiku    # #p2/#p3 tasks

allocation:
  roles:      # plan assignees that stand for a member: @qa -> tester
    qa: tester
    docs: writer
    spec: lead
  keywords:   # words in a task that point to a member
    implement: developer
    fix: developer
    bug: developer
    test: tester
    verify: tester
    docs: writer
    readme: writer
    requirement: lead
    scope: lead
---

A small software team (methodology: direct). Work moves between members through handoffs:
the lead writes what is needed, the developer builds it, the tester checks it, the writer
documents it. Every member stays within its own role and hands off the parts that belong to
others.
