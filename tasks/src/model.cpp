// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
#include <string>

#include "taskml/taskml.hpp"

namespace taskml {

const char* library_version() noexcept { return TASKML_VERSION_STRING; }
const char* spec_version() noexcept { return TASKML_SPEC_VERSION; }

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

const char* to_string(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Pending: return "pending";
        case TaskStatus::InProgress: return "in_progress";
        case TaskStatus::Completed: return "completed";
        case TaskStatus::Blocked: return "blocked";
        case TaskStatus::Cancelled: return "cancelled";
        case TaskStatus::Review: return "review";
    }
    return "pending";
}

const char* to_string(CriterionStatus status) noexcept {
    switch (status) {
        case CriterionStatus::Pending: return "pending";
        case CriterionStatus::Verified: return "verified";
        case CriterionStatus::Failed: return "failed";
    }
    return "pending";
}

char status_marker(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Pending: return ' ';
        case TaskStatus::InProgress: return '~';
        case TaskStatus::Completed: return 'x';
        case TaskStatus::Blocked: return '!';
        case TaskStatus::Cancelled: return '-';
        case TaskStatus::Review: return '?';
    }
    return ' ';
}

const char* criterion_marker(CriterionStatus status) noexcept {
    switch (status) {
        case CriterionStatus::Pending: return "○";   // ○
        case CriterionStatus::Verified: return "✓";  // ✓
        case CriterionStatus::Failed: return "✗";    // ✗
    }
    return "○";
}

const char* criterion_marker_ascii(CriterionStatus status) noexcept {
    switch (status) {
        case CriterionStatus::Pending: return "( )";
        case CriterionStatus::Verified: return "(x)";
        case CriterionStatus::Failed: return "(!)";
    }
    return "( )";
}

std::optional<TaskStatus> task_status_from_string(std::string_view name) noexcept {
    if (name == "pending") return TaskStatus::Pending;
    if (name == "in_progress") return TaskStatus::InProgress;
    if (name == "completed") return TaskStatus::Completed;
    if (name == "blocked") return TaskStatus::Blocked;
    if (name == "cancelled") return TaskStatus::Cancelled;
    if (name == "review") return TaskStatus::Review;
    return std::nullopt;
}

std::optional<CriterionStatus> criterion_status_from_string(std::string_view name) noexcept {
    if (name == "pending") return CriterionStatus::Pending;
    if (name == "verified") return CriterionStatus::Verified;
    if (name == "failed") return CriterionStatus::Failed;
    return std::nullopt;
}

const std::vector<std::string>& standard_views() {
    static const std::vector<std::string> views = {"list",  "kanban", "tree",   "timeline",
                                                   "table", "graph",  "summary"};
    return views;
}

const std::vector<std::string>& standard_directives() {
    static const std::vector<std::string> directives = {
        "project", "sprint", "version", "author", "created", "updated", "agent", "agent-id"};
    return directives;
}

std::string to_string(ErrorCode code) { return "E" + std::to_string(static_cast<int>(code)); }

const char* to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "error";
}

// ---------------------------------------------------------------------------
// StringMap
// ---------------------------------------------------------------------------

StringMap::StringMap(std::initializer_list<value_type> items) {
    for (const auto& kv : items) set(kv.first, kv.second);
}

