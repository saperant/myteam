// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// Writers: TaskML text and the JSON interchange format; JSON reader.

#include <cmath>
#include <string>

#include "taskml/taskml.hpp"
#include "text_util.hpp"

namespace taskml {

namespace {

using namespace detail;

// ---------------------------------------------------------------------------
// TaskML writer
// ---------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(const SerializeOptions& o) : opts_(o) {}

    std::string write(const Document& doc) {
        if (opts_.dialect == Dialect::Markdown) return write_markdown(doc);
        if (opts_.include_comments) {
            for (const auto& c : doc.comments) line(0, "// " + c.text);
            if (!doc.comments.empty()) blank();
        }

        for (const auto& [name, value] : doc.directives) {
            line(0, value.empty() ? "@" + name : "@" + name + " " + value);
        }
        if (!doc.directives.empty()) blank();

        for (const auto& inc : doc.includes) line(0, "< " + inc.path);
        if (!doc.includes.empty()) blank();

        write_tasks(doc.tasks, 0);
        for (const auto& s : doc.sections) {
            blank();
            std::string eq(static_cast<std::size_t>(s.level > 0 ? s.level : 2), '=');
            line(0, eq + " " + s.title + " " + eq);
            write_tasks(s.tasks, 0);
        }

        if (doc.view) {
            blank();
            std::string v = "---view:" + doc.view->type;
            for (const auto& [k, val] : doc.view->options) v += " " + k + "=" + val;
            line(0, v);
        }

        if (doc.agent_context) write_context(doc);

        if (doc.handoff) {
            const Handoff& h = *doc.handoff;
            blank();
            line(0, "---handoff");
            line(0, "from: " + h.from);
            line(0, "to: " + h.to);
            if (h.reason) line(0, "reason: \"" + *h.reason + "\"");
            if (h.context) {
                const Json* raw = raw_value(*h.context);
                line(0, "context: " + (raw ? raw->as_string() : h.context->dump()));
            }
            line(0, "---");
        }

        // Exactly one trailing newline, no leading blank lines.
        while (!out_.empty() && out_.back() == '\n') out_.pop_back();
        std::size_t lead = 0;
        while (lead < out_.size() && out_[lead] == '\n') ++lead;
        out_.erase(0, lead);
        if (!out_.empty()) out_ += '\n';
        return std::move(out_);
    }

private:
    // ----- Markdown -----

    std::string write_markdown(const Document& doc) {
        md_ = true;
        opts_.use_tabs = false;
        opts_.indent_size = 2;
        if (!doc.directives.empty()) {
            line(0, "---");
            for (const auto& [name, value] : doc.directives) line(0, name + ": " + yaml_scalar(value));
            line(0, "---");
            blank();
        }
        if (opts_.include_comments && !doc.comments.empty()) {
            for (const auto& c : doc.comments) line(0, "<!-- " + c.text + " -->");
            blank();
        }
        write_tasks(doc.tasks, 0);
        for (const auto& s : doc.sections) {
            blank();
            int level = s.level < 1 ? 2 : (s.level > 6 ? 6 : s.level);
            line(0, std::string(static_cast<std::size_t>(level), '#') + " " + escape(s.title, false));
            blank();
            write_tasks(s.tasks, 0);
        }
        return finish();
    }

    // Quote a front matter value when YAML would read it as something else.
    static std::string yaml_scalar(const std::string& v) {
        bool plain = !v.empty() && v.find(": ") == std::string::npos && v.find(" #") == std::string::npos &&
                     std::string("\"'[]{}&*!|>%@`#,?-").find(v[0]) == std::string::npos &&
                     v.back() != ' ' && v.back() != ':';
        if (plain) return v;
        std::string out = "\"";
        for (char c : v) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    }

    std::string finish() {
        while (!out_.empty() && out_.back() == '\n') out_.pop_back();
        std::size_t lead = 0;
        while (lead < out_.size() && out_[lead] == '\n') ++lead;
        out_.erase(0, lead);
        if (!out_.empty()) out_ += '\n';
        return std::move(out_);
    }

