// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// TaskML parser.
//
// TaskML is line oriented, so the parser classifies each physical line and
// uses an indentation stack to build the task hierarchy. Inline task
// metadata (#p0 ~4h @alice #tag !2026-01-01 ^id) is scanned per line.

#include <algorithm>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "taskml/taskml.hpp"
#include "text_util.hpp"

namespace taskml {

namespace {

using namespace detail;

constexpr int kTabWidth = 4;

struct Line {
    int number = 0;           // 1-based
    int indent = 0;           // indentation width (tab = 4 columns)
    int content_column = 1;   // 1-based byte column of the first content char
    std::string_view raw;     // the full line without terminator
    std::string_view content; // the line without indentation / trailing ws
};

// Split into lines, accepting \n, \r\n and \r terminators.
std::vector<Line> split_lines(std::string_view text) {
    if (starts_with(text, "\xEF\xBB\xBF")) text.remove_prefix(3);  // UTF-8 BOM
    std::vector<Line> lines;
    std::size_t start = 0;
    int number = 1;
    auto push = [&](std::size_t end) {
        Line l;
        l.number = number++;
        l.raw = text.substr(start, end - start);
        std::size_t i = 0;
        int indent = 0;
        while (i < l.raw.size() && is_space(l.raw[i])) {
            indent += l.raw[i] == '\t' ? kTabWidth : 1;
            ++i;
        }
        l.indent = indent;
        l.content_column = static_cast<int>(i) + 1;
        std::string_view rest = l.raw.substr(i);
        while (!rest.empty() && is_space(rest.back())) rest.remove_suffix(1);
        l.content = rest;
        lines.push_back(l);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            push(i);
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < text.size()) push(text.size());
    return lines;
}

std::optional<TaskStatus> status_from_marker(char c) {
    switch (c) {
        case ' ': return TaskStatus::Pending;
        case '~': return TaskStatus::InProgress;
        case 'x': case 'X': return TaskStatus::Completed;
        case '!': return TaskStatus::Blocked;
        case '-': return TaskStatus::Cancelled;
        case '?': return TaskStatus::Review;
        default: return std::nullopt;
    }
}

// Returns the criterion status and the length of the marker (bare or
// bracketed) if `s` starts with a verification marker.
std::optional<std::pair<CriterionStatus, std::size_t>> match_criterion(std::string_view s) {
    auto bare = [](std::string_view t) -> std::optional<CriterionStatus> {
        if (starts_with(t, kCheck) || starts_with(t, kHeavyCheck)) return CriterionStatus::Verified;
        if (starts_with(t, kBallotX) || starts_with(t, kHeavyBallotX)) return CriterionStatus::Failed;
        if (starts_with(t, kCircle)) return CriterionStatus::Pending;
        return std::nullopt;
    };
    if (auto st = bare(s)) return std::make_pair(*st, std::size_t{3});
    // ASCII forms, easy to type: ( ) pending, (x) verified, (!) failed.
    if (s.size() >= 3 && s[0] == '(' && s[2] == ')' && (s.size() == 3 || is_space(s[3]))) {
        switch (s[1]) {
            case ' ': return std::make_pair(CriterionStatus::Pending, std::size_t{3});
            case 'x': case 'X': return std::make_pair(CriterionStatus::Verified, std::size_t{3});
            case '!': return std::make_pair(CriterionStatus::Failed, std::size_t{3});
            default: break;
        }
    }
    if (s.size() >= 5 && s[0] == '[' && s[4] == ']') {
        if (auto st = bare(s.substr(1))) return std::make_pair(*st, std::size_t{5});
    }
    return std::nullopt;
}

bool is_valid_date(std::string_view d) {
    // d has the shape DDDD-DD-DD
    int y = (d[0] - '0') * 1000 + (d[1] - '0') * 100 + (d[2] - '0') * 10 + (d[3] - '0');
    int m = (d[5] - '0') * 10 + (d[6] - '0');
    int day = (d[8] - '0') * 10 + (d[9] - '0');
    if (m < 1 || m > 12 || day < 1) return false;
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max = kDays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
    return day <= max;
}

// Canonical token order for strict mode (E305).
enum class TokenKind { Description = 0, Priority, Estimate, Assignee, Tag, Due, Id };

const char* token_kind_name(TokenKind k) {
    switch (k) {
        case TokenKind::Description: return "description";
        case TokenKind::Priority: return "priority";
        case TokenKind::Estimate: return "estimate";
        case TokenKind::Assignee: return "assignee";
        case TokenKind::Tag: return "tag";
        case TokenKind::Due: return "due date";
        case TokenKind::Id: return "task ID";
    }
    return "token";
}

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& options) : text_(text), opts_(options) {}

