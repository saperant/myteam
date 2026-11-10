# SPDX-License-Identifier: MIT
"""TaskML (https://taskml.dev) for Python.

Parse, validate, modify and serialize TaskML documents. This package wraps
the taskml C++ library.

    >>> import taskml
    >>> doc = taskml.parse_or_raise("[ ] Write docs #p1 @alice ~2h")
    >>> doc.tasks[0].assignee
    'alice'
    >>> doc.tasks[0].status = taskml.TaskStatus.COMPLETED
    >>> print(doc.to_taskml(), end="")
    [x] Write docs #p1 ~2h @alice
"""

from ._taskml import (
    SPEC_VERSION,
    AgentContext,
    Comment,
    Criterion,
    CriterionStatus,
    Diagnostic,
    Document,
    ErrorCode,
    Handoff,
    Include,
    ParseResult,
    Section,
    Severity,
    StringMap,
    Task,
    TaskMLError,
    TaskStatus,
    ViewConfig,
    __version__,
    from_json,
    parse,
    parse_or_raise,
    standard_directives,
    standard_views,
    to_json,
    to_markdown,
    to_taskml,
    validate,
)


def dialect_for_path(path):
    """'markdown' for .md / .markdown paths, 'taskml' otherwise."""
    return "markdown" if str(path).lower().endswith((".md", ".markdown")) else "taskml"


def load(path, *, encoding="utf-8", **options):
    """Read and parse a plan file; raises TaskMLError on errors.

    The dialect follows the file name (see dialect_for_path) unless given.
    """
    options.setdefault("dialect", dialect_for_path(path))
    with open(path, encoding=encoding) as f:
        return parse_or_raise(f.read(), **options)


def dump(document, path, *, encoding="utf-8", dialect=None, **options):
    """Write a document to a plan file in canonical form, in the dialect of its name."""
    dialect = dialect or dialect_for_path(path)
    text = to_markdown(document, **options) if dialect == "markdown" else to_taskml(document, **options)
    with open(path, "w", encoding=encoding, newline="\n") as f:
        f.write(text)


__all__ = [
    "SPEC_VERSION",
    "dialect_for_path",
    "to_markdown",
    "AgentContext",
    "Comment",
    "Criterion",
    "CriterionStatus",
    "Diagnostic",
    "Document",
    "ErrorCode",
    "Handoff",
    "Include",
    "ParseResult",
    "Section",
    "Severity",
    "StringMap",
    "Task",
    "TaskMLError",
    "TaskStatus",
    "ViewConfig",
    "__version__",
    "dump",
    "from_json",
    "load",
    "parse",
    "parse_or_raise",
    "standard_directives",
    "standard_views",
    "to_json",
    "to_taskml",
    "validate",
]
