// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// taskml - C++ implementation of the TaskML language (spec v1.1).
// https://taskml.dev
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "taskml/export.hpp"
#include "taskml/json.hpp"

#define TASKML_VERSION_MAJOR 0
#define TASKML_VERSION_MINOR 1
#define TASKML_VERSION_PATCH 0
#define TASKML_VERSION_STRING "0.1.0"
#define TASKML_SPEC_VERSION "1.1"

namespace taskml {

/// Library version, e.g. "0.1.0".
TASKML_API const char* library_version() noexcept;
/// Version of the TaskML specification implemented, e.g. "1.1".
TASKML_API const char* spec_version() noexcept;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class TaskStatus {
    Pending,     ///< [ ]
    InProgress,  ///< [~]
    Completed,   ///< [x]
    Blocked,     ///< [!]
    Cancelled,   ///< [-]
    Review,      ///< [?]
};

enum class CriterionStatus {
    Pending,   ///< ○
    Verified,  ///< ✓
    Failed,    ///< ✗
};

/// Canonical string names: "pending", "in_progress", "completed", ...
TASKML_API const char* to_string(TaskStatus status) noexcept;
/// Canonical string names: "pending", "verified", "failed".
TASKML_API const char* to_string(CriterionStatus status) noexcept;
/// The character inside the status brackets: ' ', '~', 'x', '!', '-', '?'.
TASKML_API char status_marker(TaskStatus status) noexcept;
/// The UTF-8 marker: "○", "✓", "✗".
TASKML_API const char* criterion_marker(CriterionStatus status) noexcept;
/// The ASCII marker: "( )", "(x)", "(!)". Both forms parse in both dialects;
/// the Markdown writer uses this one.
TASKML_API const char* criterion_marker_ascii(CriterionStatus status) noexcept;
TASKML_API std::optional<TaskStatus> task_status_from_string(std::string_view name) noexcept;
TASKML_API std::optional<CriterionStatus> criterion_status_from_string(std::string_view name) noexcept;

/// The view types defined by the spec (section 6.2).
TASKML_API const std::vector<std::string>& standard_views();
/// The directives defined by the spec (sections 3.1 and 7.1).
TASKML_API const std::vector<std::string>& standard_directives();

// ---------------------------------------------------------------------------
// Ordered string map (used for directives and view options)
// ---------------------------------------------------------------------------

/// A small insertion-ordered string-to-string map. Keys are unique; setting
/// an existing key replaces its value but keeps its original position.
class TASKML_API StringMap {
public:
    using value_type = std::pair<std::string, std::string>;
    using const_iterator = std::vector<value_type>::const_iterator;

    StringMap() = default;
    StringMap(std::initializer_list<value_type> items);

    const std::string* get(std::string_view key) const noexcept;
    std::string value_or(std::string_view key, std::string fallback) const;
    bool contains(std::string_view key) const noexcept { return get(key) != nullptr; }
    void set(std::string key, std::string value);
    bool erase(std::string_view key);
    void clear() noexcept { items_.clear(); }

    std::size_t size() const noexcept { return items_.size(); }
    bool empty() const noexcept { return items_.empty(); }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    const std::vector<value_type>& items() const noexcept { return items_; }

    friend bool operator==(const StringMap& a, const StringMap& b) { return a.items_ == b.items_; }
    friend bool operator!=(const StringMap& a, const StringMap& b) { return !(a == b); }

private:
    std::vector<value_type> items_;
};

// ---------------------------------------------------------------------------
// Document model
// ---------------------------------------------------------------------------

/// A verification criterion (spec section 8).
struct TASKML_API Criterion {
    std::string description;
    CriterionStatus status = CriterionStatus::Pending;
    std::optional<std::string> evidence;
    int line = 0;  ///< Source line (1-based), 0 when not parsed from text.

    friend TASKML_API bool operator==(const Criterion& a, const Criterion& b);
    friend bool operator!=(const Criterion& a, const Criterion& b) { return !(a == b); }
};

/// A task (spec sections 4, 5, 8, 9).
struct TASKML_API Task {
    TaskStatus status = TaskStatus::Pending;
    std::string description;
    std::optional<std::string> id;        ///< ^id
    std::optional<int> priority;          ///< #p0 .. #p3
    std::optional<std::string> estimate;  ///< ~4h / ~2d  (stored without '~')
    std::optional<std::string> assignee;  ///< @name      (stored without '@')
    std::vector<std::string> tags;        ///< #tag       (stored without '#')
    std::optional<std::string> due;       ///< !YYYY-MM-DD (stored without '!')
    std::vector<std::string> depends_on;  ///< depends: ^a, ^b (stored without '^')
    std::vector<std::string> blocked_by;  ///< blocked-by: ^a  (stored without '^')
    std::vector<Task> subtasks;
    std::vector<Criterion> criteria;
    std::vector<std::string> notes;       ///< "- note" lines
    int line = 0;  ///< Source line (1-based), 0 when not parsed from text.