    // A note's text. In Markdown a note is a list item like any other, so a
    // start that would read as a task or a criterion is escaped.
    std::string note_text(const std::string& n) const {
        std::string e = escape(n, false);
        if (!md_) return e;
        bool task_like = e.size() >= 3 && e[0] == '[' && e[2] == ']';
        bool crit_like = e.size() >= 3 && e[0] == '(' && e[2] == ')' &&
                         (e[1] == ' ' || e[1] == 'x' || e[1] == 'X' || e[1] == '!');
        bool unicode_crit = starts_with(e, kCheck) || starts_with(e, kBallotX) || starts_with(e, kCircle) ||
                            starts_with(e, kHeavyCheck) || starts_with(e, kHeavyBallotX);
        return task_like || crit_like || unicode_crit ? "\\" + e : e;
    }

    // List items in Markdown, bare lines in TaskML.
    std::string item(std::string text) const { return md_ ? "- " + text : text; }

    // {"raw": "..."} placeholder produced for non-JSON content.
    static const Json* raw_value(const Json& j) {
        if (j.is_object() && j.as_object().size() == 1) {
            const Json* r = j.find("raw");
            if (r && r->is_string()) return r;
        }
        return nullptr;
    }

    void write_context(const Document& doc) {
        const AgentContext& ctx = *doc.agent_context;
        Json body;
        if (ctx.metadata) {
            body = *ctx.metadata;
        } else {
            // Built programmatically: emit fields that the directives don't cover.
            Json o = Json::object();
            const std::string* agent = doc.directives.get("agent");
            const std::string* agent_id = doc.directives.get("agent-id");
            if (ctx.session_id) o.set("session_id", *ctx.session_id);
            if (ctx.model && !(agent && *agent == *ctx.model)) o.set("model", *ctx.model);
            if (ctx.started_at) o.set("started_at", *ctx.started_at);
            if (ctx.agent_id && !(agent_id && *agent_id == *ctx.agent_id))
                o.set("agent_id", *ctx.agent_id);
            if (o.as_object().empty()) return;
            body = std::move(o);
        }
        blank();
        line(0, "---context");
        if (const Json* raw = raw_value(body)) {
            if (!raw->as_string().empty()) line(0, raw->as_string());
        } else {
            line(0, body.dump(2));
        }
        line(0, "---");
    }

    void write_tasks(const std::vector<Task>& tasks, int depth) {
        for (const auto& t : tasks) write_task(t, depth);
    }

    void write_task(const Task& t, int depth) {
        std::string s = "[";
        s += status_marker(t.status);
        s += "]";
        std::string desc = escape(t.description, /*tokens=*/true);
        if (!desc.empty()) s += " " + desc;
        if (t.priority) s += " #p" + std::to_string(*t.priority);
        if (t.estimate) s += " ~" + *t.estimate;
        if (t.assignee) s += " @" + *t.assignee;
        for (const auto& tag : t.tags) s += " #" + tag;
        if (t.due) s += " !" + *t.due;
        if (t.id) s += " ^" + *t.id;
        line(depth, item(s));

        const int child = depth + 1;
        if (!t.depends_on.empty()) line(child, item("depends: " + id_list(t.depends_on)));
        if (!t.blocked_by.empty()) line(child, item("blocked-by: " + id_list(t.blocked_by)));
        for (const auto& n : t.notes) line(child, n.empty() ? "-" : "- " + note_text(n));
        write_tasks(t.subtasks, child);
        for (const auto& c : t.criteria) {
            std::string cs = md_ ? criterion_marker_ascii(c.status) : criterion_marker(c.status);
            if (!c.description.empty()) cs += " " + escape(c.description, false);
            line(child, item(cs));
            if (c.evidence) line(child + 1, item("evidence: \"" + escape(*c.evidence, false) + "\""));
        }
    }

    static std::string id_list(const std::vector<std::string>& ids) {
        std::string s;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i) s += ", ";
            s += "^" + ids[i];
        }
        return s;
    }

    void line(int depth, const std::string& text) {
        if (opts_.use_tabs)
            out_.append(static_cast<std::size_t>(depth), '\t');
        else
            out_.append(static_cast<std::size_t>(depth * (opts_.indent_size > 0 ? opts_.indent_size : 4)), ' ');
        // Multi-line payloads (e.g. pretty JSON) are emitted as-is.
        out_ += text;
        out_ += '\n';
    }

    void blank() {
        if (!out_.empty() && !(out_.size() >= 2 && out_[out_.size() - 2] == '\n')) out_ += '\n';
    }

    SerializeOptions opts_;
    std::string out_;
    bool md_ = false;
};

// ---------------------------------------------------------------------------
// JSON interchange
// ---------------------------------------------------------------------------

