// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Saku Rantamäki
//
// Python bindings for taskml (pybind11).

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <taskml/taskml.hpp>

#include <cmath>
#include <string>

namespace py = pybind11;
using namespace taskml;

// Containers that are exposed by reference so that in-place mutation from
// Python (doc.tasks[0].tags.append("x")) modifies the underlying document.
PYBIND11_MAKE_OPAQUE(std::vector<Task>)
PYBIND11_MAKE_OPAQUE(std::vector<Section>)
PYBIND11_MAKE_OPAQUE(std::vector<Criterion>)
PYBIND11_MAKE_OPAQUE(std::vector<Include>)
PYBIND11_MAKE_OPAQUE(std::vector<Comment>)
PYBIND11_MAKE_OPAQUE(std::vector<std::string>)

namespace {

// ----- Json <-> Python -----

py::object json_to_py(const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: return py::none();
        case Json::Type::Bool: return py::bool_(j.as_bool());
        case Json::Type::Number: {
            double d = j.as_number();
            if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9007199254740992.0)
                return py::int_(static_cast<long long>(d));
            return py::float_(d);
        }
        case Json::Type::String: return py::str(j.as_string());
        case Json::Type::Array: {
            py::list l;
            for (const auto& v : j.as_array()) l.append(json_to_py(v));
            return std::move(l);
        }
        case Json::Type::Object: {
            py::dict d;
            for (const auto& [k, v] : j.as_object()) d[py::str(k)] = json_to_py(v);
            return std::move(d);
        }
    }
    return py::none();
}

Json py_to_json(const py::handle& o, int depth = 0) {
    if (depth > 256) throw py::value_error("object nested too deeply");
    if (o.is_none()) return Json();
    if (py::isinstance<py::bool_>(o)) return Json(o.cast<bool>());
    if (py::isinstance<py::int_>(o)) return Json(o.cast<double>());
    if (py::isinstance<py::float_>(o)) return Json(o.cast<double>());
    if (py::isinstance<py::str>(o)) return Json(o.cast<std::string>());
    if (py::isinstance<py::dict>(o)) {
        Json obj = Json::object();
        for (auto item : o.cast<py::dict>()) {
            if (!py::isinstance<py::str>(item.first))
                throw py::type_error("JSON object keys must be str");
            obj.set(item.first.cast<std::string>(), py_to_json(item.second, depth + 1));
        }
        return obj;
    }
    if (py::isinstance<py::list>(o) || py::isinstance<py::tuple>(o)) {
        Json arr = Json::array();
        for (auto item : o) arr.push_back(py_to_json(item, depth + 1));
        return arr;
    }
    throw py::type_error("value is not JSON serializable: " +
                         py::repr(o).cast<std::string>());
}

std::optional<Json> py_to_opt_json(const py::object& o) {
    if (o.is_none()) return std::nullopt;
    return py_to_json(o);
}

py::object opt_json_to_py(const std::optional<Json>& j) {
    return j ? json_to_py(*j) : py::none();
}

// ----- StringMap <-> Python -----

StringMap stringmap_from_py(const py::handle& o) {
    if (py::isinstance<StringMap>(o)) return o.cast<StringMap>();
    StringMap m;
    for (auto item : o.cast<py::dict>()) m.set(py::str(item.first), py::str(item.second));
    return m;
}

std::string repr_str(const std::string& s) { return py::repr(py::str(s)).cast<std::string>(); }

template <typename T>
std::string repr_opt(const char* name, const std::optional<T>& v) {
    if (!v) return "";
    if constexpr (std::is_same_v<T, std::string>)
        return std::string(", ") + name + "=" + repr_str(*v);
    else
        return std::string(", ") + name + "=" + std::to_string(*v);
}

}  // namespace