    friend TASKML_API bool operator==(const Task& a, const Task& b);
    friend bool operator!=(const Task& a, const Task& b) { return !(a == b); }
};

/// "== Title ==" section header with the tasks that follow it.
struct TASKML_API Section {
    std::string title;
    int level = 2;  ///< Number of '=' characters.
    std::vector<Task> tasks;
    int line = 0;

    friend TASKML_API bool operator==(const Section& a, const Section& b);
    friend bool operator!=(const Section& a, const Section& b) { return !(a == b); }
};

/// "< path/to/file.taskml" include line. Includes are recorded, not resolved.
struct TASKML_API Include {
    std::string path;
    int line = 0;

    friend bool operator==(const Include& a, const Include& b) { return a.path == b.path; }
    friend bool operator!=(const Include& a, const Include& b) { return !(a == b); }
};

/// "---view:type key=value ..." rendering hint (spec section 6).
struct TASKML_API ViewConfig {
    std::string type;
    StringMap options;

    friend bool operator==(const ViewConfig& a, const ViewConfig& b) {
        return a.type == b.type && a.options == b.options;
    }
    friend bool operator!=(const ViewConfig& a, const ViewConfig& b) { return !(a == b); }
};

/// Agent metadata from @agent / @agent-id directives and the ---context block.
struct TASKML_API AgentContext {
    std::optional<std::string> agent_id;
    std::optional<std::string> model;
    std::optional<std::string> session_id;
    std::optional<std::string> started_at;
    /// The full ---context block. If the block is not valid JSON this is
    /// {"raw": "<block text>"}.
    std::optional<Json> metadata;

    friend TASKML_API bool operator==(const AgentContext& a, const AgentContext& b);
    friend bool operator!=(const AgentContext& a, const AgentContext& b) { return !(a == b); }
};

/// ---handoff block (spec section 7.3).
struct TASKML_API Handoff {
    std::string from;
    std::string to;
    std::optional<std::string> reason;
    /// The `context:` value. If it is not valid JSON this is {"raw": "<text>"}.
    std::optional<Json> context;

    friend TASKML_API bool operator==(const Handoff& a, const Handoff& b);
    friend bool operator!=(const Handoff& a, const Handoff& b) { return !(a == b); }
};

/// A "// comment" line.
struct TASKML_API Comment {
    int line = 0;
    std::string text;

    friend bool operator==(const Comment& a, const Comment& b) {
        return a.line == b.line && a.text == b.text;
    }
    friend bool operator!=(const Comment& a, const Comment& b) { return !(a == b); }
};

/// A parsed TaskML document.
struct TASKML_API Document {
    /// Value of @version, or "1.1" when absent.
    std::string version = TASKML_SPEC_VERSION;
    StringMap directives;
    /// Tasks that appear before the first section header.
    std::vector<Task> tasks;
    std::vector<Section> sections;
    std::vector<Include> includes;
    std::optional<ViewConfig> view;
    std::optional<AgentContext> agent_context;
    std::optional<Handoff> handoff;
    std::vector<Comment> comments;

    /// Find a task (anywhere in the tree, including sections) by its ^id.
    const Task* find_task(std::string_view id) const;
    Task* find_task(std::string_view id);

    /// All tasks in document order (depth-first, pre-order), including
    /// subtasks and section tasks.
    std::vector<const Task*> all_tasks() const;

