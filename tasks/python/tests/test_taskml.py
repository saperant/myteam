# SPDX-License-Identifier: MIT
import copy
import json
import pickle
from pathlib import Path

import pytest

import taskml
from taskml import CriterionStatus, Document, ErrorCode, Task, TaskStatus

DATA = Path(__file__).resolve().parents[2] / "tests" / "data"
SPEC_EXAMPLE = (DATA / "spec_example.taskml").read_text(encoding="utf-8")


def test_version():
    assert taskml.__version__ == "0.1.0"
    assert taskml.SPEC_VERSION == "1.1"


def test_parse_spec_example():
    result = taskml.parse(SPEC_EXAMPLE)
    assert result.ok and bool(result)
    assert result.errors == [] and result.warnings == []
    doc = result.document
    assert doc.directives["project"] == "TaskML Parser"
    assert doc.directives.get("missing", "dflt") == "dflt"
    assert list(doc.directives)[:2] == ["project", "sprint"]
    assert doc.version == "1.1"
    assert len(doc.tasks) == 4
    lexer = doc.find_task("lexer")
    assert lexer.status == TaskStatus.IN_PROGRESS
    assert lexer.status.label == "in_progress"
    assert lexer.priority == 0 and lexer.estimate == "4h" and lexer.assignee == "bob"
    assert list(lexer.depends_on) == ["setup"]
    assert list(lexer.subtasks[1].notes) == ["Handle multi-line strings", "Support Unicode"]
    parser = doc.find_task("parser")
    assert [c.status for c in parser.criteria] == [CriterionStatus.PENDING] * 2
    assert doc.view.type == "kanban"
    assert doc.view.options["columns"] == "pending,in_progress,blocked,completed"
    assert doc.comments[0].text == "Core Parser Implementation"
    assert doc.find_task("nope") is None
    assert len(doc.all_tasks()) == 14


def test_mutation_is_in_place():
    doc = taskml.parse_or_raise("[ ] a #x\n    [ ] b")
    doc.tasks[0].tags.append("y")
    doc.tasks[0].subtasks[0].status = TaskStatus.COMPLETED
    doc.tasks[0].description = "A"
    doc.directives["project"] = "P"
    doc.tasks.append(Task("new", TaskStatus.BLOCKED, priority=1, tags=["t"]))
    t = doc.find_task  # noqa: F841  (just check attribute access works)
    assert doc.to_taskml() == (
        "@project P\n\n[ ] A #x #y\n    [x] b\n[!] new #p1 #t\n"
    )
    # Reference from find_task / all_tasks also edits in place.
    doc2 = taskml.parse_or_raise("[ ] a ^a")
    doc2.find_task("a").assignee = "zoe"
    doc2.all_tasks()[0].due = "2026-01-01"
    assert doc2.tasks[0].assignee == "zoe" and doc2.tasks[0].due == "2026-01-01"


def test_list_assignment_accepts_python_lists():
    t = Task("x")
    t.tags = ["a", "b"]
    t.notes = ["n"]
    t.subtasks = [Task("child")]
    assert list(t.tags) == ["a", "b"]
    assert t.subtasks[0].description == "child"


def test_errors_and_warnings():
    r = taskml.parse("[ ] a\n    [ ] b\n  [ ] c\n[y] bad")
    assert not r.ok
    assert r.errors[0].code == ErrorCode.INCONSISTENT_INDENTATION
    assert r.errors[0].code.code == "E101"
    assert (r.errors[0].line, r.errors[0].column) == (3, 3)
    assert r.warnings[0].code == ErrorCode.UNKNOWN_STATUS
    assert "E101" in str(r.errors[0])
    with pytest.raises(taskml.TaskMLError) as exc:
        taskml.parse_or_raise("[ ] a\n    [ ] b\n  [ ] c")
    assert isinstance(exc.value, ValueError)
    assert exc.value.diagnostics[0].line == 3


def test_strict_mode():
    r = taskml.parse("[ ] Task ~4h #p0", strict=True)
    assert r.ok
    assert r.warnings[0].message == "Token order: priority should appear before estimate"
    assert taskml.parse("[ ] Task ~4h #p0").warnings == []