Json strings_to_json(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

Json task_to_json(const Task& t) {
    Json o = Json::object();
    o.set("status", to_string(t.status));
    o.set("description", t.description);
    if (t.id) o.set("id", *t.id);
    if (t.priority) o.set("priority", *t.priority);
    if (t.estimate) o.set("estimate", *t.estimate);
    if (t.assignee) o.set("assignee", *t.assignee);
    if (!t.tags.empty()) o.set("tags", strings_to_json(t.tags));
    if (t.due) o.set("due", *t.due);
    if (!t.depends_on.empty()) o.set("dependsOn", strings_to_json(t.depends_on));
    if (!t.blocked_by.empty()) o.set("blockedBy", strings_to_json(t.blocked_by));
    if (!t.subtasks.empty()) {
        Json a = Json::array();
        for (const auto& s : t.subtasks) a.push_back(task_to_json(s));
        o.set("subtasks", std::move(a));
    }
    if (!t.criteria.empty()) {
        Json a = Json::array();
        for (const auto& c : t.criteria) {
            Json co = Json::object();
            co.set("description", c.description);
            co.set("status", to_string(c.status));
            if (c.evidence) co.set("evidence", *c.evidence);
            a.push_back(std::move(co));
        }
        o.set("criteria", std::move(a));
    }
    if (!t.notes.empty()) o.set("notes", strings_to_json(t.notes));
    return o;
}

Json tasks_to_json(const std::vector<Task>& tasks) {
    Json a = Json::array();
    for (const auto& t : tasks) a.push_back(task_to_json(t));
    return a;
}

// --- reading ---

[[noreturn]] void fail(ErrorCode code, const std::string& msg) {
    Diagnostic d;
    d.code = code;
    d.severity = Severity::Error;
    d.message = msg;
    throw Error(msg, {d});
}

const Json* field(const Json& obj, const char* key, Json::Type type, const std::string& where,
                  bool required = false) {
    const Json* v = obj.find(key);
    if (!v || v->is_null()) {
        if (required) fail(ErrorCode::MissingRequiredField, "Missing required field \"" + where + key + "\"");
        return nullptr;
    }
    if (v->type() != type) {
        static const char* names[] = {"null", "boolean", "number", "string", "array", "object"};
        fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + where + key + "\": expected " +
                                              names[static_cast<int>(type)]);
    }
    return v;
}

std::optional<std::string> opt_string(const Json& obj, const char* key, const std::string& where) {
    const Json* v = field(obj, key, Json::Type::String, where);
    if (!v || v->as_string().empty()) return std::nullopt;
    return v->as_string();
}

std::vector<std::string> string_list(const Json& obj, const char* key, const std::string& where) {
    std::vector<std::string> out;
    const Json* v = field(obj, key, Json::Type::Array, where);
    if (!v) return out;
    for (const auto& item : v->as_array()) {
        if (!item.is_string())
            fail(ErrorCode::InvalidFieldType,
                 "Invalid type for field \"" + where + key + "[]\": expected string");
        out.push_back(item.as_string());
    }
    return out;
}

Task task_from_json(const Json& j, const std::string& where, int depth) {
    if (depth > 1000) fail(ErrorCode::InvalidJson, "Task nesting too deep");
    if (!j.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + where + "\": expected object");
    const std::string w = where + ".";
    Task t;
    const std::string& status = field(j, "status", Json::Type::String, w, true)->as_string();
    auto st = task_status_from_string(status);
    if (!st)
        fail(ErrorCode::InvalidFieldType, "Invalid value for field \"" + w + "status\": \"" + status + "\"");
    t.status = *st;
    t.description = field(j, "description", Json::Type::String, w, true)->as_string();
    t.id = opt_string(j, "id", w);
    if (const Json* p = field(j, "priority", Json::Type::Number, w)) {
        double d = p->as_number();
        if (d != std::floor(d) || d < 0 || d > 3)
            fail(ErrorCode::InvalidFieldType, "Invalid value for field \"" + w + "priority\": expected 0-3");
        t.priority = static_cast<int>(d);
    }
    t.estimate = opt_string(j, "estimate", w);
    t.assignee = opt_string(j, "assignee", w);
    t.tags = string_list(j, "tags", w);
    t.due = opt_string(j, "due", w);
    t.depends_on = string_list(j, "dependsOn", w);
    t.blocked_by = string_list(j, "blockedBy", w);
    if (const Json* subs = field(j, "subtasks", Json::Type::Array, w)) {
        std::size_t i = 0;
        for (const auto& s : subs->as_array())
            t.subtasks.push_back(task_from_json(s, w + "subtasks[" + std::to_string(i++) + "]", depth + 1));
    }
    if (const Json* crits = field(j, "criteria", Json::Type::Array, w)) {
        std::size_t i = 0;
        for (const auto& c : crits->as_array()) {
            std::string cw = w + "criteria[" + std::to_string(i++) + "]";
            if (!c.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + cw + "\": expected object");
            Criterion cr;
            cr.description = field(c, "description", Json::Type::String, cw + ".", true)->as_string();
            const std::string& cs = field(c, "status", Json::Type::String, cw + ".", true)->as_string();
            auto cst = criterion_status_from_string(cs);
            if (!cst) fail(ErrorCode::InvalidFieldType, "Invalid value for field \"" + cw + ".status\": \"" + cs + "\"");
            cr.status = *cst;
            cr.evidence = opt_string(c, "evidence", cw + ".");
            t.criteria.push_back(std::move(cr));
        }
    }
    t.notes = string_list(j, "notes", w);
    return t;
}

