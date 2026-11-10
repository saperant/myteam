---
id: research
name: Research
product_owner: editor
members: [editor, researcher, analyst, reviewer, writer]

# For the catalog an org picks teams from (myorg catalog):
summary: "Research and documentation: questions, sources, evidence, review, and the written result"
topology: enabling
domain: research
delivers: [studies, literature-reviews, documentation]

# Every member uses these unless its own file sets models:
models:
  default: claude:sonnet
  premium: claude:opus     # #p0 tasks
  economy: claude:haiku    # #p2/#p3 tasks

allocation:
  roles:      # plan assignees that stand for a member: @review -> reviewer
    research: researcher
    literature: researcher
    analysis: analyst
    review: reviewer
    writing: writer
    docs: writer
  keywords:   # words in a task that point to a member
    question: editor
    scope: editor
    source: researcher
    literature: researcher
    survey: researcher
    evidence: analyst
    compare: analyst
    trade-off: analyst
    review: reviewer
    check: reviewer
    write: writer
    document: writer
    guide: writer
---

A research and documentation team (methodology: direct). It does not write software: its
result is writing that others can rely on. Each member keeps its own work file under
`study/`, and the writer turns the reviewed work into the final document.

Work files:

- `study/01_questions.md` — the questions and the scope (editor)
- `study/02_sources.md` — sources and related work, each with where it came from (researcher)
- `study/03_analysis.md` — the evidence, compared, and the trade-offs (analyst)
- `study/04_review.md` — what does not hold up, and what was fixed (reviewer)
- `docs/` — the result (writer)