def test_validate():
    diags = taskml.validate("[ ] a ^a\n    depends: ^b\n[ ] b ^b\n    depends: ^a\n")
    assert [d.code for d in diags] == [ErrorCode.CIRCULAR_DEPENDENCY]
    assert taskml.validate(SPEC_EXAMPLE) == []
    doc = Document()
    doc.tasks = [Task("x", id="dup"), Task("y", id="dup", blocked_by=["ghost"])]
    codes = {d.code for d in doc.validate()}
    assert codes == {ErrorCode.DUPLICATE_TASK_ID, ErrorCode.UNKNOWN_BLOCKER}


def test_json_interchange():
    doc = taskml.parse_or_raise(SPEC_EXAMPLE)
    data = json.loads(doc.to_json())
    assert data["tasks"][1]["status"] == "in_progress"
    assert data["tasks"][1]["dependsOn"] == ["setup"]
    assert doc.to_dict() == data
    assert Document.from_dict(data) == doc
    assert taskml.from_json(taskml.to_json(doc, indent=None)) == doc
    with pytest.raises(taskml.TaskMLError) as exc:
        taskml.from_json('{"tasks":[{"status":"nope","description":"x"}]}')
    assert exc.value.diagnostics[0].code == ErrorCode.INVALID_FIELD_TYPE
    with pytest.raises(taskml.TaskMLError):
        taskml.from_json("{")


def test_context_and_handoff_blocks():
    src = (
        "@agent claude-3\n---context\n"
        '{"session_id": "abc", "nested": {"n": [1, 2.5, null, true]}}\n---\n'
        '---handoff\nfrom: a1\nto: a2\nreason: "why"\ncontext: {"k": "v"}\n---\n'
    )
    doc = taskml.parse_or_raise(src)
    ctx = doc.agent_context
    assert ctx.model == "claude-3" and ctx.session_id == "abc"
    assert ctx.metadata["nested"] == {"n": [1, 2.5, None, True]}
    h = doc.handoff
    assert (h.from_, h.to, h.reason, h.context) == ("a1", "a2", "why", {"k": "v"})
    assert taskml.parse_or_raise(doc.to_taskml()) == doc


def test_build_document_from_scratch_round_trips():
    doc = Document()
    doc.directives = {"project": "Demo", "x-team": "core"}
    doc.tasks = [
        Task(
            "Use #hash and @at literally",
            TaskStatus.REVIEW,
            id="t1",
            priority=2,
            estimate="3d",
            assignee="ann",
            tags=["a"],
            due="2026-05-01",
            notes=["note"],
            criteria=[taskml.Criterion("works", CriterionStatus.VERIFIED, evidence="log")],
            subtasks=[Task("child", depends_on=["t1"])],
        )
    ]
    doc.sections = [taskml.Section("Later", 3, [Task("s")])]
    doc.view = taskml.ViewConfig("table", {"columns": "status,task"})
    doc.handoff = taskml.Handoff(from_="x", to="y", context={"a": 1})
    text = doc.to_taskml()
    assert "\\#hash" in text and "\\@at" in text
    back = taskml.parse_or_raise(text)
    assert back == doc
    assert back.to_taskml() == text
    assert taskml.to_taskml(doc, use_tabs=True).splitlines()[4].startswith("\t")


def test_pickle_and_copy():
    doc = taskml.parse_or_raise(SPEC_EXAMPLE)
    assert pickle.loads(pickle.dumps(doc)) == doc
    c = copy.deepcopy(doc)
    c.tasks[0].description = "changed"
    assert doc.tasks[0].description != "changed"


def test_load_and_dump(tmp_path):
    p = tmp_path / "x.taskml"
    doc = taskml.parse_or_raise("[ ] a #p1")
    taskml.dump(doc, p)
    assert taskml.load(p) == doc


def test_enum_helpers_and_lists():
    assert TaskStatus.from_label("blocked") == TaskStatus.BLOCKED
    assert TaskStatus.BLOCKED.marker == "!"
    assert CriterionStatus.FAILED.marker == "✗"
    with pytest.raises(ValueError):
        TaskStatus.from_label("nope")
    assert "kanban" in taskml.standard_views()
    assert "project" in taskml.standard_directives()


def test_repr():
    doc = taskml.parse_or_raise("@project P\n[ ] a ^x #p1")
    assert repr(doc) == "Document(project='P', tasks=1, sections=0)"
    assert repr(doc.tasks[0]) == "Task('a', pending, id='x', priority=1)"
