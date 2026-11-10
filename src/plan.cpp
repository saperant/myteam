#include "myteam/plan.hpp"

#include <algorithm>
#include <set>
#include <optional>
#include <cctype>
#include <stdexcept>

#include "myteam/util.hpp"

namespace myteam {

using taskml::Task;
using taskml::TaskStatus;

// ---------------------------------------------------------------------------
// TaskRef and helpers
// ---------------------------------------------------------------------------

std::string TaskRef::key() const { return task->id ? "^" + *task->id : "L" + std::to_string(task->line); }

bool is_open(TaskStatus s) { return s == TaskStatus::Pending || s == TaskStatus::InProgress; }
bool is_finished(TaskStatus s) { return s == TaskStatus::Completed || s == TaskStatus::Cancelled; }

std::string status_label(TaskStatus s) {
    std::string out = "[";
    out += taskml::status_marker(s);
    out += "] ";
    std::string name = taskml::to_string(s);
    std::replace(name.begin(), name.end(), '_', ' ');
    return out + name;
}

std::string duplicate_of(const Task& t) {
    if (t.status != TaskStatus::Cancelled) return "";
    for (auto it = t.notes.rbegin(); it != t.notes.rend(); ++it)
        for (const char* p : {"Duplicate of ^", "Merged into ^"})
            if (starts_with(*it, p)) {
                std::string id;
                for (std::size_t i = std::string_view(p).size(); i < it->size(); ++i) {
                    const char c = (*it)[i];
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) break;
                    id += c;
                }
                return id;
            }
    return "";
}

std::string task_one_liner(const Task& t) {
    std::string out = t.description + " (";
    if (t.id) out += "^" + *t.id + ", ";
    if (t.assignee) out += "@" + *t.assignee + ", ";
    std::string st = taskml::to_string(t.status);
    std::replace(st.begin(), st.end(), '_', ' ');
    return out + st + ")";
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

Plan Plan::load(const fs::path& path) {
    return from_text(read_file(path), taskml::dialect_for_path(path.string()), path);
}

Plan Plan::from_text(std::string text, taskml::Dialect dialect, fs::path path) {
    Plan p;
    p.path_ = std::move(path);
    p.text_ = std::move(text);
    p.dialect_ = dialect;
    taskml::ParseOptions opts;
    opts.dialect = dialect;
    p.result_ = taskml::parse(p.text_, opts);
    p.errors_ = p.result_.errors;
    for (auto& d : taskml::validate(p.result_.document))
        if (d.severity == taskml::Severity::Error) p.errors_.push_back(std::move(d));
    p.index();
    return p;
}

void Plan::index() {
    tasks_.clear();
    std::vector<const Task*> parents;
    std::vector<std::string> sections;
    auto walk = [&](auto& self, const std::vector<Task>& list) -> void {
        for (const Task& t : list) {
            TaskRef r;
            r.task = &t;
            r.parents = parents;
            r.sections = sections;
            r.depth = static_cast<int>(parents.size());
            tasks_.push_back(std::move(r));
            parents.push_back(&t);
            self(self, t.subtasks);
            parents.pop_back();
        }
    };
    walk(walk, doc().tasks);
    // Sections are flat in the model; rebuild the heading path from levels.
    std::vector<std::pair<int, std::string>> stack;
    for (const auto& s : doc().sections) {
        while (!stack.empty() && stack.back().first >= s.level) stack.pop_back();
        stack.emplace_back(s.level, s.title);
        sections.clear();
        for (const auto& e : stack) sections.push_back(e.second);
        walk(walk, s.tasks);
    }
}

const TaskRef* Plan::find(std::string_view key) const {
    key = trim(key);
    if (key.empty()) return nullptr;
    if (key[0] == '^') return by_id(key.substr(1));
    std::string_view num = key;
    if (num[0] == 'L' || num[0] == 'l') num.remove_prefix(1);
    if (!num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        int line = std::stoi(std::string(num));
        for (const TaskRef& r : tasks_)
            if (r.task->line == line) return &r;
        if (num.size() == key.size() || key[0] == 'L' || key[0] == 'l') return nullptr;
    }
    return by_id(key);
}

const TaskRef* Plan::by_id(std::string_view id) const {
    for (const TaskRef& r : tasks_)
        if (r.task->id && *r.task->id == id) return &r;
    return nullptr;
}

std::string normalized_title(std::string_view title) {
    std::string out;
    bool gap = false;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) {
            if (gap && !out.empty()) out += ' ';
            gap = false;
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            gap = true;
        }
    }
    return out;
}

namespace {
std::string twin_key(std::string_view title, std::string_view assignee) {
    return normalized_title(title) + "\n" + normalize_handle(assignee);
}
}  // namespace

const TaskRef* Plan::open_twin(std::string_view title, std::string_view assignee, const Task* except) const {
    const std::string key = twin_key(title, assignee);
    if (normalized_title(title).empty()) return nullptr;
    for (const TaskRef& r : tasks_)
        if (r.task != except && !is_finished(r.task->status) &&
            twin_key(r.task->description, r.task->assignee.value_or("")) == key)
            return &r;
    return nullptr;
}