std::vector<Task> tasks_from_json(const Json* arr, const std::string& where) {
    std::vector<Task> out;
    if (!arr) return out;
    std::size_t i = 0;
    for (const auto& t : arr->as_array())
        out.push_back(task_from_json(t, where + "[" + std::to_string(i++) + "]", 0));
    return out;
}

}  // namespace

std::string to_markdown(const Document& doc, SerializeOptions options) {
    options.dialect = Dialect::Markdown;
    return Writer(options).write(doc);
}

std::string to_taskml(const Document& doc, const SerializeOptions& options) {
    return Writer(options).write(doc);
}

Json to_json_value(const Document& doc) {
    Json o = Json::object();
    o.set("version", doc.version);
    if (!doc.directives.empty()) {
        Json d = Json::object();
        for (const auto& [k, v] : doc.directives) d.set(k, v);
        o.set("directives", std::move(d));
    }
    o.set("tasks", tasks_to_json(doc.tasks));
    if (!doc.sections.empty()) {
        Json a = Json::array();
        for (const auto& s : doc.sections) {
            Json so = Json::object();
            so.set("title", s.title);
            so.set("level", s.level);
            so.set("tasks", tasks_to_json(s.tasks));
            a.push_back(std::move(so));
        }
        o.set("sections", std::move(a));
    }
    if (!doc.includes.empty()) {
        Json a = Json::array();
        for (const auto& inc : doc.includes) {
            Json io = Json::object();
            io.set("path", inc.path);
            io.set("line", inc.line);
            a.push_back(std::move(io));
        }
        o.set("includes", std::move(a));
    }
    if (doc.view) {
        Json v = Json::object();
        v.set("type", doc.view->type);
        if (!doc.view->options.empty()) {
            Json opts = Json::object();
            for (const auto& [k, val] : doc.view->options) opts.set(k, val);
            v.set("options", std::move(opts));
        }
        o.set("view", std::move(v));
    }
    if (doc.agent_context) {
        const AgentContext& c = *doc.agent_context;
        Json a = Json::object();
        if (c.agent_id) a.set("agentId", *c.agent_id);
        if (c.model) a.set("model", *c.model);
        if (c.session_id) a.set("sessionId", *c.session_id);
        if (c.started_at) a.set("startedAt", *c.started_at);
        if (c.metadata) a.set("metadata", *c.metadata);
        o.set("agentContext", std::move(a));
    }
    if (doc.handoff) {
        const Handoff& h = *doc.handoff;
        Json a = Json::object();
        a.set("from", h.from);
        a.set("to", h.to);
        if (h.reason) a.set("reason", *h.reason);
        if (h.context) a.set("context", *h.context);
        o.set("handoff", std::move(a));
    }
    if (!doc.comments.empty()) {
        Json a = Json::array();
        for (const auto& c : doc.comments) {
            Json co = Json::object();
            co.set("line", c.line);
            co.set("text", c.text);
            a.push_back(std::move(co));
        }
        o.set("comments", std::move(a));
    }
    return o;
}

std::string to_json(const Document& doc, int indent) { return to_json_value(doc).dump(indent); }

