// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "taskml/taskml.hpp"

namespace taskml {

namespace {

Diagnostic make(ErrorCode code, Severity sev, int line, std::string msg, std::string suggestion = {}) {
    Diagnostic d;
    d.code = code;
    d.severity = sev;
    d.line = line;
    d.column = line > 0 ? 1 : 0;
    d.message = std::move(msg);
    d.suggestion = std::move(suggestion);
    return d;
}

}  // namespace

std::vector<Diagnostic> validate(const Document& doc) {
    std::vector<Diagnostic> out;
    const std::vector<const Task*> tasks = doc.all_tasks();

    // Duplicate IDs (E209) and invalid priorities (E106).
    std::unordered_map<std::string, const Task*> by_id;
    for (const Task* t : tasks) {
        if (t->priority && (*t->priority < 0 || *t->priority > 3)) {
            out.push_back(make(ErrorCode::InvalidPriority, Severity::Error, t->line,
                               "Invalid priority " + std::to_string(*t->priority) +
                                   ": expected #p0, #p1, #p2, or #p3"));
        }
        if (!t->id) continue;
        auto [it, inserted] = by_id.emplace(*t->id, t);
        if (!inserted) {
            out.push_back(make(ErrorCode::DuplicateTaskId, Severity::Error, t->line,
                               "Duplicate task ID \"" + *t->id + "\"" +
                                   (it->second->line ? " (first defined on line " +
                                                           std::to_string(it->second->line) + ")"
                                                     : std::string()),
                               "Each ^id must be unique. Rename one of the duplicates."));
        }
    }

    // Unknown references (E303 / E306).
    for (const Task* t : tasks) {
        for (const auto& dep : t->depends_on) {
            if (!by_id.count(dep))
                out.push_back(make(ErrorCode::UnknownDependency, Severity::Error, t->line,
                                   "Unknown task dependency \"" + dep + "\"",
                                   "Ensure the referenced task exists with a matching ^id"));
        }
        for (const auto& dep : t->blocked_by) {
            if (!by_id.count(dep))
                out.push_back(make(ErrorCode::UnknownBlocker, Severity::Error, t->line,
                                   "Unknown blocker task \"" + dep + "\"",
                                   "Ensure the referenced task exists with a matching ^id"));
        }
    }

    // Circular dependencies (E304). Edges: task -> each task it waits for
    // (depends: and blocked-by:). Iterative DFS with colouring.
    enum Color { White, Gray, Black };
    std::map<std::string, Color> color;
    for (const auto& kv : by_id) color[kv.first] = White;
    auto edges = [&](const std::string& id) {
        std::vector<std::string> e;
        const Task* t = by_id.at(id);
        for (const auto& d : t->depends_on)
            if (by_id.count(d)) e.push_back(d);
        for (const auto& d : t->blocked_by)
            if (by_id.count(d)) e.push_back(d);
        return e;
    };
    // Visit IDs in document order for deterministic output.
    std::vector<std::string> order;
    for (const Task* t : tasks)
        if (t->id && by_id.at(*t->id) == t) order.push_back(*t->id);

    for (const auto& root : order) {
        if (color[root] != White) continue;
        struct Frame {
            std::string id;
            std::vector<std::string> next;
            std::size_t idx;
        };
        std::vector<Frame> stack;
        color[root] = Gray;
        stack.push_back({root, edges(root), 0});
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.idx >= f.next.size()) {
                color[f.id] = Black;
                stack.pop_back();
                continue;
            }
            std::string n = f.next[f.idx++];
            if (color[n] == Gray) {
                // Report the cycle path.
                std::string path;
                auto start = std::find_if(stack.begin(), stack.end(),
                                          [&](const Frame& fr) { return fr.id == n; });
                for (auto it = start; it != stack.end(); ++it) path += "^" + it->id + " -> ";
                path += "^" + n;
                out.push_back(make(ErrorCode::CircularDependency, Severity::Error,
                                   by_id.at(f.id)->line, "Circular dependency detected: " + path,
                                   "Remove one of the dependencies to break the cycle"));
            } else if (color[n] == White) {
                color[n] = Gray;
                stack.push_back({n, edges(n), 0});
            }
        }
    }

    // View type (E301).
    if (doc.view) {
        const auto& views = standard_views();
        if (std::find(views.begin(), views.end(), doc.view->type) == views.end()) {
            out.push_back(make(ErrorCode::InvalidViewType, Severity::Warning, 0,
                               "Invalid view type \"" + doc.view->type +
                                   "\": expected list, kanban, tree, timeline, table, graph, or summary",
                               "Valid views: list, kanban, tree, timeline, table, graph, summary"));
        }
    }
    return out;
}

std::vector<Diagnostic> validate(std::string_view text) { return validate(text, Dialect::TaskML); }

std::vector<Diagnostic> validate(std::string_view text, Dialect dialect) {
    ParseOptions opts;
    opts.dialect = dialect;
    opts.strict = true;
    // Markdown front matter is open-ended (team, goal, ...), so only TaskML
    // reports unknown directives.
    opts.allow_unknown_directives = dialect == Dialect::Markdown;
    ParseResult r = parse(text, opts);

    std::vector<Diagnostic> out = std::move(r.errors);
    out.insert(out.end(), r.warnings.begin(), r.warnings.end());
    bool view_reported = std::any_of(out.begin(), out.end(), [](const Diagnostic& d) {
        return d.code == ErrorCode::InvalidViewType;
    });
    for (auto& d : validate(r.document)) {
        if (view_reported && d.code == ErrorCode::InvalidViewType) continue;
        out.push_back(std::move(d));
    }
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.column < b.column;
    });
    return out;
}

}  // namespace taskml
