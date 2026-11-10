# taskml

A C++17 implementation of [TaskML](https://taskml.dev) (Task Markup Language,
spec v1.1). It ships as a shared library for C++ applications and has Python
bindings.

```taskml
@project Website Redesign
@sprint Sprint 1

[x] Design homepage #p0 ~2d @alice ^design
[~] Build API #p1 ~8h @bob ^api
    depends: ^design
    [x] Setup database
    [ ] Create endpoints
    ✓ Returns JSON
    ○ Handles errors
[ ] Write tests #p2 #qa

---view:kanban
```

Features:

- **Parse** all of TaskML 1.1: directives, the six task statuses, inline tokens
  (`#p0` `~4h` `@name` `#tag` `!YYYY-MM-DD` `^id`), nesting, notes,
  `depends:` and `blocked-by:`, criteria (`✓ ✗ ○`, bare or bracketed),
  `evidence:`, views with options, `---context` and `---handoff` blocks,
  comments, `== sections ==`, `< includes` and backslash escapes.
- **Report problems without throwing.** Errors and warnings carry the
  reference implementation's codes (E101, E209, …), line and column, and a
  suggestion. The parser recovers from bad input and keeps going.
- **Validate** the document: duplicate IDs, unknown dependencies and blockers,
  circular dependencies, token order in strict mode, unknown directives and
  invalid view types.
- **Serialize** back to canonical TaskML. `parse(to_taskml(doc)) == doc`.
- **Convert to and from JSON** using the same format as the reference
  TypeScript implementation (`@taskml/core`).
- Has no dependencies beyond the C++ standard library. The Python bindings
  use pybind11.

## Markdown plans

The same model can be written as an ordinary Markdown document, so editors,
previews and linters work on plans. Files named `*.md` / `*.markdown` use this
dialect in the CLI and in Python's `load`/`dump`; in C++ set
`ParseOptions::dialect = taskml::Dialect::Markdown` (and `to_markdown(doc)` to
write).

```markdown
---
project: Website Redesign
sprint: Sprint 1
---

# Website Redesign

Prose, tables and code blocks are ignored.

## Backend

- [x] Design homepage #p0 ~2d @alice ^design
- [~] Build API #p1 ~8h @bob ^api
  - depends: ^design
  - A note about the task
  - [x] Setup database
  - (x) Returns JSON
  - ( ) Handles errors
```

- Front matter `key: value` lines are the directives (nested values are
  skipped); `@name value` lines work too.
- `#` headings are sections, at the heading's level.
- List items (`-`, `*`, `+`, `1.`) hold tasks, criteria, `depends:`,
  `blocked-by:` and `evidence:`. Criteria are written in ASCII, easy to
  type: `( )` pending, `(x)` verified, `(!)` failed (`○ ✓ ✗` are read too,
  in both dialects). Other list items nested under a task are
  notes; lists that are not under a task are prose.
- Paragraphs, code blocks, tables, quotes, rules and HTML comments are prose:
  they end an open list and produce no diagnostics.
- `---context` / `---handoff` / `---view` blocks and `<` includes exist only in
  TaskML.

```sh
taskml check PLAN.md            # Markdown by its name
taskml md --taskml old.plan     # TaskML -> Markdown
```

## Building (C++)

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build            # run the test suite
cmake --install build --prefix /usr/local
```

| CMake option          | Default | Meaning                                  |
|-----------------------|---------|------------------------------------------|
| `BUILD_SHARED_LIBS`   | `ON`    | Build `libtaskml.so` (`OFF` gives a static lib) |
| `TASKML_BUILD_TESTS`  | `ON`*   | C++ test suite                           |
| `TASKML_BUILD_CLI`    | `ON`*   | `taskml` command line tool               |
| `TASKML_BUILD_PYTHON` | `OFF`   | Python extension module (needs pybind11) |
| `TASKML_INSTALL`      | `ON`*   | Install rules, CMake package, pkg-config |

\* defaults to `ON` only when taskml is the top-level project.

The shared library builds with hidden symbol visibility, and only the
`TASKML_API` symbols are exported. Its SONAME is `libtaskml.so.0.1`, and the
ABI is kept stable within a minor version.

### Using it from a C++ project

After installing, use the CMake package:

```cmake
find_package(taskml 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE taskml::taskml)
```

Or vendor the source tree with `add_subdirectory(taskml)` or `FetchContent`
and link `taskml::taskml`. You can also use pkg-config:
`pkg-config --cflags --libs taskml`.

```cpp
#include <taskml/taskml.hpp>

taskml::ParseResult r = taskml::parse(text);
for (const auto& e : r.errors) std::cerr << e.format(text) << '\n';

taskml::Document& doc = r.document;
for (const taskml::Task* t : doc.all_tasks())
    std::cout << taskml::to_string(t->status) << ' ' << t->description << '\n';

if (taskml::Task* t = doc.find_task("api")) t->status = taskml::TaskStatus::Completed;

std::string canonical = taskml::to_taskml(doc);
std::string json      = taskml::to_json(doc);
taskml::Document back = taskml::from_json(json);           // throws taskml::Error
std::vector<taskml::Diagnostic> problems = taskml::validate(text);
```

All public declarations are in [`include/taskml/taskml.hpp`](include/taskml/taskml.hpp),
and [`examples/basic.cpp`](examples/basic.cpp) is a complete program.

## Python

```sh
pip install .            # builds the extension via scikit-build-core + pybind11
```

```python
import taskml

result = taskml.parse(text)               # never raises on bad input
if not result.ok:
    for e in result.errors:
        print(e.format(text))