PYBIND11_MODULE(_taskml, m) {
    m.doc() = "TaskML (https://taskml.dev) parser, validator and serializer";
    m.attr("__version__") = library_version();
    m.attr("SPEC_VERSION") = spec_version();

    // ----- exceptions -----
    // Intentionally leaked: must outlive the interpreter's module teardown.
    static py::handle taskml_error = (new py::exception<Error>(m, "TaskMLError", PyExc_ValueError))->release();
    taskml_error.inc_ref();
    m.attr("TaskMLError") = taskml_error;
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const Error& e) {
            py::object inst = taskml_error(e.what());  // instantiate the exception class
            inst.attr("diagnostics") = py::cast(e.diagnostics());
            PyErr_SetObject(taskml_error.ptr(), inst.ptr());
        } catch (const JsonParseError& e) {
            PyErr_SetString(PyExc_ValueError, e.what());
        }
    });

    // ----- enums -----
    py::enum_<TaskStatus>(m, "TaskStatus", "Task status ([ ], [~], [x], [!], [-], [?])")
        .value("PENDING", TaskStatus::Pending)
        .value("IN_PROGRESS", TaskStatus::InProgress)
        .value("COMPLETED", TaskStatus::Completed)
        .value("BLOCKED", TaskStatus::Blocked)
        .value("CANCELLED", TaskStatus::Cancelled)
        .value("REVIEW", TaskStatus::Review)
        .def_property_readonly("label", [](TaskStatus s) { return to_string(s); },
                               "Canonical name, e.g. 'in_progress'")
        .def_property_readonly("marker", [](TaskStatus s) { return std::string(1, status_marker(s)); },
                               "Character between the brackets, e.g. '~'")
        .def_static("from_label", [](const std::string& s) {
            auto v = task_status_from_string(s);
            if (!v) throw py::value_error("unknown task status: " + s);
            return *v;
        });

    py::enum_<CriterionStatus>(m, "CriterionStatus", "Verification criterion status (○, ✓, ✗)")
        .value("PENDING", CriterionStatus::Pending)
        .value("VERIFIED", CriterionStatus::Verified)
        .value("FAILED", CriterionStatus::Failed)
        .def_property_readonly("label", [](CriterionStatus s) { return to_string(s); })
        .def_property_readonly("marker", [](CriterionStatus s) { return criterion_marker(s); })
        .def_property_readonly("ascii_marker", [](CriterionStatus s) { return criterion_marker_ascii(s); })
        .def_static("from_label", [](const std::string& s) {
            auto v = criterion_status_from_string(s);
            if (!v) throw py::value_error("unknown criterion status: " + s);
            return *v;
        });

    py::enum_<Severity>(m, "Severity")
        .value("ERROR", Severity::Error)
        .value("WARNING", Severity::Warning)
        .value("INFO", Severity::Info);

    py::enum_<ErrorCode> codes(m, "ErrorCode", "Diagnostic codes (E1xx lexical, E2xx syntax, E3xx semantic, E4xx interchange)");
    codes.value("UNEXPECTED_CHARACTER", ErrorCode::UnexpectedCharacter)
        .value("INCONSISTENT_INDENTATION", ErrorCode::InconsistentIndentation)
        .value("INVALID_ESCAPE", ErrorCode::InvalidEscape)
        .value("INVALID_DATE_FORMAT", ErrorCode::InvalidDateFormat)
        .value("INVALID_ESTIMATE_FORMAT", ErrorCode::InvalidEstimateFormat)
        .value("INVALID_PRIORITY", ErrorCode::InvalidPriority)
        .value("EXPECTED_STATUS", ErrorCode::ExpectedStatus)
        .value("EXPECTED_DESCRIPTION", ErrorCode::ExpectedDescription)
        .value("EXPECTED_DIRECTIVE_VALUE", ErrorCode::ExpectedDirectiveValue)
        .value("UNEXPECTED_TOKEN", ErrorCode::UnexpectedToken)
        .value("INVALID_NESTING", ErrorCode::InvalidNesting)
        .value("EXPECTED_FENCE_END", ErrorCode::ExpectedFenceEnd)
        .value("INVALID_SECTION", ErrorCode::InvalidSection)
        .value("INVALID_CRITERION", ErrorCode::InvalidCriterion)
        .value("DUPLICATE_TASK_ID", ErrorCode::DuplicateTaskId)
        .value("UNKNOWN_DIRECTIVE", ErrorCode::UnknownDirective)
        .value("INVALID_VIEW_TYPE", ErrorCode::InvalidViewType)
        .value("UNKNOWN_STATUS", ErrorCode::UnknownStatus)
        .value("UNKNOWN_DEPENDENCY", ErrorCode::UnknownDependency)
        .value("CIRCULAR_DEPENDENCY", ErrorCode::CircularDependency)
        .value("TOKEN_ORDER_VIOLATION", ErrorCode::TokenOrderViolation)
        .value("UNKNOWN_BLOCKER", ErrorCode::UnknownBlocker)
        .value("INVALID_VERSION", ErrorCode::InvalidVersion)
        .value("MULTIPLE_ASSIGNEES", ErrorCode::MultipleAssignees)
        .value("INVALID_JSON", ErrorCode::InvalidJson)
        .value("MISSING_REQUIRED_FIELD", ErrorCode::MissingRequiredField)
        .value("INVALID_FIELD_TYPE", ErrorCode::InvalidFieldType);
    codes.def_property_readonly("code", [](ErrorCode c) { return to_string(c); }, "e.g. 'E101'");

    // ----- containers -----
    py::bind_vector<std::vector<std::string>>(m, "StringList");
    py::bind_vector<std::vector<Task>>(m, "TaskList");
    py::bind_vector<std::vector<Section>>(m, "SectionList");
    py::bind_vector<std::vector<Criterion>>(m, "CriterionList");
    py::bind_vector<std::vector<Include>>(m, "IncludeList");
    py::bind_vector<std::vector<Comment>>(m, "CommentList");
    py::implicitly_convertible<py::list, std::vector<std::string>>();
    py::implicitly_convertible<py::list, std::vector<Task>>();
    py::implicitly_convertible<py::list, std::vector<Section>>();
    py::implicitly_convertible<py::list, std::vector<Criterion>>();
    py::implicitly_convertible<py::list, std::vector<Include>>();
    py::implicitly_convertible<py::list, std::vector<Comment>>();

    py::class_<StringMap>(m, "StringMap", "Insertion-ordered str -> str mapping")
        .def(py::init<>())
        .def(py::init([](const py::dict& d) { return stringmap_from_py(d); }))
        .def("__getitem__", [](const StringMap& s, const std::string& k) {
            const std::string* v = s.get(k);
            if (!v) throw py::key_error(k);
            return *v;
        })
        .def("__setitem__", [](StringMap& s, std::string k, std::string v) { s.set(std::move(k), std::move(v)); })
        .def("__delitem__", [](StringMap& s, const std::string& k) {
            if (!s.erase(k)) throw py::key_error(k);
        })
        .def("__contains__", [](const StringMap& s, const std::string& k) { return s.contains(k); })
        .def("__len__", &StringMap::size)
        .def("__iter__", [](const StringMap& s) {
            py::list keys;
            for (const auto& kv : s) keys.append(kv.first);
            return py::iter(keys);
        })
        .def("get", [](const StringMap& s, const std::string& k, py::object dflt) -> py::object {
            const std::string* v = s.get(k);
            return v ? py::str(*v) : dflt;
        }, py::arg("key"), py::arg("default") = py::none())
        .def("keys", [](const StringMap& s) {
            py::list l;
            for (const auto& kv : s) l.append(kv.first);
            return l;
        })
        .def("values", [](const StringMap& s) {
            py::list l;
            for (const auto& kv : s) l.append(kv.second);
            return l;
        })
        .def("items", [](const StringMap& s) {
            py::list l;
            for (const auto& kv : s) l.append(py::make_tuple(kv.first, kv.second));
            return l;
        })
        .def("clear", &StringMap::clear)
        .def("to_dict", [](const StringMap& s) {
            py::dict d;
            for (const auto& kv : s) d[py::str(kv.first)] = kv.second;
            return d;
        })
        .def(py::self == py::self)
        .def("__eq__", [](const StringMap& s, const py::dict& d) { return s == stringmap_from_py(d); })
        .def("__repr__", [](const StringMap& s) {
            std::string r = "StringMap({";
            bool first = true;
            for (const auto& kv : s) {
                if (!first) r += ", ";
                first = false;
                r += repr_str(kv.first) + ": " + repr_str(kv.second);
            }
            return r + "})";
        });
    py::implicitly_convertible<py::dict, StringMap>();

    // ----- model -----
    py::class_<Criterion>(m, "Criterion", "Verification criterion (✓ / ✗ / ○)")
        .def(py::init([](std::string description, CriterionStatus status, std::optional<std::string> evidence) {
                 Criterion c;
                 c.description = std::move(description);
                 c.status = status;
                 c.evidence = std::move(evidence);
                 return c;
             }),
             py::arg("description") = "", py::arg("status") = CriterionStatus::Pending,
             py::arg("evidence") = py::none())
        .def_readwrite("description", &Criterion::description)
        .def_readwrite("status", &Criterion::status)
        .def_readwrite("evidence", &Criterion::evidence)
        .def_readonly("line", &Criterion::line)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def("__repr__", [](const Criterion& c) {
            return "Criterion(" + repr_str(c.description) + ", " + to_string(c.status) +
                   repr_opt("evidence", c.evidence) + ")";
        });

    py::class_<Task>(m, "Task", "A TaskML task")
        .def(py::init([](std::string description, TaskStatus status, std::optional<std::string> id,
                         std::optional<int> priority, std::optional<std::string> estimate,
                         std::optional<std::string> assignee, std::vector<std::string> tags,
                         std::optional<std::string> due, std::vector<std::string> depends_on,
                         std::vector<std::string> blocked_by, std::vector<Task> subtasks,
                         std::vector<Criterion> criteria, std::vector<std::string> notes) {
                 Task t;
                 t.description = std::move(description);
                 t.status = status;
                 t.id = std::move(id);
                 t.priority = priority;
                 t.estimate = std::move(estimate);
                 t.assignee = std::move(assignee);
                 t.tags = std::move(tags);
                 t.due = std::move(due);
                 t.depends_on = std::move(depends_on);
                 t.blocked_by = std::move(blocked_by);
                 t.subtasks = std::move(subtasks);
                 t.criteria = std::move(criteria);
                 t.notes = std::move(notes);
                 return t;
             }),
             py::arg("description") = "", py::arg("status") = TaskStatus::Pending,
             py::arg("id") = py::none(), py::arg("priority") = py::none(),
             py::arg("estimate") = py::none(), py::arg("assignee") = py::none(),
             py::arg("tags") = std::vector<std::string>{}, py::arg("due") = py::none(),
             py::arg("depends_on") = std::vector<std::string>{},
             py::arg("blocked_by") = std::vector<std::string>{},
             py::arg("subtasks") = std::vector<Task>{},
             py::arg("criteria") = std::vector<Criterion>{},
             py::arg("notes") = std::vector<std::string>{})
        .def_readwrite("status", &Task::status)
        .def_readwrite("description", &Task::description)
        .def_readwrite("id", &Task::id)
        .def_readwrite("priority", &Task::priority)
        .def_readwrite("estimate", &Task::estimate)
        .def_readwrite("assignee", &Task::assignee)
        .def_readwrite("tags", &Task::tags)
        .def_readwrite("due", &Task::due)
        .def_readwrite("depends_on", &Task::depends_on)
        .def_readwrite("blocked_by", &Task::blocked_by)
        .def_readwrite("subtasks", &Task::subtasks)
        .def_readwrite("criteria", &Task::criteria)
        .def_readwrite("notes", &Task::notes)
        .def_readonly("line", &Task::line)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def("__repr__", [](const Task& t) {
            return std::string("Task(") + repr_str(t.description) + ", " + to_string(t.status) +
                   repr_opt("id", t.id) + repr_opt("priority", t.priority) +
                   repr_opt("estimate", t.estimate) + repr_opt("assignee", t.assignee) +
                   repr_opt("due", t.due) +
                   (t.subtasks.empty() ? "" : ", subtasks=" + std::to_string(t.subtasks.size())) + ")";
        });

    py::class_<Section>(m, "Section", "'== Title ==' section and its tasks")
        .def(py::init([](std::string title, int level, std::vector<Task> tasks) {
                 Section s;
                 s.title = std::move(title);
                 s.level = level;
                 s.tasks = std::move(tasks);
                 return s;
             }),
             py::arg("title") = "", py::arg("level") = 2, py::arg("tasks") = std::vector<Task>{})
        .def_readwrite("title", &Section::title)
        .def_readwrite("level", &Section::level)
        .def_readwrite("tasks", &Section::tasks)
        .def_readonly("line", &Section::line)
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def("__repr__", [](const Section& s) {
            return "Section(" + repr_str(s.title) + ", level=" + std::to_string(s.level) +
                   ", tasks=" + std::to_string(s.tasks.size()) + ")";
        });

    py::class_<Include>(m, "Include", "'< path' include line (not resolved)")
        .def(py::init([](std::string path) { return Include{std::move(path), 0}; }), py::arg("path"))
        .def_readwrite("path", &Include::path)
        .def_readonly("line", &Include::line)
        .def(py::self == py::self)
        .def("__repr__", [](const Include& i) { return "Include(" + repr_str(i.path) + ")"; });

    py::class_<Comment>(m, "Comment", "'// comment' line")
        .def(py::init([](int line, std::string text) { return Comment{line, std::move(text)}; }),
             py::arg("line") = 0, py::arg("text") = "")
        .def_readwrite("line", &Comment::line)
        .def_readwrite("text", &Comment::text)
        .def(py::self == py::self)
        .def("__repr__", [](const Comment& c) {
            return "Comment(line=" + std::to_string(c.line) + ", text=" + repr_str(c.text) + ")";
        });

    py::class_<ViewConfig>(m, "ViewConfig", "'---view:type key=value' rendering hint")
        .def(py::init([](std::string type, const py::object& options) {
                 ViewConfig v;
                 v.type = std::move(type);
                 if (!options.is_none()) v.options = stringmap_from_py(options);
                 return v;
             }),
             py::arg("type") = "list", py::arg("options") = py::none())
        .def_readwrite("type", &ViewConfig::type)
        .def_property("options", [](ViewConfig& v) -> StringMap& { return v.options; },
                      [](ViewConfig& v, const py::object& o) { v.options = stringmap_from_py(o); },
                      py::return_value_policy::reference_internal)
        .def(py::self == py::self)
        .def("__repr__", [](const ViewConfig& v) {
            return "ViewConfig(" + repr_str(v.type) + ", options=" +
                   py::repr(py::cast(v.options)).cast<std::string>() + ")";
        });

    py::class_<AgentContext>(m, "AgentContext", "@agent / @agent-id directives and ---context block")
        .def(py::init<>())
        .def_readwrite("agent_id", &AgentContext::agent_id)
        .def_readwrite("model", &AgentContext::model)
        .def_readwrite("session_id", &AgentContext::session_id)
        .def_readwrite("started_at", &AgentContext::started_at)
        .def_property("metadata", [](const AgentContext& c) { return opt_json_to_py(c.metadata); },
                      [](AgentContext& c, const py::object& o) { c.metadata = py_to_opt_json(o); },
                      "Full ---context block as Python data (copy; assign to modify)")
        .def(py::self == py::self)
        .def("__repr__", [](const AgentContext& c) {
            std::string r = repr_opt("agent_id", c.agent_id) + repr_opt("model", c.model) +
                            repr_opt("session_id", c.session_id) + repr_opt("started_at", c.started_at);
            return "AgentContext(" + (r.empty() ? r : r.substr(2)) + ")";
        });

    py::class_<Handoff>(m, "Handoff", "---handoff block")
        .def(py::init([](std::string from, std::string to, std::optional<std::string> reason,
                         const py::object& context) {
                 Handoff h;
                 h.from = std::move(from);
                 h.to = std::move(to);
                 h.reason = std::move(reason);
                 h.context = py_to_opt_json(context);
                 return h;
             }),
             py::arg("from_") = "", py::arg("to") = "", py::arg("reason") = py::none(),
             py::arg("context") = py::none())
        .def_readwrite("from_", &Handoff::from, "'from:' field ('from' is a Python keyword)")
        .def_readwrite("to", &Handoff::to)
        .def_readwrite("reason", &Handoff::reason)
        .def_property("context", [](const Handoff& h) { return opt_json_to_py(h.context); },
                      [](Handoff& h, const py::object& o) { h.context = py_to_opt_json(o); })
        .def(py::self == py::self)
        .def("__repr__", [](const Handoff& h) {
            return "Handoff(from_=" + repr_str(h.from) + ", to=" + repr_str(h.to) +
                   repr_opt("reason", h.reason) + ")";
        });

    py::class_<Document>(m, "Document", "A TaskML document")
        .def(py::init<>())
        .def_readwrite("version", &Document::version)
        .def_property("directives", [](Document& d) -> StringMap& { return d.directives; },
                      [](Document& d, const py::object& o) { d.directives = stringmap_from_py(o); },
                      py::return_value_policy::reference_internal)
        .def_readwrite("tasks", &Document::tasks)
        .def_readwrite("sections", &Document::sections)
        .def_readwrite("includes", &Document::includes)
        .def_readwrite("view", &Document::view)
        .def_readwrite("agent_context", &Document::agent_context)
        .def_readwrite("handoff", &Document::handoff)
        .def_readwrite("comments", &Document::comments)
        .def("find_task", [](Document& d, const std::string& id) { return d.find_task(id); },
             py::arg("id"), py::return_value_policy::reference_internal,
             "Find a task anywhere in the document by its ^id; None if absent")
        .def("all_tasks", [](Document& d) {
                 py::list out;
                 py::object self = py::cast(&d, py::return_value_policy::reference);
                 for (const Task* t : d.all_tasks())
                     out.append(py::cast(const_cast<Task*>(t), py::return_value_policy::reference_internal, self));
                 return out;
             },
             "All tasks in document order (depth-first), including subtasks and section tasks")
        .def("validate", [](const Document& d) { return validate(d); },
             "Semantic validation: duplicate IDs, unknown/circular dependencies, ...")
        .def("to_taskml", [](const Document& d, int indent_size, bool use_tabs, bool include_comments) {
                 SerializeOptions o;
                 o.indent_size = indent_size;
                 o.use_tabs = use_tabs;
                 o.include_comments = include_comments;
                 return to_taskml(d, o);
             },
             py::arg("indent_size") = 4, py::arg("use_tabs") = false, py::arg("include_comments") = false)
        .def("to_markdown", [](const Document& d, bool include_comments) {
                 SerializeOptions o;
                 o.include_comments = include_comments;
                 return to_markdown(d, o);
             },
             py::arg("include_comments") = false)
        .def("to_json", [](const Document& d, std::optional<int> indent) { return to_json(d, indent ? *indent : -1); },
             py::arg("indent") = 2, "JSON interchange format; indent=None for compact output")
        .def("to_dict", [](const Document& d) { return json_to_py(to_json_value(d)); },
             "JSON interchange format as Python data")
        .def_static("from_json", [](const std::string& s) { return from_json(s); }, py::arg("json"))
        .def_static("from_dict", [](const py::object& o) { return from_json_value(py_to_json(o)); }, py::arg("data"))
        .def(py::self == py::self)
        .def(py::self != py::self)
        .def(py::pickle([](const Document& d) { return to_json(d, -1); },
                        [](const std::string& s) { return from_json(s); }))
        .def("__copy__", [](const Document& d) { return Document(d); })
        .def("__deepcopy__", [](const Document& d, py::dict) { return Document(d); }, py::arg("memo"))
        .def("__repr__", [](const Document& d) {
            const std::string* project = d.directives.get("project");
            return std::string("Document(") + (project ? "project=" + repr_str(*project) + ", " : "") +
                   "tasks=" + std::to_string(d.all_tasks().size()) +
                   ", sections=" + std::to_string(d.sections.size()) + ")";
        });

    py::class_<Diagnostic>(m, "Diagnostic", "A parse/validation error or warning")
        .def_readonly("code", &Diagnostic::code)
        .def_readonly("severity", &Diagnostic::severity)
        .def_readonly("line", &Diagnostic::line)
        .def_readonly("column", &Diagnostic::column)
        .def_readonly("message", &Diagnostic::message)
        .def_readonly("suggestion", &Diagnostic::suggestion)
        .def("format", [](const Diagnostic& d, const std::string& source) { return d.format(source); },
             py::arg("source") = "", "Human readable form; quotes the source line when given")
        .def("__str__", [](const Diagnostic& d) { return d.format(); })
        .def("__repr__", [](const Diagnostic& d) {
            return "Diagnostic(" + to_string(d.code) + ", " + to_string(d.severity) + ", line=" +
                   std::to_string(d.line) + ", column=" + std::to_string(d.column) + ", " +
                   repr_str(d.message) + ")";
        });

    py::class_<ParseResult>(m, "ParseResult")
        .def_readwrite("document", &ParseResult::document)
        .def_readonly("errors", &ParseResult::errors)
        .def_readonly("warnings", &ParseResult::warnings)
        .def_property_readonly("ok", &ParseResult::ok)
        .def("__bool__", &ParseResult::ok)
        .def("__repr__", [](const ParseResult& r) {
            return "ParseResult(ok=" + std::string(r.ok() ? "True" : "False") +
                   ", errors=" + std::to_string(r.errors.size()) +
                   ", warnings=" + std::to_string(r.warnings.size()) + ")";
        });

    // ----- functions -----
    auto to_dialect = [](const std::string& name) {
        if (name == "markdown") return Dialect::Markdown;
        if (name == "taskml") return Dialect::TaskML;
        throw py::value_error("dialect must be 'taskml' or 'markdown', not '" + name + "'");
    };
    auto make_opts = [to_dialect](bool strict, bool allow_unknown_directives, int max_depth,
                                  const std::string& dialect) {
        ParseOptions o;
        o.dialect = to_dialect(dialect);
        o.strict = strict;
        o.allow_unknown_directives = allow_unknown_directives;
        o.max_depth = max_depth;
        return o;
    };

    m.def("parse",
          [make_opts](const std::string& text, bool strict, bool allow_unknown_directives, int max_depth,
                      const std::string& dialect) {
              ParseOptions o = make_opts(strict, allow_unknown_directives, max_depth, dialect);
              py::gil_scoped_release release;
              return parse(text, o);
          },
          py::arg("text"), py::kw_only(), py::arg("strict") = false,
          py::arg("allow_unknown_directives") = true, py::arg("max_depth") = 64,
          py::arg("dialect") = "taskml",
          "Parse a plan ('taskml' or 'markdown' dialect). Never raises on malformed input; "
          "see .errors / .warnings.");

    m.def("parse_or_raise",
          [make_opts](const std::string& text, bool strict, bool allow_unknown_directives, int max_depth,
                      const std::string& dialect) {
              return parse_or_throw(text, make_opts(strict, allow_unknown_directives, max_depth, dialect));
          },
          py::arg("text"), py::kw_only(), py::arg("strict") = false,
          py::arg("allow_unknown_directives") = true, py::arg("max_depth") = 64,
          py::arg("dialect") = "taskml",
          "Parse TaskML text; raises TaskMLError if there are errors.");

    m.def("validate", [to_dialect](const std::string& text, const std::string& dialect) {
              Dialect d = to_dialect(dialect);
              py::gil_scoped_release release;
              return validate(std::string_view(text), d);
          },
          py::arg("text"), py::kw_only(), py::arg("dialect") = "taskml",
          "Strict parse + semantic validation; returns all diagnostics.");
    m.def("validate", [](const Document& d) { return validate(d); }, py::arg("document"));

    m.def("to_taskml", [](const Document& d, int indent_size, bool use_tabs, bool include_comments) {
              SerializeOptions o;
              o.indent_size = indent_size;
              o.use_tabs = use_tabs;
              o.include_comments = include_comments;
              return to_taskml(d, o);
          },
          py::arg("document"), py::kw_only(), py::arg("indent_size") = 4, py::arg("use_tabs") = false,
          py::arg("include_comments") = false, "Serialize a document as canonical TaskML text.");
    m.def("to_markdown", [](const Document& d, bool include_comments) {
              SerializeOptions o;
              o.include_comments = include_comments;
              return to_markdown(d, o);
          },
          py::arg("document"), py::kw_only(), py::arg("include_comments") = false,
          "Serialize a document as a Markdown plan.");
    m.def("to_json", [](const Document& d, std::optional<int> indent) { return to_json(d, indent ? *indent : -1); },
          py::arg("document"), py::kw_only(), py::arg("indent") = 2);
    m.def("from_json", [](const std::string& s) { return from_json(s); }, py::arg("json"),
          "Parse the JSON interchange format; raises TaskMLError.");

    m.def("standard_views", []() {
        py::list l;
        for (const auto& s : standard_views()) l.append(s);
        return l;
    });
    m.def("standard_directives", []() {
        py::list l;
        for (const auto& s : standard_directives()) l.append(s);
        return l;
    });
}