    friend TASKML_API bool operator==(const Document& a, const Document& b);
    friend bool operator!=(const Document& a, const Document& b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

/// Error codes, compatible with the reference implementation.
///   E1xx lexical, E2xx syntax, E3xx semantic, E4xx interchange.
enum class ErrorCode {
    UnexpectedCharacter = 100,
    InconsistentIndentation = 101,
    InvalidEscape = 103,
    InvalidDateFormat = 104,
    InvalidEstimateFormat = 105,
    InvalidPriority = 106,

    ExpectedStatus = 200,
    ExpectedDescription = 201,
    ExpectedDirectiveValue = 203,
    UnexpectedToken = 204,
    InvalidNesting = 205,
    ExpectedFenceEnd = 206,
    InvalidSection = 207,
    InvalidCriterion = 208,
    DuplicateTaskId = 209,

    UnknownDirective = 300,
    InvalidViewType = 301,
    UnknownStatus = 302,
    UnknownDependency = 303,
    CircularDependency = 304,
    TokenOrderViolation = 305,
    UnknownBlocker = 306,
    InvalidVersion = 307,
    MultipleAssignees = 308,

    InvalidJson = 400,
    MissingRequiredField = 402,
    InvalidFieldType = 403,
};

/// "E101" etc.
TASKML_API std::string to_string(ErrorCode code);

enum class Severity { Error, Warning, Info };
TASKML_API const char* to_string(Severity severity) noexcept;

struct TASKML_API Diagnostic {
    ErrorCode code = ErrorCode::UnexpectedToken;
    Severity severity = Severity::Error;
    int line = 0;    ///< 1-based; 0 when not applicable.
    int column = 0;  ///< 1-based byte column; 0 when not applicable.
    std::string message;
    std::string suggestion;

    /// Human readable form, e.g.
    ///   error[E101]: Inconsistent indentation: expected 4 spaces, got 2 (3:3)
    /// When `source` is given, the offending line is quoted with a caret.
    std::string format(std::string_view source = {}) const;
};

/// Thrown by parse_or_throw() and from_json().
class TASKML_API Error : public std::runtime_error {
public:
    Error(const std::string& message, std::vector<Diagnostic> diagnostics);
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

/// Surface syntax of a plan file.
///
/// TaskML is the syntax of the TaskML 1.1 specification. Markdown is the same
/// model written as a CommonMark/GFM document, so that ordinary Markdown
/// tooling (highlighting, previews, linters) works on plans:
///
///     ---
///     project: Website Redesign
///     sprint: Sprint 1
///     ---
///
///     ## Backend
///
///     - [~] Build API #p1 ~8h @bob ^api
///       - depends: ^design
///       - A note about the task
///       - [x] Setup database
///       - (x) Returns JSON
///
/// - YAML-style front matter (`key: value`) gives the directives; `@name value`
///   lines are accepted too.
/// - `#`..`######` headings are sections (level = number of `#`).
/// - List items (`-`, `*`, `+`, `1.`, `1)`) carry tasks (`[ ]`), criteria
///   (`( )` `(x)` `(!)`, or `○ ✓ ✗`), `depends:`, `blocked-by:`, `evidence:`; other list items under
///   a task are notes. Nesting follows the list indentation.
/// - Everything else (paragraphs, code blocks, tables, quotes, HTML comments,
///   rules) is prose and is ignored without diagnostics.
/// - `---context`, `---handoff`, `---view` blocks and `<` includes are TaskML
///   only.
enum class Dialect { TaskML, Markdown };

/// Markdown for ".md" / ".markdown" paths, TaskML otherwise.
TASKML_API Dialect dialect_for_path(std::string_view path) noexcept;

struct ParseOptions {
    /// Surface syntax; see Dialect.
    Dialect dialect = Dialect::TaskML;
    /// Strict mode reports token-order violations (E305) as warnings.
    bool strict = false;
    /// When false, directives that are neither standard nor "x-" prefixed
    /// are reported as warnings (E300).
    bool allow_unknown_directives = true;
    /// Maximum task nesting depth (top-level tasks are depth 1). Deeper
    /// tasks are reported as errors (E205) and dropped.
    int max_depth = 64;
};

struct ParseResult {
    Document document;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;

    bool ok() const noexcept { return errors.empty(); }
};

/// Parse TaskML text. Never throws on malformed input: problems are
/// reported in ParseResult::errors / warnings and parsing recovers.
TASKML_API ParseResult parse(std::string_view text, const ParseOptions& options = {});

/// Parse TaskML text; throws taskml::Error if there are any errors.
TASKML_API Document parse_or_throw(std::string_view text, const ParseOptions& options = {});

/// Semantic validation of a document: duplicate IDs (E209), unknown
/// dependencies (E303) and blockers (E306), circular dependencies (E304),
/// invalid view type (E301), invalid priority (E106).
TASKML_API std::vector<Diagnostic> validate(const Document& document);

/// Parse in strict mode (unknown directives reported) and run semantic
/// validation. Returns all errors and warnings; empty means valid.
TASKML_API std::vector<Diagnostic> validate(std::string_view text);
/// validate(text) for the given dialect. Unknown directives are not reported
/// for Markdown, whose front matter is open-ended.
TASKML_API std::vector<Diagnostic> validate(std::string_view text, Dialect dialect);

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

struct SerializeOptions {
    /// Surface syntax. Markdown output always uses two-space list nesting and
    /// writes directives as front matter; view, context and handoff blocks and
    /// includes are not written.
    Dialect dialect = Dialect::TaskML;
    /// Spaces per nesting level (ignored when use_tabs is true).
    int indent_size = 4;
    bool use_tabs = false;
    /// Emit comments, grouped at the top of the document (their original
    /// positions are not preserved).
    bool include_comments = false;
};

/// Write a document back as TaskML text in canonical form. Tokens are
/// emitted in the recommended order:
///   description #priority ~estimate @assignee #tags !due ^id
/// For any document d: parse(to_taskml(d)).document == d (ignoring line
/// numbers, comments and whitespace normalization of descriptions).
TASKML_API std::string to_taskml(const Document& document, const SerializeOptions& options = {});

/// to_taskml() with options.dialect = Dialect::Markdown.
TASKML_API std::string to_markdown(const Document& document, SerializeOptions options = {});

/// Convert to the JSON interchange format used by the reference
/// implementation (camelCase keys; empty fields omitted). `indent` < 0
/// produces compact output.
TASKML_API std::string to_json(const Document& document, int indent = 2);
TASKML_API Json to_json_value(const Document& document);

/// Parse the JSON interchange format. Throws taskml::Error (E400/E402/E403).
TASKML_API Document from_json(std::string_view json);
TASKML_API Document from_json_value(const Json& json);

}  // namespace taskml