Document from_json_value(const Json& j) {
    if (!j.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for document: expected object");
    Document doc;
    if (const Json* v = field(j, "version", Json::Type::String, "")) {
        if (!v->as_string().empty()) doc.version = v->as_string();
    }
    if (const Json* d = field(j, "directives", Json::Type::Object, "")) {
        for (const auto& [k, v] : d->as_object()) {
            if (!v.is_string())
                fail(ErrorCode::InvalidFieldType, "Invalid type for field \"directives." + k + "\": expected string");
            doc.directives.set(k, v.as_string());
        }
    }
    doc.tasks = tasks_from_json(field(j, "tasks", Json::Type::Array, ""), "tasks");
    if (const Json* secs = field(j, "sections", Json::Type::Array, "")) {
        std::size_t i = 0;
        for (const auto& s : secs->as_array()) {
            std::string w = "sections[" + std::to_string(i++) + "]";
            if (!s.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + w + "\": expected object");
            Section sec;
            sec.title = field(s, "title", Json::Type::String, w + ".", true)->as_string();
            if (const Json* lv = field(s, "level", Json::Type::Number, w + "."))
                sec.level = static_cast<int>(lv->as_number());
            sec.tasks = tasks_from_json(field(s, "tasks", Json::Type::Array, w + "."), w + ".tasks");
            doc.sections.push_back(std::move(sec));
        }
    }
    if (const Json* incs = field(j, "includes", Json::Type::Array, "")) {
        std::size_t i = 0;
        for (const auto& inc : incs->as_array()) {
            std::string w = "includes[" + std::to_string(i++) + "]";
            if (!inc.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + w + "\": expected object");
            Include in;
            in.path = field(inc, "path", Json::Type::String, w + ".", true)->as_string();
            if (const Json* ln = field(inc, "line", Json::Type::Number, w + "."))
                in.line = static_cast<int>(ln->as_number());
            doc.includes.push_back(std::move(in));
        }
    }
    if (const Json* v = field(j, "view", Json::Type::Object, "")) {
        ViewConfig vc;
        vc.type = field(*v, "type", Json::Type::String, "view.", true)->as_string();
        if (const Json* opts = field(*v, "options", Json::Type::Object, "view.")) {
            for (const auto& [k, val] : opts->as_object()) {
                if (!val.is_string())
                    fail(ErrorCode::InvalidFieldType, "Invalid type for field \"view.options." + k + "\": expected string");
                vc.options.set(k, val.as_string());
            }
        }
        doc.view = std::move(vc);
    }
    if (const Json* a = field(j, "agentContext", Json::Type::Object, "")) {
        AgentContext c;
        c.agent_id = opt_string(*a, "agentId", "agentContext.");
        c.model = opt_string(*a, "model", "agentContext.");
        c.session_id = opt_string(*a, "sessionId", "agentContext.");
        c.started_at = opt_string(*a, "startedAt", "agentContext.");
        if (const Json* m = a->find("metadata"); m && !m->is_null()) c.metadata = *m;
        doc.agent_context = std::move(c);
    }
    if (const Json* h = field(j, "handoff", Json::Type::Object, "")) {
        Handoff ho;
        if (const Json* f = field(*h, "from", Json::Type::String, "handoff.")) ho.from = f->as_string();
        if (const Json* t = field(*h, "to", Json::Type::String, "handoff.")) ho.to = t->as_string();
        ho.reason = opt_string(*h, "reason", "handoff.");
        if (const Json* c = h->find("context"); c && !c->is_null()) ho.context = *c;
        doc.handoff = std::move(ho);
    }
    if (const Json* cs = field(j, "comments", Json::Type::Array, "")) {
        std::size_t i = 0;
        for (const auto& c : cs->as_array()) {
            std::string w = "comments[" + std::to_string(i++) + "]";
            if (!c.is_object()) fail(ErrorCode::InvalidFieldType, "Invalid type for field \"" + w + "\": expected object");
            Comment cm;
            if (const Json* ln = field(c, "line", Json::Type::Number, w + "."))
                cm.line = static_cast<int>(ln->as_number());
            cm.text = field(c, "text", Json::Type::String, w + ".", true)->as_string();
            doc.comments.push_back(std::move(cm));
        }
    }
    return doc;
}

Document from_json(std::string_view json) {
    Json j;
    try {
        j = Json::parse(json);
    } catch (const JsonParseError& e) {
        fail(ErrorCode::InvalidJson, std::string("Invalid JSON: ") + e.what());
    }
    return from_json_value(j);
}

}  // namespace taskml