const TaskRef* Plan::ref_of(const Task* t) const {
    for (const TaskRef& r : tasks_)
        if (r.task == t) return &r;
    return nullptr;
}

std::vector<const TaskRef*> Plan::dependents(const TaskRef& t) const {
    std::vector<const TaskRef*> out;
    if (!t.task->id) return out;
    const std::string& id = *t.task->id;
    for (const TaskRef& r : tasks_) {
        const auto& d = r.task->depends_on;
        const auto& b = r.task->blocked_by;
        if (std::find(d.begin(), d.end(), id) != d.end() || std::find(b.begin(), b.end(), id) != b.end())
            out.push_back(&r);
    }
    return out;
}

std::string follow_duplicates(const Plan& plan, const std::string& id) {
    std::set<std::string> seen{id};
    std::string at = id;
    for (int i = 0; i < 16; ++i) {
        const TaskRef* r = plan.by_id(at);
        if (!r) return "";
        const std::string next = duplicate_of(*r->task);
        if (next.empty()) return at == id ? "" : at;
        if (!seen.insert(next).second || !plan.by_id(next)) return "";
        at = next;
    }
    return "";
}

static std::vector<std::string> unfinished(const Plan& p, const std::vector<std::string>& ids, bool review_counts) {
    std::vector<std::string> out;
    for (const std::string& id : ids) {
        const TaskRef* r = p.by_id(id);
        // Closed as a duplicate: the work goes on in its twin (none: a circle of copies).
        if (r && !duplicate_of(*r->task).empty()) {
            const std::string twin = follow_duplicates(p, id);
            r = twin.empty() ? nullptr : p.by_id(twin);
        }
        bool met = r && (is_finished(r->task->status) || (review_counts && r->task->status == TaskStatus::Review));
        if (!met) out.push_back(id);
    }
    return out;
}

// A dependency in review is met: its work is done, and the work that
// depends on it (tests, docs) is often what reviews it. A blocker is not:
// a question waiting for the customer is [?] too.
std::vector<std::string> Plan::unmet_dependencies(const TaskRef& t) const {
    return unfinished(*this, t.task->depends_on, true);
}
std::vector<std::string> Plan::active_blockers(const TaskRef& t) const { return unfinished(*this, t.task->blocked_by, false); }

std::vector<std::string> inherited_tags(const taskml::Task& t) {
    std::vector<std::string> out;
    for (const auto& tag : t.tags)
        if (starts_with(tag, "sprint-")) out.push_back(tag);
    return out;
}

std::string Plan::directive(std::string_view name, std::string fallback) const {
    return doc().directives.value_or(name, std::move(fallback));
}

// ---------------------------------------------------------------------------
// PlanEditor
// ---------------------------------------------------------------------------