const std::string* StringMap::get(std::string_view key) const noexcept {
    for (const auto& kv : items_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string StringMap::value_or(std::string_view key, std::string fallback) const {
    const std::string* v = get(key);
    return v ? *v : std::move(fallback);
}

void StringMap::set(std::string key, std::string value) {
    for (auto& kv : items_) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    }
    items_.emplace_back(std::move(key), std::move(value));
}

bool StringMap::erase(std::string_view key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->first == key) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Equality (source line numbers are deliberately ignored)
// ---------------------------------------------------------------------------

bool operator==(const Criterion& a, const Criterion& b) {
    return a.description == b.description && a.status == b.status && a.evidence == b.evidence;
}

bool operator==(const Task& a, const Task& b) {
    return a.status == b.status && a.description == b.description && a.id == b.id &&
           a.priority == b.priority && a.estimate == b.estimate && a.assignee == b.assignee &&
           a.tags == b.tags && a.due == b.due && a.depends_on == b.depends_on &&
           a.blocked_by == b.blocked_by && a.subtasks == b.subtasks && a.criteria == b.criteria &&
           a.notes == b.notes;
}

bool operator==(const Section& a, const Section& b) {
    return a.title == b.title && a.level == b.level && a.tasks == b.tasks;
}

bool operator==(const AgentContext& a, const AgentContext& b) {
    return a.agent_id == b.agent_id && a.model == b.model && a.session_id == b.session_id &&
           a.started_at == b.started_at && a.metadata == b.metadata;
}

bool operator==(const Handoff& a, const Handoff& b) {
    return a.from == b.from && a.to == b.to && a.reason == b.reason && a.context == b.context;
}

bool operator==(const Document& a, const Document& b) {
    return a.version == b.version && a.directives == b.directives && a.tasks == b.tasks &&
           a.sections == b.sections && a.includes == b.includes && a.view == b.view &&
           a.agent_context == b.agent_context && a.handoff == b.handoff &&
           a.comments == b.comments;
}

// ---------------------------------------------------------------------------
// Document helpers
// ---------------------------------------------------------------------------

namespace {

template <typename Vec, typename Fn>
bool visit_tasks(Vec& tasks, Fn&& fn) {
    // Iterative pre-order traversal (no recursion: documents may be deep).
    std::vector<std::pair<Vec*, std::size_t>> stack{{&tasks, 0}};
    while (!stack.empty()) {
        auto& [vec, idx] = stack.back();
        if (idx >= vec->size()) {
            stack.pop_back();
            continue;
        }
        auto& task = (*vec)[idx++];
        if (fn(task)) return true;
        if (!task.subtasks.empty()) stack.emplace_back(&task.subtasks, 0);
    }
    return false;
}

}  // namespace

const Task* Document::find_task(std::string_view id) const {
    const Task* found = nullptr;
    auto fn = [&](const Task& t) {
        if (t.id && *t.id == id) {
            found = &t;
            return true;
        }
        return false;
    };
    if (visit_tasks(tasks, fn)) return found;
    for (const auto& s : sections)
        if (visit_tasks(s.tasks, fn)) return found;
    return nullptr;
}

Task* Document::find_task(std::string_view id) {
    return const_cast<Task*>(static_cast<const Document&>(*this).find_task(id));
}

std::vector<const Task*> Document::all_tasks() const {
    std::vector<const Task*> out;
    auto fn = [&](const Task& t) {
        out.push_back(&t);
        return false;
    };
    visit_tasks(tasks, fn);
    for (const auto& s : sections) visit_tasks(s.tasks, fn);
    return out;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::string Diagnostic::format(std::string_view source) const {
    std::string out = std::string(to_string(severity)) + "[" + to_string(code) + "]: " + message;
    if (line > 0) out += " (" + std::to_string(line) + ":" + std::to_string(column) + ")";
    if (!suggestion.empty()) out += "\n  suggestion: " + suggestion;
    if (!source.empty() && line > 0) {
        // Locate the line (accepting \n, \r\n and \r line endings).
        int current = 1;
        std::size_t start = 0;
        for (std::size_t i = 0; i < source.size() && current < line; ++i) {
            if (source[i] == '\n' || source[i] == '\r') {
                if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n') ++i;
                ++current;
                start = i + 1;
            }
        }
        if (current == line && start <= source.size()) {
            std::size_t end = source.find_first_of("\r\n", start);
            if (end == std::string_view::npos) end = source.size();
            std::string ln = std::to_string(line);
            out += "\n  " + ln + " | " + std::string(source.substr(start, end - start));
            out += "\n  " + std::string(ln.size(), ' ') + " | " +
                   std::string(static_cast<std::size_t>(column > 1 ? column - 1 : 0), ' ') + "^";
        }
    }
    return out;
}

Error::Error(const std::string& message, std::vector<Diagnostic> diagnostics)
    : std::runtime_error(message), diagnostics_(std::move(diagnostics)) {}

}  // namespace taskml