    ParseResult run() {
        lines_ = split_lines(text_);
        if (opts_.dialect == Dialect::Markdown) {
            for (const Line& line : lines_) handle_markdown_line(line);
            if (md_.front_matter == FrontMatter::Open)
                error(ErrorCode::ExpectedFenceEnd, 1, 1, "Unterminated front matter: expected ---",
                      "Close the front matter with --- on its own line");
            if (const std::string* v = doc_.directives.get("version")) doc_.version = *v;
            return std::move(result_);
        }
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            if (block_ != Block::None) {
                handle_block_line(lines_[i]);
                continue;
            }
            handle_line(lines_[i]);
        }
        if (block_ != Block::None) {
            error(ErrorCode::ExpectedFenceEnd, block_start_line_, 1,
                  std::string("Unterminated ---") + (block_ == Block::Context ? "context" : "handoff") +
                      " block: expected fence end ---",
                  "Close the block with --- on its own line");
            finish_block();
        }
        if (const std::string* v = doc_.directives.get("version")) doc_.version = *v;
        return std::move(result_);
    }

private:
    enum class Block { None, Context, Handoff };

    struct Frame {
        int indent;
        Task* task;
        int child_indent = -1;  // indentation established by the first child
    };

    // ----- diagnostics -----

    void report(Severity sev, ErrorCode code, int line, int column, std::string msg,
                std::string suggestion = {}) {
        Diagnostic d;
        d.code = code;
        d.severity = sev;
        d.line = line;
        d.column = column;
        d.message = std::move(msg);
        d.suggestion = std::move(suggestion);
        (sev == Severity::Error ? result_.errors : result_.warnings).push_back(std::move(d));
    }
    void error(ErrorCode code, int line, int column, std::string msg, std::string suggestion = {}) {
        report(Severity::Error, code, line, column, std::move(msg), std::move(suggestion));
    }
    void warn(ErrorCode code, int line, int column, std::string msg, std::string suggestion = {}) {
        report(Severity::Warning, code, line, column, std::move(msg), std::move(suggestion));
    }

    // ----- structure -----

    std::vector<Task>& root_tasks() {
        return current_section_ ? doc_.sections.back().tasks : doc_.tasks;
    }

    // Pops the indentation stack down to the frame that owns a line with the
    // given indentation; returns it (or nullptr for top level). When
    // `check_consistency` is set, a sibling with a different indentation than
    // its earlier siblings is reported (E101).
    Frame* owner_for(const Line& line, bool check_consistency = true) {
        while (!stack_.empty() && stack_.back().indent >= line.indent) stack_.pop_back();
        if (stack_.empty()) return nullptr;
        Frame& parent = stack_.back();
        if (check_consistency) {
            if (parent.child_indent < 0) {
                parent.child_indent = line.indent;
            } else if (parent.child_indent != line.indent) {
                error(ErrorCode::InconsistentIndentation, line.number, line.content_column,
                      "Inconsistent indentation: expected " + std::to_string(parent.child_indent) +
                          " spaces, got " + std::to_string(line.indent),
                      "Use consistent indentation (4 spaces per level recommended)");
            }
        }
        return &parent;
    }

    void reset_structure() { stack_.clear(); }

    // ----- line dispatch -----

    void handle_line(const Line& line) {
        std::string_view c = line.content;
        if (c.empty()) return;

        if (starts_with(c, "//")) {
            doc_.comments.push_back({line.number, std::string(trim(c.substr(2)))});
            return;
        }

        // Lines indented below a note: nested notes ("- ...") and wrapped
        // continuation text. Not covered by the spec, but common in practice.
        const NoteRef prev_note = std::exchange(last_note_, NoteRef{});
        if (prev_note.task && line.indent > prev_note.indent && c[0] == '-' && !starts_with(c, "---")) {
            owner_for(line, /*check_consistency=*/false);
            prev_note.task->notes.push_back(unescape(trim(c.substr(1))));
            last_note_ = prev_note;
            return;
        }

        if (starts_with(c, "---")) {
            reset_structure();
            handle_fence(line);
            return;
        }
        if (c[0] == '@' && line.indent == 0) {
            reset_structure();
            handle_directive(line);
            return;
        }
        if (starts_with(c, "==")) {
            reset_structure();
            handle_section(line);
            return;
        }
        if (c[0] == '<') {
            reset_structure();
            std::string_view path = trim(c.substr(1));
            if (path.empty()) {
                warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                     "Include line without a path");
                return;
            }
            doc_.includes.push_back({std::string(path), line.number});
            return;
        }
        if (auto crit = match_criterion(c)) {
            handle_criterion(line, crit->first, crit->second);
            return;
        }
        if (c.size() >= 3 && c[0] == '[' && c[2] == ']') {
            if (auto st = status_from_marker(c[1])) {
                handle_task(line, *st);
                return;
            }
            warn(ErrorCode::UnknownStatus, line.number, line.content_column,
                 std::string("Unknown task status \"[") + c[1] + "]\"; line ignored",
                 "Start task lines with [ ], [x], [~], [!], [-], or [?]");
            return;
        }
        if (c[0] == '-') {
            handle_note(line);
            return;
        }
        if (starts_with(c, "depends:") || starts_with(c, "depends-on:")) {
            handle_dependencies(line, c.substr(c.find(':') + 1), /*blocked=*/false);
            return;
        }
        if (starts_with(c, "blocked-by:")) {
            handle_dependencies(line, c.substr(c.find(':') + 1), /*blocked=*/true);
            return;
        }
        if (starts_with(c, "evidence:")) {
            handle_evidence(line, c.substr(9));
            return;
        }
        if (prev_note.task && line.indent > prev_note.indent && !prev_note.task->notes.empty()) {
            std::string& note = prev_note.task->notes.back();
            note += note.empty() ? "" : " ";
            note += unescape(c);
            last_note_ = prev_note;
            return;
        }
        warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
             "Unrecognized line ignored: \"" + std::string(c.substr(0, 40)) +
                 (c.size() > 40 ? "..." : "") + "\"");
    }

    // ----- directives -----

    void handle_directive(const Line& line) {
        std::string_view c = line.content.substr(1);
        std::size_t n = 0;
        while (n < c.size() && is_ident_char(c[n])) ++n;
        set_directive(line, std::string(c.substr(0, n)), std::string(trim(c.substr(n))),
                      line.content_column + 1 + int(n));
    }

    void set_directive(const Line& line, std::string name, std::string value, int value_column) {
        if (name.empty()) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 "Directive without a name ignored");
            return;
        }
        if (value.empty()) {
            warn(ErrorCode::ExpectedDirectiveValue, line.number, value_column,
                 "Expected value after directive @" + name);
        }
        if (!opts_.allow_unknown_directives && !starts_with(name, "x-")) {
            const auto& std_dirs = standard_directives();
            if (std::find(std_dirs.begin(), std_dirs.end(), name) == std_dirs.end()) {
                warn(ErrorCode::UnknownDirective, line.number, line.content_column,
                     "Unknown directive @" + name,
                     "Standard directives: @project, @sprint, @version, @author, @created, "
                     "@updated, @agent, @agent-id; custom directives use the @x- prefix");
            }
        }
        if (name == "version" && !value.empty() && value != "1.1" && value != "1.0" &&
            !starts_with(value, "1.")) {
            warn(ErrorCode::InvalidVersion, line.number, line.content_column,
                 "Unsupported TaskML version \"" + value + "\"; this parser implements 1.1");
        }
        if (name == "agent") {
            if (!doc_.agent_context) doc_.agent_context.emplace();
            doc_.agent_context->model = value;
        } else if (name == "agent-id") {
            if (!doc_.agent_context) doc_.agent_context.emplace();
            doc_.agent_context->agent_id = value;
        }
        doc_.directives.set(std::move(name), std::move(value));
    }

    // ----- sections -----

    void handle_section(const Line& line) {
        std::string_view c = line.content;
        std::size_t level = 0;
        while (level < c.size() && c[level] == '=') ++level;
        std::string_view rest = c.substr(level);
        // Trailing '=' run is optional.
        std::size_t end = rest.size();
        while (end > 0 && rest[end - 1] == '=') --end;
        std::string_view title = trim(rest.substr(0, end));
        if (title.empty()) {
            warn(ErrorCode::InvalidSection, line.number, line.content_column,
                 "Invalid section header: expected == title ==");
            return;
        }
        Section s;
        s.title = std::string(title);
        s.level = static_cast<int>(level);
        s.line = line.number;
        doc_.sections.push_back(std::move(s));
        current_section_ = true;
    }

    // ----- fences and blocks -----

    void handle_fence(const Line& line) {
        std::string_view rest = line.content.substr(3);
        if (rest.empty()) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 "Fence end --- without an open block");
            return;
        }
        if (starts_with(rest, "view:")) {
            handle_view(line, rest.substr(5));
            return;
        }
        std::string_view word = rest;
        std::size_t n = 0;
        while (n < word.size() && is_ident_char(word[n])) ++n;
        word = word.substr(0, n);
        if (word == "context" || word == "handoff") {
            block_ = word == "context" ? Block::Context : Block::Handoff;
            block_start_line_ = line.number;
            block_lines_.clear();
            return;
        }
        warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
             "Unknown fence \"---" + std::string(rest) + "\" ignored");
    }

    void handle_view(const Line& line, std::string_view spec) {
        std::size_t n = 0;
        while (n < spec.size() && is_ident_char(spec[n])) ++n;
        ViewConfig view;
        view.type = std::string(spec.substr(0, n));
        const auto& views = standard_views();
        if (view.type.empty()) {
            warn(ErrorCode::InvalidViewType, line.number, line.content_column,
                 "Missing view type after ---view:",
                 "Valid views: list, kanban, tree, timeline, table, graph, summary");
        } else if (std::find(views.begin(), views.end(), view.type) == views.end()) {
            warn(ErrorCode::InvalidViewType, line.number, line.content_column + 8,
                 "Invalid view type \"" + view.type +
                     "\": expected list, kanban, tree, timeline, table, graph, or summary",
                 "Valid views: list, kanban, tree, timeline, table, graph, summary");
        }
        std::string_view opts = spec.substr(n);
        std::size_t i = 0;
        while (i < opts.size()) {
            while (i < opts.size() && is_space(opts[i])) ++i;
            std::size_t start = i;
            while (i < opts.size() && !is_space(opts[i])) ++i;
            std::string_view item = opts.substr(start, i - start);
            if (item.empty()) continue;
            std::size_t eq = item.find('=');
            if (eq == std::string_view::npos || eq == 0) {
                warn(ErrorCode::UnexpectedToken, line.number,
                     line.content_column + 8 + static_cast<int>(n + start),
                     "View option \"" + std::string(item) + "\" is not key=value; ignored");
                continue;
            }
            view.options.set(std::string(item.substr(0, eq)), std::string(item.substr(eq + 1)));
        }
        if (doc_.view) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 "Multiple view fences; the last one is used");
        }
        doc_.view = std::move(view);
    }

    void handle_block_line(const Line& line) {
        if (line.content == "---" ) {
            finish_block();
            return;
        }
        block_lines_.push_back(line);
    }

    void finish_block() {
        if (block_ == Block::Context) {
            std::string body;
            for (const auto& l : block_lines_) {
                body.append(l.raw);
                body += '\n';
            }
            if (!doc_.agent_context) doc_.agent_context.emplace();
            AgentContext& ctx = *doc_.agent_context;
            try {
                Json j = Json::parse(body);
                if (j.is_object()) {
                    auto take = [&](const char* key, std::optional<std::string>& field) {
                        if (const Json* v = j.find(key); v && v->is_string() && !v->as_string().empty())
                            field = v->as_string();
                    };
                    take("session_id", ctx.session_id);
                    take("model", ctx.model);
                    take("started_at", ctx.started_at);
                    take("agent_id", ctx.agent_id);
                }
                ctx.metadata = std::move(j);
            } catch (const JsonParseError& e) {
                std::string_view raw = body;
                auto ws = [](char ch) { return is_space(ch) || ch == '\n'; };
                while (!raw.empty() && ws(raw.front())) raw.remove_prefix(1);
                while (!raw.empty() && ws(raw.back())) raw.remove_suffix(1);
                Json m = Json::object();
                m.set("raw", std::string(raw));
                ctx.metadata = std::move(m);
                if (!raw.empty())
                    warn(ErrorCode::InvalidJson, block_start_line_, 1,
                         std::string("---context block is not valid JSON (") + e.what() +
                             "); stored as raw text");
            }
        } else if (block_ == Block::Handoff) {
            Handoff h;
            for (const auto& l : block_lines_) {
                std::string_view c = l.content;
                if (c.empty()) continue;
                std::size_t colon = c.find(':');
                if (colon == std::string_view::npos) {
                    warn(ErrorCode::UnexpectedToken, l.number, l.content_column,
                         "Expected key: value in ---handoff block");
                    continue;
                }
                std::string_view key = trim(c.substr(0, colon));
                std::string_view value = trim(c.substr(colon + 1));
                if (key == "from") {
                    h.from = std::string(unquote(value));
                } else if (key == "to") {
                    h.to = std::string(unquote(value));
                } else if (key == "reason") {
                    h.reason = std::string(unquote(value));
                } else if (key == "context") {
                    try {
                        h.context = Json::parse(value);
                    } catch (const JsonParseError&) {
                        Json m = Json::object();
                        m.set("raw", std::string(value));
                        h.context = std::move(m);
                    }
                } else {
                    warn(ErrorCode::UnexpectedToken, l.number, l.content_column,
                         "Unknown handoff field \"" + std::string(key) + "\" ignored");
                }
            }
            if (h.from.empty() || h.to.empty()) {
                warn(ErrorCode::MissingRequiredField, block_start_line_, 1,
                     "---handoff block should declare both from: and to:");
            }
            doc_.handoff = std::move(h);
        }
        block_ = Block::None;
        block_lines_.clear();
    }

    // ----- tasks -----

    void handle_task(const Line& line, TaskStatus status) {
        Frame* parent = owner_for(line);
        int depth = static_cast<int>(stack_.size()) + 1;
        if (opts_.max_depth > 0 && depth > opts_.max_depth) {
            error(ErrorCode::InvalidNesting, line.number, line.content_column,
                  "Invalid nesting: maximum depth of " + std::to_string(opts_.max_depth) +
                      " exceeded; task dropped",
                  "Consider restructuring tasks.");
            return;
        }
        Task task;
        task.status = status;
        task.line = line.number;
        scan_task_content(line, task);

        Task* placed;
        if (parent) {
            parent->task->subtasks.push_back(std::move(task));
            placed = &parent->task->subtasks.back();
        } else {
            auto& roots = root_tasks();
            roots.push_back(std::move(task));
            placed = &roots.back();
        }
        stack_.push_back({line.indent, placed});
    }

    // Scan "description #p0 ~4h @alice #tag !2026-01-01 ^id" (text after the
    // status marker).
    void scan_task_content(const Line& line, Task& task) {
        const std::string_view s = line.content.substr(3);
        const int base_col = line.content_column + 3;
        std::string desc;
        bool prev_token = false;          // previous item was a token
        int last_order = -1;              // strict mode ordering
        const char* last_name = nullptr;
        bool desc_after_token_reported = false;

        auto note_order = [&](TokenKind kind, std::size_t pos) {
            if (!opts_.strict) return;
            int order = static_cast<int>(kind);
            if (order < last_order) {
                if (kind != TokenKind::Description || !desc_after_token_reported) {
                    warn(ErrorCode::TokenOrderViolation, line.number, base_col + static_cast<int>(pos),
                         std::string("Token order: ") + token_kind_name(kind) +
                             " should appear before " + last_name,
                         "Recommended order: description #priority ~estimate @assignee #tags !due ^id");
                }
                if (kind == TokenKind::Description) desc_after_token_reported = true;
            }
            if (order >= last_order) {
                last_order = order;
                last_name = token_kind_name(kind);
            }
        };

        auto ident_len = [&](std::size_t from) {
            std::size_t j = from;
            while (j < s.size() && is_ident_char(s[j])) ++j;
            return j - from;
        };
        auto boundary = [&](std::size_t j) { return j >= s.size() || !is_ident_char(s[j]); };

        std::size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            const int col = base_col + static_cast<int>(i);
            if (c == '\\') {
                std::size_t n = escapable_len(s.substr(i + 1));
                if (n) {
                    note_order(TokenKind::Description, i);
                    desc.append(s.substr(i + 1, n));
                    i += 1 + n;
                    prev_token = false;
                    continue;
                }
            }
            const bool word_start = i == 0 || is_space(s[i - 1]) || prev_token;
            if (word_start) {
                std::size_t consumed = 0;
                if (c == '#' && i + 2 < s.size() && s[i + 1] == 'p' && is_digit(s[i + 2]) &&
                    boundary(i + 3)) {
                    int p = s[i + 2] - '0';
                    if (p <= 3) {
                        note_order(TokenKind::Priority, i);
                        if (task.priority && *task.priority != p)
                            warn(ErrorCode::UnexpectedToken, line.number, col,
                                 "Multiple priorities on one task; the last one is used");
                        task.priority = p;
                        consumed = 3;
                    } else {
                        warn(ErrorCode::InvalidPriority, line.number, col,
                             "Invalid priority: expected #p0, #p1, #p2, or #p3; treated as tag",
                             "Use #p0 (critical), #p1 (high), #p2 (medium), or #p3 (low)");
                    }
                }
                if (!consumed && c == '#') {
                    std::size_t n = ident_len(i + 1);
                    if (n) {
                        note_order(TokenKind::Tag, i);
                        std::string tag(s.substr(i + 1, n));
                        if (std::find(task.tags.begin(), task.tags.end(), tag) == task.tags.end())
                            task.tags.push_back(std::move(tag));
                        consumed = 1 + n;
                    }
                } else if (c == '~') {
                    std::size_t j = i + 1;
                    while (j < s.size() && is_digit(s[j])) ++j;
                    if (j > i + 1 && j < s.size() && (s[j] == 'h' || s[j] == 'd' || s[j] == 'w') &&
                        boundary(j + 1)) {
                        note_order(TokenKind::Estimate, i);
                        if (s[j] == 'w' && opts_.strict)
                            warn(ErrorCode::InvalidEstimateFormat, line.number, col,
                                 "Week estimates (~Nw) are not part of TaskML 1.1; use ~Nd",
                                 "Use ~Nh for hours or ~Nd for days (e.g., ~4h, ~2d)");
                        task.estimate = std::string(s.substr(i + 1, j - i));
                        consumed = j + 1 - i;
                    } else if (j > i + 1 && j < s.size() && is_ident_char(s[j])) {
                        warn(ErrorCode::InvalidEstimateFormat, line.number, col,
                             "Invalid estimate format: expected Nh or Nd (e.g., 4h, 2d); treated as text",
                             "Use ~Nh for hours or ~Nd for days (e.g., ~4h, ~2d)");
                    }
                } else if (c == '!') {
                    std::string_view d = s.substr(i + 1, 10);
                    bool shape = d.size() == 10 && is_digit(d[0]) && is_digit(d[1]) && is_digit(d[2]) &&
                                 is_digit(d[3]) && d[4] == '-' && is_digit(d[5]) && is_digit(d[6]) &&
                                 d[7] == '-' && is_digit(d[8]) && is_digit(d[9]) && boundary(i + 11);
                    if (shape) {
                        note_order(TokenKind::Due, i);
                        if (!is_valid_date(d))
                            warn(ErrorCode::InvalidDateFormat, line.number, col,
                                 "Invalid date \"" + std::string(d) + "\": no such calendar day",
                                 "Use ISO 8601 format: !YYYY-MM-DD (e.g., !2026-01-15)");
                        task.due = std::string(d);
                        consumed = 11;
                    } else if (i + 1 < s.size() && is_digit(s[i + 1]) && ident_len(i + 1) >= 4 &&
                               s.substr(i + 1).find('-') < 5) {
                        warn(ErrorCode::InvalidDateFormat, line.number, col,
                             "Invalid date format: expected YYYY-MM-DD; treated as text",
                             "Use ISO 8601 format: !YYYY-MM-DD (e.g., !2026-01-15)");
                    }
                } else if (c == '^') {
                    std::size_t n = ident_len(i + 1);
                    if (n) {
                        note_order(TokenKind::Id, i);
                        if (task.id)
                            warn(ErrorCode::UnexpectedToken, line.number, col,
                                 "Multiple IDs on one task; the last one is used");
                        task.id = std::string(s.substr(i + 1, n));
                        consumed = 1 + n;
                    }
                } else if (c == '@') {
                    std::size_t n = ident_len(i + 1);
                    if (n) {
                        note_order(TokenKind::Assignee, i);
                        if (task.assignee)
                            warn(ErrorCode::MultipleAssignees, line.number, col,
                                 "Multiple assignees not allowed on a single task; the last one is used");
                        task.assignee = std::string(s.substr(i + 1, n));
                        consumed = 1 + n;
                    }
                }
                if (consumed) {
                    i += consumed;
                    prev_token = true;
                    desc += ' ';  // keep words separated; whitespace is normalized below
                    continue;
                }
            }
            if (!is_space(c)) note_order(TokenKind::Description, i);
            desc += c;
            prev_token = false;
            ++i;
        }

        // Normalize whitespace: collapse runs and trim.
        std::string norm;
        norm.reserve(desc.size());
        for (char ch : desc) {
            if (is_space(ch)) {
                if (!norm.empty() && norm.back() != ' ') norm += ' ';
            } else {
                norm += ch;
            }
        }
        if (!norm.empty() && norm.back() == ' ') norm.pop_back();
        task.description = std::move(norm);
        if (task.description.empty()) {
            warn(ErrorCode::ExpectedDescription, line.number, base_col,
                 "Expected task description after status");
        }
    }

    // ----- task children -----

    Task* child_owner(const Line& line, const char* what, bool check_consistency = true) {
        Frame* f = owner_for(line, check_consistency);
        if (!f) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 std::string(what) + " outside of a task ignored",
                 "Indent the line under the task it belongs to");
            return nullptr;
        }
        return f->task;
    }

    void handle_note(const Line& line) { add_note(line, line.content.substr(1)); }

    void add_note(const Line& line, std::string_view text) {
        Task* t = child_owner(line, "Note");
        if (!t) return;
        t->notes.push_back(unescape(trim(text)));
        last_note_ = {t, line.indent};
    }

    void handle_criterion(const Line& line, CriterionStatus status, std::size_t marker_len) {
        Task* t = child_owner(line, "Verification criterion");
        if (!t) return;
        Criterion cr;
        cr.status = status;
        cr.description = unescape(trim(line.content.substr(marker_len)));
        cr.line = line.number;
        t->criteria.push_back(std::move(cr));
    }

    void handle_dependencies(const Line& line, std::string_view list, bool blocked) {
        Task* t = child_owner(line, blocked ? "blocked-by:" : "depends:");
        if (!t) return;
        auto& out = blocked ? t->blocked_by : t->depends_on;
        std::size_t i = 0;
        while (i < list.size()) {
            while (i < list.size() && (is_space(list[i]) || list[i] == ',')) ++i;
            std::size_t start = i;
            while (i < list.size() && !is_space(list[i]) && list[i] != ',') ++i;
            std::string_view item = list.substr(start, i - start);
            if (!item.empty() && item[0] == '^') item.remove_prefix(1);
            if (item.empty()) continue;
            std::string id(item);
            if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(std::move(id));
        }
        if (out.empty())
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 std::string(blocked ? "blocked-by:" : "depends:") + " without any task IDs");
    }

    void handle_evidence(const Line& line, std::string_view value) {
        // Evidence is usually indented below its criterion, so it does not
        // take part in the sibling indentation check.
        Task* t = child_owner(line, "evidence:", /*check_consistency=*/false);
        if (!t) return;
        if (t->criteria.empty()) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 "evidence: without a preceding verification criterion ignored");
            return;
        }
        t->criteria.back().evidence = unescape(unquote(trim(value)));
    }


    // ----- Markdown dialect -----

    enum class FrontMatter { Unseen, Open, Done };

    struct MarkdownState {
        FrontMatter front_matter = FrontMatter::Unseen;
        char fence_char = 0;        // ` or ~ while inside a fenced code block
        std::size_t fence_len = 0;
        bool in_html_comment = false;
    };

    // Length of the list marker at the start of `c` ("-", "*", "+", "1.", "1)"),
    // or 0 when `c` is not a list item. The marker must be followed by
    // whitespace or end the line.
    static std::size_t list_marker(std::string_view c) {
        std::size_t n = 0;
        if (!c.empty() && (c[0] == '-' || c[0] == '*' || c[0] == '+')) {
            n = 1;
        } else {
            while (n < c.size() && n < 9 && c[n] >= '0' && c[n] <= '9') ++n;
            if (n == 0 || n >= c.size() || (c[n] != '.' && c[n] != ')')) return 0;
            ++n;
        }
        if (n < c.size() && !is_space(c[n])) return 0;
        return n;
    }

    // "---", "***", "___", "- - -" ...
    static bool is_thematic_break(std::string_view c) {
        if (c.empty() || (c[0] != '-' && c[0] != '*' && c[0] != '_')) return false;
        int count = 0;
        for (char ch : c) {
            if (ch == c[0]) ++count;
            else if (!is_space(ch)) return false;
        }
        return count >= 3;
    }

    // Markdown sees prose here: it ends any open list structure.
    void markdown_prose() {
        reset_structure();
        last_note_ = {};
    }

    void handle_markdown_line(const Line& line) {
        std::string_view c = line.content;

        // Front matter: "---" on the very first line opens it.
        if (md_.front_matter == FrontMatter::Unseen) {
            if (line.number == 1 && line.indent == 0 && c == "---") {
                md_.front_matter = FrontMatter::Open;
                return;
            }
            md_.front_matter = FrontMatter::Done;
        }
        if (md_.front_matter == FrontMatter::Open) {
            if (line.indent == 0 && (c == "---" || c == "...")) {
                md_.front_matter = FrontMatter::Done;
                return;
            }
            handle_front_matter_line(line);
            return;
        }

        // Fenced code blocks and HTML comments are skipped whole.
        if (md_.fence_char) {
            std::size_t n = 0;
            while (n < c.size() && c[n] == md_.fence_char) ++n;
            if (n >= md_.fence_len && trim(c.substr(n)).empty()) md_.fence_char = 0;
            return;
        }
        if (md_.in_html_comment) {
            if (c.find("-->") != std::string_view::npos) md_.in_html_comment = false;
            return;
        }
        if (c.empty()) return;
        if (starts_with(c, "```") || starts_with(c, "~~~")) {
            std::size_t n = 0;
            while (n < c.size() && c[n] == c[0]) ++n;
            md_.fence_char = c[0];
            md_.fence_len = n;
            return;
        }
        if (starts_with(c, "<!--")) {
            if (c.find("-->", 4) == std::string_view::npos) md_.in_html_comment = true;
            return;
        }

        const NoteRef prev_note = std::exchange(last_note_, NoteRef{});

        if (line.indent == 0 && c[0] == '#') {
            std::size_t level = 0;
            while (level < c.size() && c[level] == '#') ++level;
            if (level <= 6 && (level == c.size() || is_space(c[level]))) {
                handle_heading(line, static_cast<int>(level), c.substr(level));
                return;
            }
        }
        if (line.indent == 0 && c[0] == '@') {
            reset_structure();
            handle_directive(line);
            return;
        }
        if (is_thematic_break(c)) {
            markdown_prose();
            return;
        }

        const std::size_t marker = list_marker(c);
        if (marker == 0) {
            // A lazy continuation of the previous note, or prose.
            if (prev_note.task && line.indent > prev_note.indent && !prev_note.task->notes.empty()) {
                std::string& note = prev_note.task->notes.back();
                note += note.empty() ? "" : " ";
                note += unescape(c);
                last_note_ = prev_note;
            } else if (line.indent == 0) {
                markdown_prose();
            }
            return;
        }

        // The list item's content, with columns pointing into the source.
        std::size_t skip = marker;
        while (skip < c.size() && is_space(c[skip])) ++skip;
        Line item = line;
        item.content = c.substr(skip);
        item.content_column = line.content_column + static_cast<int>(skip);
        std::string_view ic = item.content;

        if (auto crit = match_criterion(ic)) {
            handle_criterion(item, crit->first, crit->second);
            return;
        }
        if (ic.size() >= 3 && ic[0] == '[' && ic[2] == ']' && (ic.size() == 3 || is_space(ic[3]))) {
            if (auto st = status_from_marker(ic[1])) {
                handle_task(item, *st);
                return;
            }
            warn(ErrorCode::UnknownStatus, line.number, item.content_column,
                 std::string("Unknown task status \"[") + ic[1] + "]\"; line ignored",
                 "Start task items with - [ ], - [x], - [~], - [!], - [-], or - [?]");
            return;
        }
        if (starts_with(ic, "depends:") || starts_with(ic, "depends-on:")) {
            handle_dependencies(item, ic.substr(ic.find(':') + 1), /*blocked=*/false);
            return;
        }
        if (starts_with(ic, "blocked-by:")) {
            handle_dependencies(item, ic.substr(ic.find(':') + 1), /*blocked=*/true);
            return;
        }
        if (starts_with(ic, "evidence:")) {
            handle_evidence(item, ic.substr(9));
            return;
        }

        // A plain list item: a note when it is nested under a task (or under a
        // note, which flattens into the same task), otherwise an ordinary
        // Markdown list.
        if (prev_note.task && line.indent > prev_note.indent) {
            owner_for(line, /*check_consistency=*/false);
            prev_note.task->notes.push_back(unescape(trim(ic)));
            last_note_ = prev_note;
            return;
        }
        while (!stack_.empty() && stack_.back().indent >= line.indent) stack_.pop_back();
        if (stack_.empty()) return;
        add_note(item, ic);
    }

    void handle_heading(const Line& line, int level, std::string_view rest) {
        std::string_view title = trim(rest);
        // Optional closing sequence: "## Title ##".
        std::size_t end = title.size();
        while (end > 0 && title[end - 1] == '#') --end;
        if (end < title.size() && (end == 0 || is_space(title[end - 1]))) title = trim(title.substr(0, end));
        reset_structure();
        if (title.empty()) return;
        Section s;
        s.title = unescape(title);
        s.level = level;
        s.line = line.number;
        doc_.sections.push_back(std::move(s));
        current_section_ = true;
    }

    void handle_front_matter_line(const Line& line) {
        std::string_view c = line.content;
        // Blank lines, comments, nested values and sequence items are not
        // directives.
        if (c.empty() || c[0] == '#' || line.indent > 0 || c[0] == '-') return;
        std::size_t colon = c.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            warn(ErrorCode::UnexpectedToken, line.number, line.content_column,
                 "Front matter line ignored: expected key: value");
            return;
        }
        std::string_view key = trim(c.substr(0, colon));
        std::string_view raw = trim(c.substr(colon + 1));
        // "key:" alone opens a nested map or sequence, which has no directive form.
        if (raw.empty()) return;
        std::string value(raw);
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            value.clear();
            for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 2 < raw.size()) ++i;
                value += raw[i];
            }
        } else if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
            value.clear();
            for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
                if (raw[i] == '\'' && i + 2 < raw.size() && raw[i + 1] == '\'') ++i;
                value += raw[i];
            }
        }
        set_directive(line, std::string(key), std::move(value),
                      line.content_column + static_cast<int>(colon) + 1);
    }

    MarkdownState md_;

    std::string_view text_;
    ParseOptions opts_;
    std::vector<Line> lines_;
    ParseResult result_;
    Document& doc_ = result_.document;
    std::vector<Frame> stack_;
    bool current_section_ = false;

    // The most recent note line (for nested notes / continuation lines).
    // Cleared by every other kind of line, so the pointer stays valid.
    struct NoteRef {
        Task* task = nullptr;
        int indent = 0;
    };
    NoteRef last_note_;

    Block block_ = Block::None;
    int block_start_line_ = 0;
    std::vector<Line> block_lines_;
};

}  // namespace

Dialect dialect_for_path(std::string_view path) noexcept {
    auto ends_with_ci = [&](std::string_view suffix) {
        if (path.size() < suffix.size()) return false;
        for (std::size_t i = 0; i < suffix.size(); ++i) {
            char a = path[path.size() - suffix.size() + i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != suffix[i]) return false;
        }
        return true;
    };
    return ends_with_ci(".md") || ends_with_ci(".markdown") ? Dialect::Markdown : Dialect::TaskML;
}

ParseResult parse(std::string_view text, const ParseOptions& options) {
    return Parser(text, options).run();
}

Document parse_or_throw(std::string_view text, const ParseOptions& options) {
    ParseResult r = parse(text, options);
    if (!r.errors.empty()) {
        std::string msg = "TaskML parse errors:";
        for (const auto& e : r.errors) msg += "\nLine " + std::to_string(e.line) + ": " + e.message;
        throw Error(msg, std::move(r.errors));
    }
    return std::move(r.document);
}

}  // namespace taskml