namespace {

int indent_of(std::string_view l) {
    int n = 0;
    for (char c : l) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

bool blank(std::string_view l) { return trim(l).empty(); }

// Byte offset of the content after indentation and (Markdown) list marker.
std::size_t item_start(std::string_view l, taskml::Dialect d) {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    if (d == taskml::Dialect::Markdown && i < l.size()) {
        std::size_t j = i;
        if (l[j] == '-' || l[j] == '*' || l[j] == '+') {
            ++j;
        } else {
            while (j < l.size() && std::isdigit(static_cast<unsigned char>(l[j]))) ++j;
            if (j > i && j < l.size() && (l[j] == '.' || l[j] == ')')) ++j;
            else j = i;
        }
        if (j > i) {
            while (j < l.size() && l[j] == ' ') ++j;
            i = j;
        }
    }
    return i;
}

// Escape inline-token sigils at word starts, so text stays text.
std::string escape_text(std::string_view s) {
    std::string out;
    bool word_start = true;
    for (char c : s) {
        if (c == '\n' || c == '\r') c = ' ';
        if (word_start && (c == '#' || c == '@' || c == '~' || c == '!' || c == '^' || c == '[')) out += '\\';
        out += c;
        word_start = c == ' ';
    }
    return std::string(trim(out));
}

// Notes are plain text: only a start that would read as something else
// (a task, a criterion, a dependency) needs an escape.
std::string escape_note(std::string_view s) {
    std::string out;
    for (char c : s) out += (c == '\n' || c == '\r') ? ' ' : c;
    std::string_view t = trim(out);
    std::string r(t);
    bool crit_like = r.size() >= 3 && r[0] == '(' && r[2] == ')' && (r[1] == ' ' || r[1] == 'x' || r[1] == 'X' || r[1] == '!');
    if (!r.empty() && (r[0] == '[' || crit_like || starts_with(r, "\u2713") || starts_with(r, "\u2717") ||
                       starts_with(r, "\u25cb") || starts_with(r, "\u2714") || starts_with(r, "\u2718")))
        r.insert(0, "\\");
    else if (starts_with(r, "depends:") || starts_with(r, "depends-on:") || starts_with(r, "blocked-by:") ||
             starts_with(r, "evidence:"))
        r.insert(0, "Note: ");
    return r;
}

}  // namespace

PlanEditor::PlanEditor(const Plan& plan) : plan_(plan) {
    lines_ = split_lines(plan.text());
    trailing_newline_ = plan.text().empty() || plan.text().back() == '\n';
    index_.resize(lines_.size() + 2);
    for (std::size_t i = 0; i < index_.size(); ++i) index_[i] = static_cast<int>(i) - 1;
}

int PlanEditor::current_index(int line) const {
    if (line < 1 || line >= static_cast<int>(index_.size())) throw std::out_of_range("no line " + std::to_string(line));
    return index_[static_cast<std::size_t>(line)];
}

int PlanEditor::line_indent(int index) const { return indent_of(lines_[static_cast<std::size_t>(index)]); }

int PlanEditor::subtree_end(int index) const {
    const int base = line_indent(index);
    int last = index;
    for (int i = index + 1; i < static_cast<int>(lines_.size()); ++i) {
        const std::string& l = lines_[static_cast<std::size_t>(i)];
        if (blank(l)) continue;
        if (indent_of(l) <= base) break;
        last = i;
    }
    return last;
}

std::string PlanEditor::marker_of(int index) const {
    if (plan_.dialect() != taskml::Dialect::Markdown) return "";
    std::string_view l = lines_[static_cast<std::size_t>(index)];
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    if (i < l.size() && (l[i] == '-' || l[i] == '*' || l[i] == '+')) return std::string(1, l[i]) + " ";
    return "- ";
}

void PlanEditor::insert(int at, const std::vector<std::string>& new_lines) {
    lines_.insert(lines_.begin() + at, new_lines.begin(), new_lines.end());
    const int n = static_cast<int>(new_lines.size());
    for (int& idx : index_)
        if (idx >= at) idx += n;
    changed_ = true;
}

void PlanEditor::set_status(const TaskRef& t, TaskStatus status) {
    int idx = current_index(t.task->line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    std::size_t i = item_start(l, plan_.dialect());
    if (i + 2 >= l.size() || l[i] != '[' || l[i + 2] != ']')
        throw std::runtime_error("line " + std::to_string(t.task->line) + " is not a task line");
    char m = taskml::status_marker(status);
    if (l[i + 1] != m) {
        l[i + 1] = m;
        changed_ = true;
    }
}

void PlanEditor::set_assignee(const TaskRef& t, std::string_view handle) {
    int idx = current_index(t.task->line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    const std::string token = "@" + normalize_handle(handle);
    std::size_t start = item_start(l, plan_.dialect()) + 3;
    // Replace the first "@name" token that starts a word.
    for (std::size_t i = start; i < l.size(); ++i) {
        if (l[i] == '@' && l[i - 1] == ' ') {
            std::size_t end = i + 1;
            while (end < l.size() && l[end] != ' ') ++end;
            if (l.compare(i, end - i, token) != 0) {
                l.replace(i, end - i, token);
                changed_ = true;
            }
            return;
        }
    }
    // None: put it before a trailing ^id, or at the end.
    std::size_t caret = l.rfind(" ^");
    if (caret != std::string::npos && caret >= start && l.find(' ', caret + 1) == std::string::npos)
        l.insert(caret, " " + token);
    else
        l += " " + token;
    changed_ = true;
}

std::string PlanEditor::ensure_id(const TaskRef& t) {
    if (t.task->id) return *t.task->id;
    std::string id = unique_id(t.task->description);
    std::string& l = lines_[static_cast<std::size_t>(current_index(t.task->line))];
    while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
    l += " ^" + id;
    changed_ = true;
    return id;
}

void PlanEditor::add_note(const TaskRef& t, std::string_view text) {
    int idx = current_index(t.task->line);
    int end = subtree_end(idx);
    int indent = end > idx ? line_indent(idx + 1) : line_indent(idx) + (plan_.dialect() == taskml::Dialect::Markdown ? 2 : 4);
    std::string note = std::string(static_cast<std::size_t>(indent), ' ') + "- " + escape_note(text);
    // Notes go before the subtasks, right after the task line and its other notes.
    int at = idx + 1;
    while (at <= end) {
        std::string_view l = lines_[static_cast<std::size_t>(at)];
        if (blank(l)) {
            ++at;
            continue;
        }
        std::size_t s = item_start(l, plan_.dialect());
        bool child_task = s + 2 < l.size() && l[s] == '[' && l[s + 2] == ']';
        if (child_task || indent_of(l) < indent) break;
        ++at;
    }
    insert(at, {note});
}

void PlanEditor::add_link(const TaskRef& t, std::string_view key, std::string_view id) {
    // A link that closes a cycle (^id already waits, through its links, for this task) is not
    // made; a note says so (rather than refusing the whole edit for one such link).
    if (t.task->id && !id.empty()) {
        Plan now = Plan::from_text(text(), plan_.dialect());
        const std::string self = *t.task->id;
        std::vector<std::string> todo{std::string(id)};
        std::set<std::string> seen;
        bool cycle = false;
        // A direct reversal (^id waits for this task itself) its caller undoes right after (a
        // blocker on one's own handoff); only a longer cycle is refused here.
        if (const TaskRef* r = now.by_id(std::string(id))) {
            const auto& a = r->task->depends_on;
            const auto& b = r->task->blocked_by;
            if (std::find(a.begin(), a.end(), self) != a.end() || std::find(b.begin(), b.end(), self) != b.end())
                todo.clear();
        }
        while (!todo.empty() && !cycle) {
            const std::string at = todo.back();
            todo.pop_back();
            if (at == self) {
                cycle = true;
                break;
            }
            if (!seen.insert(at).second) continue;
            if (const TaskRef* r = now.by_id(at)) {
                for (const auto& d : r->task->depends_on) todo.push_back(d);
                for (const auto& d : r->task->blocked_by) todo.push_back(d);
            }
        }
        if (cycle) {
            add_note(t, "Not made to wait for ^" + std::string(id) + ": ^" + std::string(id) +
                            " already waits for this task, so the link would make a cycle");
            return;
        }
    }
    int idx = current_index(t.task->line);
    int end = subtree_end(idx);
    const std::string ref = "^" + std::string(id);
    // An existing key: line (blocked-by:, depends:) among the task's own children.
    const std::string head = std::string(key) + ":";
    int child_indent = -1;
    for (int i = idx + 1; i <= end; ++i) {
        std::string& l = lines_[static_cast<std::size_t>(i)];
        if (blank(l)) continue;
        if (child_indent < 0) child_indent = indent_of(l);
        if (indent_of(l) != child_indent) continue;
        std::string_view c = std::string_view(l).substr(item_start(l, plan_.dialect()));
        if (starts_with(c, head)) {
            if (l.find(ref) == std::string::npos) {
                while (!l.empty() && l.back() == ' ') l.pop_back();
                l += ", " + ref;
                changed_ = true;
            }
            return;
        }
    }
    const bool md = plan_.dialect() == taskml::Dialect::Markdown;
    if (child_indent < 0) child_indent = line_indent(idx) + (md ? static_cast<int>(marker_of(idx).size()) : 4);
    insert(idx + 1, {std::string(static_cast<std::size_t>(child_indent), ' ') + (md ? "- " : "") + head + " " + ref});
}

void PlanEditor::add_blocked_by(const TaskRef& t, std::string_view id) { add_link(t, "blocked-by", id); }

void PlanEditor::add_depends(const TaskRef& t, std::string_view id) { add_link(t, "depends", id); }

bool PlanEditor::remove_blocked_by(const TaskRef& t, std::string_view id) {
    return remove_link(t, "blocked-by", id, "No longer blocked by ^");
}

bool PlanEditor::remove_depends(const TaskRef& t, std::string_view id) {
    return remove_link(t, "depends", id, "No longer waits for ^");
}

bool PlanEditor::remove_link(const TaskRef& t, std::string_view key, std::string_view id, std::string_view gone) {
    const std::string head_key = std::string(key) + ":";
    int idx = current_index(t.task->line);
    int end = subtree_end(idx);
    int child_indent = -1;
    for (int i = idx + 1; i <= end; ++i) {
        std::string& l = lines_[static_cast<std::size_t>(i)];
        if (blank(l)) continue;
        if (child_indent < 0) child_indent = indent_of(l);
        if (indent_of(l) != child_indent) continue;
        const std::size_t start = item_start(l, plan_.dialect());
        std::string_view c = std::string_view(l).substr(start);
        if (!starts_with(c, head_key)) continue;
        std::vector<std::string> keep;
        bool found = false;
        std::string list(c.substr(head_key.size()));
        for (char& ch : list)
            if (ch == ',') ch = ' ';
        std::size_t p = 0;
        while (p < list.size()) {
            while (p < list.size() && list[p] == ' ') ++p;
            std::size_t e = p;
            while (e < list.size() && list[e] != ' ') ++e;
            if (e > p) {
                std::string ref = list.substr(p, e - p);
                std::string bare = ref[0] == '^' ? ref.substr(1) : ref;
                if (bare == id) found = true;
                else keep.push_back("^" + bare);
            }
            p = e;
        }
        if (!found) return false;
        std::string head = l.substr(0, start);
        if (keep.empty()) {
            l = head + std::string(gone) + std::string(id);
        } else {
            std::string joined;
            for (const auto& k : keep) joined += (joined.empty() ? "" : ", ") + k;
            l = head + head_key + " " + joined;
        }
        changed_ = true;
        return true;
    }
    return false;
}

void PlanEditor::reword_criterion(const TaskRef& t, std::size_t index, std::string_view text) {
    if (index >= t.task->criteria.size()) throw std::runtime_error("no criterion " + std::to_string(index + 1));
    int idx = current_index(t.task->criteria[index].line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    std::size_t s = item_start(l, plan_.dialect());
    std::size_t e = l.compare(s, 3, "( )") == 0 ? s + 3 : l.find(' ', s);
    if (e == std::string::npos) e = l.size();
    std::string one(trim(text));
    for (char& ch : one)
        if (ch == '\n') ch = ' ';
    const std::string now = l.substr(0, e) + " " + one;
    if (now != l) {
        l = now;
        changed_ = true;
    }
}

void PlanEditor::remove_criterion(const TaskRef& t, std::size_t index, std::string_view why) {
    if (index >= t.task->criteria.size()) throw std::runtime_error("no criterion " + std::to_string(index + 1));
    const taskml::Criterion& c = t.task->criteria[index];
    int idx = current_index(c.line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    const std::string pad = l.substr(0, item_start(l, plan_.dialect()));
    std::string note = "Removed criterion: " + c.description;
    if (!trim(why).empty()) note += " (" + std::string(trim(why)) + ")";
    for (char& ch : note)
        if (ch == '\n') ch = ' ';
    l = pad + note;
    changed_ = true;
    // Its evidence, a line below it, becomes a note beside it.
    if (idx + 1 < static_cast<int>(lines_.size())) {
        std::string& next = lines_[static_cast<std::size_t>(idx + 1)];
        std::size_t ns = item_start(next, plan_.dialect());
        if (!blank(next) && indent_of(next) > indent_of(l) &&
            starts_with(std::string_view(next).substr(ns), "evidence:"))
            next = pad + "Its evidence: " + std::string(trim(std::string_view(next).substr(ns + 9)));
    }
}

void PlanEditor::set_criterion(const TaskRef& t, std::size_t index, taskml::CriterionStatus status,
                               std::optional<std::string> evidence) {
    if (index >= t.task->criteria.size()) throw std::runtime_error("no criterion " + std::to_string(index + 1));
    const taskml::Criterion& c = t.task->criteria[index];
    const bool md = plan_.dialect() == taskml::Dialect::Markdown;
    int idx = current_index(c.line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    std::size_t s = item_start(l, plan_.dialect());
    std::size_t e = l.find(' ', s);
    if (e == std::string::npos) e = l.size();
    // "( )" has a space inside.
    if (l.compare(s, 3, "( )") == 0) e = s + 3;
    const std::string marker = md ? taskml::criterion_marker_ascii(status) : taskml::criterion_marker(status);
    if (l.compare(s, e - s, marker) != 0) {
        l.replace(s, e - s, marker);
        changed_ = true;
    }
    if (!evidence) return;
    std::string ev = *evidence;
    for (char& ch : ev)
        if (ch == '"' || ch == '\n') ch = ch == '"' ? '\'' : ' ';
    const std::string pad(static_cast<std::size_t>(indent_of(l) + (md ? static_cast<int>(marker_of(idx).size()) : 4)), ' ');
    const std::string text = pad + (md ? "- " : "") + "evidence: \"" + ev + "\"";
    if (idx + 1 < static_cast<int>(lines_.size())) {
        std::string& next = lines_[static_cast<std::size_t>(idx + 1)];
        if (!blank(next) && indent_of(next) > indent_of(l) &&
            starts_with(std::string_view(next).substr(item_start(next, plan_.dialect())), "evidence:")) {
            if (next != text) {
                next = text;
                changed_ = true;
            }
            return;
        }
    }
    insert(idx + 1, {text});
}

void PlanEditor::add_criterion(const TaskRef& t, std::string_view text) {
    int idx = current_index(t.task->line);
    int end = subtree_end(idx);
    const bool md = plan_.dialect() == taskml::Dialect::Markdown;
    int indent = line_indent(idx) + (md ? static_cast<int>(marker_of(idx).size()) : 4);
    std::string line = std::string(static_cast<std::size_t>(indent), ' ') + (md ? "- ( ) " : "○ ") + escape_note(text);
    insert(end + 1, {line});
}

void PlanEditor::set_estimate(const TaskRef& t, std::string_view estimate) {
    std::string e = normalize_estimate(estimate);
    if (e.empty()) return;
    int idx = current_index(t.task->line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    const std::string token = "~" + e;
    std::size_t start = item_start(l, plan_.dialect()) + 3;
    for (std::size_t i = start; i + 1 < l.size(); ++i) {
        if (l[i] == '~' && l[i - 1] == ' ' && std::isdigit(static_cast<unsigned char>(l[i + 1]))) {
            std::size_t end = l.find(' ', i);
            if (end == std::string::npos) end = l.size();
            if (l.compare(i, end - i, token) != 0) {
                l.replace(i, end - i, token);
                changed_ = true;
            }
            return;
        }
    }
    std::size_t at = std::string::npos;
    for (std::size_t i = start; i < l.size(); ++i)
        if ((l[i] == '@' || l[i] == '^') && l[i - 1] == ' ') {
            at = i - 1;
            break;
        }
    if (at == std::string::npos) l += " " + token;
    else l.insert(at, " " + token);
    changed_ = true;
}

void PlanEditor::add_tag(const TaskRef& t, std::string_view tag) {
    if (std::find(t.task->tags.begin(), t.task->tags.end(), tag) != t.task->tags.end()) return;
    int idx = current_index(t.task->line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    const std::string token = "#" + std::string(tag);
    std::size_t start = item_start(l, plan_.dialect()) + 3;
    for (std::size_t p = l.find(token, start); p != std::string::npos; p = l.find(token, p + 1))
        if (l[p - 1] == ' ' && (p + token.size() == l.size() || l[p + token.size()] == ' ')) return;  // added in this edit
    std::size_t at = std::string::npos;
    for (std::size_t i = start; i < l.size(); ++i)
        if ((l[i] == '@' || l[i] == '^') && l[i - 1] == ' ') {
            at = i - 1;
            break;
        }
    if (at == std::string::npos) l += " " + token;
    else l.insert(at, " " + token);
    changed_ = true;
}

void PlanEditor::set_directive(std::string_view key, std::string_view value) {
    const std::string line = std::string(key) + ": " + std::string(value);
    if (!lines_.empty() && trim(lines_[0]) == "---") {
        for (int i = 1; i < static_cast<int>(lines_.size()); ++i) {
            std::string& l = lines_[static_cast<std::size_t>(i)];
            if (trim(l) == "---") {
                insert(i, {line});
                return;
            }
            if (starts_with(l, std::string(key) + ":")) {
                if (l != line) {
                    l = line;
                    changed_ = true;
                }
                return;
            }
        }
    }
    insert(0, {"---", line, "---", ""});
}

void PlanEditor::set_priority(const TaskRef& t, int priority) {
    int idx = current_index(t.task->line);
    std::string& l = lines_[static_cast<std::size_t>(idx)];
    const std::string token = "#p" + std::to_string(priority);
    std::size_t start = item_start(l, plan_.dialect()) + 3;
    for (std::size_t i = start; i + 2 < l.size(); ++i) {
        if (l[i] == '#' && l[i - 1] == ' ' && l[i + 1] == 'p' && std::isdigit(static_cast<unsigned char>(l[i + 2]))) {
            std::size_t end = i + 2;
            while (end < l.size() && std::isdigit(static_cast<unsigned char>(l[end]))) ++end;
            if (end == l.size() || l[end] == ' ') {
                if (l.compare(i, end - i, token) != 0) {
                    l.replace(i, end - i, token);
                    changed_ = true;
                }
                return;
            }
        }
    }
    // None: before the first @assignee or a trailing ^id, else at the end.
    std::size_t at = std::string::npos;
    for (std::size_t i = start; i < l.size(); ++i)
        if ((l[i] == '@' || l[i] == '^') && l[i - 1] == ' ') {
            at = i - 1;
            break;
        }
    if (at == std::string::npos) l += " " + token;
    else l.insert(at, " " + token);
    changed_ = true;
}

std::string PlanEditor::unique_id(std::string_view base) {
    std::string root = slug(base, 24);
    std::string id = root;
    for (int n = 2;; ++n) {
        bool taken = plan_.by_id(id) || std::find(new_ids_.begin(), new_ids_.end(), id) != new_ids_.end();
        if (!taken) break;
        id = root + "-" + std::to_string(n);
    }
    new_ids_.push_back(id);
    return id;
}

// A given id that is taken: the next free "<id>-N" (rather than refusing the whole edit; this
// is the one place every new task passes).
std::string PlanEditor::free_id(const std::string& id) {
    auto taken = [&](const std::string& x) {
        return plan_.by_id(x) || std::find(new_ids_.begin(), new_ids_.end(), x) != new_ids_.end();
    };
    std::string out = id;
    for (int n = 2; taken(out); ++n) out = id + "-" + std::to_string(n);
    new_ids_.push_back(out);
    return out;
}

std::vector<std::string> PlanEditor::render(const NewTask& t, int indent, const std::string& marker) {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::string line = pad + marker + "[" + taskml::status_marker(t.status) + "] " + escape_text(t.description);
    if (t.priority) line += " #p" + std::to_string(*t.priority);
    if (t.estimate && !normalize_estimate(*t.estimate).empty()) line += " ~" + normalize_estimate(*t.estimate);
    if (!t.assignee.empty()) line += " @" + normalize_handle(t.assignee);
    for (const std::string& tag : t.tags) line += " #" + tag;
    line += " ^" + t.id;
    std::vector<std::string> out{line};
    const bool md = plan_.dialect() == taskml::Dialect::Markdown;
    const std::string child = pad + std::string(md ? marker.size() : 4, ' ');
    const std::string item = md ? "- " : "";
    if (!t.depends_on.empty()) {
        std::string deps;
        for (const std::string& d : t.depends_on) deps += (deps.empty() ? "^" : ", ^") + d;
        out.push_back(child + item + "depends: " + deps);
    }
    for (const std::string& n : t.notes) out.push_back(child + "- " + escape_note(n));
    for (const std::string& c : t.criteria) out.push_back(child + item + "( ) " + std::string(trim(c)));
    return out;
}

std::optional<std::string> PlanEditor::merge_into_twin(NewTask& task) {
    if (is_finished(task.status) || normalized_title(task.description).empty()) return std::nullopt;
    const std::string key = twin_key(task.description, task.assignee);
    std::string into;
    if (auto it = added_keys_.find(key); it != added_keys_.end()) {
        into = it->second;
    } else if (const TaskRef* twin = plan_.open_twin(task.description, task.assignee)) {
        into = ensure_id(*twin);
        // Where the second filing came from (its last note says who and from where).
        std::string from = task.notes.empty() ? std::string() : first_line(task.notes.back(), 200);
        add_note(*twin, "Filed again " + iso_timestamp().substr(0, 10) + " (the twin check: same title and assignee)" +
                            (from.empty() ? "" : ": " + from));
    } else {
        if (task.id.empty()) task.id = unique_id(task.description);
        else task.id = free_id(task.id);
        added_keys_[key] = task.id;
        return std::nullopt;
    }
    twin_merges_.push_back({task.description, into});
    return into;
}

std::string PlanEditor::add_at(const TaskRef& anchor, NewTask task, bool as_child) {
    if (auto twin = merge_into_twin(task)) return *twin;
    if (task.id.empty()) task.id = unique_id(task.description);
    else if (std::find(new_ids_.begin(), new_ids_.end(), task.id) == new_ids_.end()) task.id = free_id(task.id);
    int idx = current_index(anchor.task->line);
    int end = subtree_end(idx);
    int indent = line_indent(idx);
    if (as_child) {
        // Children line up with the anchor's existing child items.
        int child_indent = -1;
        for (int i = idx + 1; i <= end; ++i)
            if (!blank(lines_[static_cast<std::size_t>(i)])) {
                child_indent = line_indent(i);
                break;
            }
        indent = child_indent >= 0 ? child_indent
                                   : indent + (plan_.dialect() == taskml::Dialect::Markdown
                                                   ? static_cast<int>(marker_of(idx).size())
                                                   : 4);
    }
    std::vector<std::string> lines = render(task, indent, marker_of(idx));
    if (as_child) {
        insert(end + 1, lines);
    } else {
        // Siblings added after the same task keep the order they were added in.
        int& added = after_[anchor.task->line];
        insert(end + 1 + added, lines);
        added += static_cast<int>(lines.size());
    }
    return task.id;
}

std::string PlanEditor::add_to_section(std::string_view title, NewTask task, int level) {
    if (auto twin = merge_into_twin(task)) return *twin;
    if (task.id.empty()) task.id = unique_id(task.description);
    else if (std::find(new_ids_.begin(), new_ids_.end(), task.id) == new_ids_.end()) task.id = free_id(task.id);
    const bool md = plan_.dialect() == taskml::Dialect::Markdown;
    // (level, title) of a heading line.
    auto heading_title = [&](std::string_view l) -> std::optional<std::pair<int, std::string>> {
        const char c = md ? '#' : '=';
        std::size_t n = 0;
        while (n < l.size() && l[n] == c) ++n;
        if (n == 0 || (md && n > 6) || (!md && n < 2) || (n < l.size() && l[n] != ' ')) return std::nullopt;
        std::string t(trim(l.substr(n)));
        while (!t.empty() && t.back() == c) t.pop_back();
        return std::make_pair(static_cast<int>(n), std::string(trim(t)));
    };
    // Skip front matter when looking for headings.
    int start = 0;
    if (md && !lines_.empty() && trim(lines_[0]) == "---") {
        for (int i = 1; i < static_cast<int>(lines_.size()); ++i)
            if (trim(lines_[static_cast<std::size_t>(i)]) == "---") {
                start = i + 1;
                break;
            }
    }
    // The heading with this title, at `level` if there is one.
    int heading = -1;
    for (int i = start; i < static_cast<int>(lines_.size()); ++i) {
        auto t = heading_title(lines_[static_cast<std::size_t>(i)]);
        if (!t || lower(t->second) != lower(trim(title))) continue;
        if (t->first == level || heading < 0) heading = i;
        if (t->first == level) break;
    }
    std::vector<std::string> rendered = render(task, 0, md ? "- " : "");
    if (heading < 0) {
        std::vector<std::string> add;
        if (!lines_.empty() && !blank(lines_.back())) add.push_back("");
        add.push_back(md ? std::string(static_cast<std::size_t>(std::clamp(level, 1, 6)), '#') + " " + std::string(title)
                         : "== " + std::string(title) + " ==");
        add.push_back("");
        add.insert(add.end(), rendered.begin(), rendered.end());
        insert(static_cast<int>(lines_.size()), add);
        return task.id;
    }
    int end = static_cast<int>(lines_.size());
    for (int i = heading + 1; i < static_cast<int>(lines_.size()); ++i)
        if (heading_title(lines_[static_cast<std::size_t>(i)])) {
            end = i;
            break;
        }
    // After the last list item of the section; keep any prose after it.
    int last_item = -1;
    for (int i = heading + 1; i < end; ++i) {
        std::string_view l = lines_[static_cast<std::size_t>(i)];
        if (!blank(l) && (indent_of(l) > 0 || item_start(l, plan_.dialect()) > 0 || starts_with(l, "[")))
            last_item = i;
    }
    if (last_item < 0) {
        std::vector<std::string> add{""};
        add.insert(add.end(), rendered.begin(), rendered.end());
        if (heading + 1 < static_cast<int>(lines_.size()) && !blank(lines_[static_cast<std::size_t>(heading + 1)]))
            add.push_back("");
        insert(heading + 1, add);
    } else {
        insert(last_item + 1, rendered);
    }
    return task.id;
}

std::string PlanEditor::add_subtask(const TaskRef& parent, NewTask task) { return add_at(parent, std::move(task), true); }
std::string PlanEditor::add_sibling(const TaskRef& sibling, NewTask task) { return add_at(sibling, std::move(task), false); }

std::string PlanEditor::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i];
        if (i + 1 < lines_.size() || trailing_newline_) out += '\n';
    }
    return out;
}

CommitResult PlanEditor::check() const {
    CommitResult r;
    Plan edited = Plan::from_text(text(), plan_.dialect());
    if (edited.errors().size() > plan_.errors().size()) {
        r.error = "the edit would break the plan";
        r.diagnostics = edited.errors();
        return r;
    }
    r.ok = true;
    return r;
}

CommitResult PlanEditor::commit() {
    CommitResult r = check();
    if (!r.ok || !changed_ || plan_.path().empty()) return r;
    auto on_disk = try_read_file(plan_.path());
    if (!on_disk || *on_disk != plan_.text()) {
        r.ok = false;
        r.error = "the plan changed on disk since it was read";
        return r;
    }
    write_file_atomic(plan_.path(), text());
    return r;
}

// ---------------------------------------------------------------------------
// The twin cleanup
// ---------------------------------------------------------------------------

std::vector<TwinCleanup> merge_open_twins(const Plan& plan, PlanEditor& ed) {
    std::map<std::string, std::vector<const TaskRef*>> groups;
    std::vector<std::string> order;
    for (const TaskRef& r : plan.tasks()) {
        if (is_finished(r.task->status) || normalized_title(r.task->description).empty()) continue;
        const std::string key = twin_key(r.task->description, r.task->assignee.value_or(""));
        if (groups[key].empty()) order.push_back(key);
        groups[key].push_back(&r);
    }
    std::vector<TwinCleanup> out;
    const std::string date = iso_timestamp().substr(0, 10);
    for (const std::string& key : order) {
        auto& g = groups[key];
        if (g.size() < 2) continue;
        // The one with work in it stays: in progress or in review first, then plan order.
        auto rank = [](const TaskRef* r) {
            const TaskStatus s = r->task->status;
            return s == TaskStatus::InProgress || s == TaskStatus::Review ? 0 : 1;
        };
        std::stable_sort(g.begin(), g.end(), [&](const TaskRef* a, const TaskRef* b) { return rank(a) < rank(b); });
        const std::string keep = ed.ensure_id(*g[0]);
        for (std::size_t i = 1; i < g.size(); ++i) {
            const std::string old = ed.ensure_id(*g[i]);
            // The kept one waiting for its twin would wait for itself.
            const Task& k = *g[0]->task;
            if (std::find(k.depends_on.begin(), k.depends_on.end(), old) != k.depends_on.end()) ed.remove_depends(*g[0], old);
            if (std::find(k.blocked_by.begin(), k.blocked_by.end(), old) != k.blocked_by.end()) ed.remove_blocked_by(*g[0], old);
            ed.set_status(*g[i], TaskStatus::Cancelled);
            ed.add_note(*g[i], "Duplicate of ^" + keep + " (the twin cleanup, " + date + ")");
            ed.add_note(*g[0], "Twin ^" + old + " merged in (the twin cleanup, " + date + ")");
            out.push_back({old, keep, g[i]->task->description});
        }
    }
    return out;
}

std::string twin_cleanup_line(const TwinCleanup& m) { return "^" + m.from + " -> ^" + m.into + "  " + m.title; }

std::string twin_cleanup_entry(const std::vector<TwinCleanup>& merges) {
    if (merges.empty()) return "";
    std::string e = "## " + iso_timestamp() + " · Twin tasks merged (same title and assignee)\n\n";
    for (const auto& m : merges) e += "- " + twin_cleanup_line(m) + "\n";
    return e;
}

}  // namespace myteam