doc = taskml.parse_or_raise(text)          # raises taskml.TaskMLError (a ValueError)
doc.directives["project"]                  # ordered str -> str mapping
task = doc.find_task("api")
task.status = taskml.TaskStatus.COMPLETED  # edits happen in place
task.tags.append("shipped")
doc.tasks.append(taskml.Task("New task", priority=1, assignee="bob"))

print(doc.to_taskml())
data = doc.to_dict()                       # JSON interchange format as Python data
doc2 = taskml.Document.from_dict(data)
taskml.validate(text)                      # list[Diagnostic]
doc = taskml.load("plan.taskml"); taskml.dump(doc, "plan.taskml")
```

Documents can be compared with `==`, and they support `pickle` and
`copy.deepcopy`. Lists such as `doc.tasks`, `task.subtasks` and `task.tags`
are views onto the C++ document, so changing them changes the document.
You can assign ordinary Python lists to them. `metadata` and `context` in
the `---context` and `---handoff` blocks are returned as plain Python
dicts and lists, so assign a new value when you want to change them.

To work on the bindings without installing them:

```sh
cmake -S . -B build -DTASKML_BUILD_PYTHON=ON -Dpybind11_DIR=$(python -m pybind11 --cmakedir)
cmake --build build
PYTHONPATH=python python -m pytest python/tests
```

## Gantt charts (Python)

`taskml.gantt` turns a TaskML file into an interactive HTML Gantt chart. The
output is a single file with no external assets. It has hover and keyboard
tooltips, highlights a task's dependencies when you hover it, supports light
and dark mode, and includes a table view.

```sh
taskml-gantt plan.taskml -o plan.html --start 2026-01-05     # or: python -m taskml.gantt ...
```

```python
from taskml import gantt
sched = gantt.schedule(doc, gantt.ScheduleOptions(start=date(2026, 1, 5)))
for row in sched.task_rows:
    print(row.title, row.start_date, row.end_date, row.late_days)
html = gantt.render_html(sched)
```

TaskML has no start dates, so the chart is a computed plan:

- **Durations.** `~Nh` estimates are converted using 8 hours per day
  (`--hours-per-day`). `~Nd` is N working days and `~Nw` is five working days
  per week. Weekends are skipped unless you pass `--include-weekends`.
- **Tasks without an estimate.** These share whatever is left of their
  parent's estimate. Otherwise they are assumed to take one day
  (`--default-days`) and are drawn hollow.
- **Start time.** A task starts once everything it depends on (`depends:` or
  `blocked-by:`) has finished. Dependencies declared on a parent task also
  apply to its subtasks.
- **Who works when.** Each assignee does one task at a time, in document
  order. Subtasks without an assignee take the parent's, and unassigned
  subtasks of the same top-level task run one after another. Pass
  `--no-leveling` to turn this off.
- **Parent tasks** are drawn as bars spanning their subtasks.
- **Due dates** appear as diamonds. A task the plan finishes after its due
  date is flagged "late", unless the task is completed or cancelled.
- **Project start.** Taken from `--start`, then the `start=` option of a
  `---view:timeline` fence, then `@created`, then today.

## Command line tool

```sh
taskml check plan.taskml      # strict + semantic validation; exit code 1 on errors
taskml json plan.taskml       # TaskML -> JSON
taskml fmt plan.taskml        # canonical TaskML
taskml from-json plan.json    # JSON -> TaskML
```

## Spec notes

Sources: the [TaskML 1.1 spec](https://github.com/yuvalsuede/taskml/blob/main/packages/spec/SPEC.md)
and the reference TypeScript implementation. Where the two leave something
undefined, or where the reference behaves in a way that is plainly
accidental, this library does the following:

- **Inline tokens have to start a word**, meaning they come at the start of
  the text or after whitespace. `bob@example.com`, `C#` and `issue#42` stay
  as description text. Tokens also need to end at a word boundary, so
  `#p0x` and `~4hours` are text as well.
- **`//` starts a comment only at the beginning of a line.** URLs in
  descriptions are safe.
- **Indentation.** Use 2 or 4 spaces; a tab counts as 4 columns. Siblings
  must share the same indentation, and a mismatch raises error E101. The
  parser still recovers and attaches the line to the nearest parent.
- **Lenient extensions.** These are accepted, and strict mode warns where
  noted:
  - `~Nw` week estimates. The taskml.dev homepage uses them; strict mode
    warns.
  - `depends-on:` as an alias for `depends:`. The upstream README uses it.
  - Notes nested under other notes, and indented continuation lines that
    wrap a note. Nested notes are flattened into the task's `notes` list.
  - `✔` and `✘` as alternatives to `✓` and `✗`, and `[X]` for completed.
- **Escapes**: `\#`, `\@`, `\~`, `\!`, `\^`, `\[`, `\\`, `\✓`, `\✗` and
  `\○` produce the literal character. The serializer adds escapes where
  needed so that round-trips are exact.
- **Description whitespace is normalized**: runs of spaces collapse and
  the ends are trimmed.
- **Empty sections are kept.** The reference implementation drops them.
- **JSON output adds `sections` and `includes`.** The reference `toJSON`
  leaves them out. All other keys match the reference output exactly.
- **`to_taskml` drops comments unless asked.** Pass
  `include_comments = true` to keep them; they are then grouped at the top
  of the output because their positions are not kept. Child items come out
  in this order: `depends:`, `blocked-by:`, notes, subtasks, criteria.
- **Includes are recorded but not resolved.**
- **Rendering is not included.** The HTML, kanban and other views of the
  reference implementation are not part of this library.

## License

MIT, as myteam. See [LICENSE](../LICENSE).
