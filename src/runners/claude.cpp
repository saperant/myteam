// Claude Code: claude -p --output-format stream-json --verbose
//
// Stream: {"type":"system","subtype":"init","session_id","model"},
// {"type":"assistant","message":{"content":[{"type":"text"|"tool_use",...}]}},
// {"type":"user","message":{"content":[{"type":"tool_result","is_error"}]}},
// {"type":"result","subtype","result","session_id","total_cost_usd","usage",
//  "num_turns","is_error"}.
#include <map>

#include "runners/runners.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace runner_detail {

std::optional<Json> parse_json(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '{') return std::nullopt;
    try {
        return Json::parse(line);
    } catch (const taskml::JsonParseError&) {
        return std::nullopt;
    }
}

std::string str(const Json* j) { return j && j->is_string() ? j->as_string() : std::string(); }
long long num(const Json* j) { return j && j->is_number() ? static_cast<long long>(j->as_number()) : 0; }
double dbl(const Json* j) { return j && j->is_number() ? j->as_number() : 0.0; }

const Json* get(const Json& j, std::initializer_list<const char*> path) {
    const Json* cur = &j;
    for (const char* k : path) {
        cur = cur->find(k);
        if (!cur) return nullptr;
    }
    return cur;
}

std::string tool_detail(const Json* input, std::size_t max) {
    if (!input) return {};
    std::string out;
    if (input->is_string()) {
        out = input->as_string();
    } else if (input->is_object()) {
        for (const char* k : {"command", "cmd", "file_path", "path", "pattern", "url", "query", "description"}) {
            const Json* v = input->find(k);
            if (v && v->is_string()) {
                out = v->as_string();
                break;
            }
            if (v && v->is_array()) {  // codex: ["bash", "-lc", "..."]
                for (const Json& part : v->as_array())
                    if (part.is_string()) out += (out.empty() ? "" : " ") + part.as_string();
                break;
            }
        }
        if (out.empty()) out = input->dump();
    } else {
        out = input->dump();
    }
    if (out.size() > max) out = out.substr(0, max) + "…";
    return out;
}

void emit(const EventSink& sink, AgentEvent::Kind kind, std::string text, std::string tool) {
    if (sink) sink({kind, std::move(text), std::move(tool)});
}

}  // namespace runner_detail

namespace {

using namespace runner_detail;

class ClaudeRunner : public AgentRunner {
public:
    using AgentRunner::AgentRunner;
    std::string id() const override { return "claude"; }
    bool supports_mcp() const override { return true; }

    std::vector<std::string> command(const RunRequest& r) const override {
        std::vector<std::string> a = {"claude", "-p", "--output-format", "stream-json", "--verbose"};
        if (!r.model.empty()) a.insert(a.end(), {"--model", r.model});
        if (r.talk) {
            // One answer, nothing else: no tools, no MCP servers, no skills, no settings, and a
            // system prompt of its own instead of Claude Code's.
            a.insert(a.end(), {"--tools", "", "--strict-mcp-config", "--disable-slash-commands", "--setting-sources", "",
                               "--system-prompt",
                               r.system_prompt.empty() ? std::string("You answer in the format the message asks for.")
                                                       : r.system_prompt});
            if (settings_.isolate) a.push_back("--bare");
            if (r.max_cost_usd > 0) a.insert(a.end(), {"--max-budget-usd", std::to_string(r.max_cost_usd)});
            a.insert(a.end(), settings_.args.begin(), settings_.args.end());
            return a;
        }
        if (settings_.slim)
            a.insert(a.end(), {"--strict-mcp-config", "--disable-slash-commands", "--setting-sources", "",
                               "--exclude-dynamic-system-prompt-sections"});
        const char* mode = "acceptEdits";
        if (r.permission == Permission::ReadOnly) mode = "plan";
        if (r.permission == Permission::Full) mode = "bypassPermissions";
        a.insert(a.end(), {"--permission-mode", mode});
        if (settings_.isolate) a.push_back("--bare");
        if (r.resume_session) a.insert(a.end(), {"--resume", *r.resume_session});
        if (r.max_cost_usd > 0) a.insert(a.end(), {"--max-budget-usd", std::to_string(r.max_cost_usd)});
        std::string denied;
        for (const std::string& d : r.deny_paths)
            for (const char* tool : {"Read", "Edit", "Write"})
                denied += (denied.empty() ? "" : ",") + std::string(tool) + "(./" + d + "/**)";
        {
            std::string& rules = denied;
            for (std::string d : r.deny_read) {
                while (d.size() > 1 && d.back() == '/') d.pop_back();
                for (const char* tool : {"Read", "Grep", "Glob"}) {
                    rules += (rules.empty() ? "" : ",") + std::string(tool) + "(/" + d + ")";
                    rules += std::string(",") + tool + "(/" + d + "/**)";
                }
            }
        }
        if (!denied.empty()) a.insert(a.end(), {"--disallowedTools", denied});
        if (r.mcp && !r.mcp->command.empty()) {
            Json server = Json::object();
            server.set("type", "stdio");
            server.set("command", r.mcp->command[0]);
            Json args = Json::array();
            for (std::size_t i = 1; i < r.mcp->command.size(); ++i) args.push_back(r.mcp->command[i]);
            server.set("args", args);
            Json servers = Json::object();
            servers.set(r.mcp->name, server);
            Json cfg = Json::object();
            cfg.set("mcpServers", servers);
            // The server's tools need no permission prompt (there is nobody to answer one).
            a.insert(a.end(), {"--mcp-config", cfg.dump()});
        }
        if (!r.guard_hook.empty()) {
            std::string cmd;
            for (const auto& w : r.guard_hook) cmd += (cmd.empty() ? "" : " ") + shell_quote(w);
            Json hook = Json::object();
            hook.set("type", "command");
            hook.set("command", cmd);
            Json entry = Json::object();
            entry.set("matcher", "Bash|Edit|Write|MultiEdit|NotebookEdit|Read|Grep|Glob");
            entry.set("hooks", Json::array({hook}));
            Json hooks = Json::object();
            hooks.set("PreToolUse", Json::array({entry}));
            Json settings = Json::object();
            settings.set("hooks", hooks);
            a.insert(a.end(), {"--settings", settings.dump()});
        }
        // Tools that need no prompt (there is nobody to answer one): the MCP
        // server's, and with edit permission the shell commands allowed.
        std::string allowed;
        if (r.mcp && !r.mcp->command.empty()) allowed = "mcp__" + r.mcp->name;
        if (r.permission == Permission::EditFiles || (r.permission == Permission::ReadOnly && r.read_only_shell))
            for (const std::string& c : r.commands) {
                std::string rule = c == "all" ? "Bash" : "Bash(" + c + ")";
                allowed += (allowed.empty() ? "" : ",") + rule;
            }
        // Web pages, from the project's domains only (reading: any permission). A
        // domain rule matches the host exactly, so the www. host is given too
        // (example.org does not allow www.example.org).
        for (const std::string& d : r.web_domains) {
            allowed += (allowed.empty() ? "" : ",") + std::string("WebFetch(domain:") + d + ")";
            if (d.rfind("www.", 0) != 0) allowed += ",WebFetch(domain:www." + d + ")";
        }
        // In the sandbox /tmp is the run's own: what the agent writes there (a
        // screenshot, a log) it may read back (Read outside the project is asked
        // otherwise, and nobody answers).
        if (!r.sandbox.empty()) allowed += std::string(allowed.empty() ? "" : ",") + "Read(//tmp/**)";
        // The directories outside the project it is to read (the org's interfaces, a tool's docs).
        for (std::string d : r.read_paths) {
            while (d.size() > 1 && d.back() == '/') d.pop_back();
            allowed += std::string(allowed.empty() ? "" : ",") + "Read(/" + d + "/**)";
        }
        // ... and those it writes (a contract it owns): acceptEdits covers the working
        // directory only (an Edit of ../interfaces/schema/README.md would be refused).
        if (r.permission != Permission::ReadOnly)
            for (std::string d : r.write_paths) {
                while (d.size() > 1 && d.back() == '/') d.pop_back();
                allowed += std::string(allowed.empty() ? "" : ",") + "Edit(/" + d + "/**),Write(/" + d + "/**)";
            }
        if (!allowed.empty()) a.insert(a.end(), {"--allowedTools", allowed});
        a.insert(a.end(), settings_.args.begin(), settings_.args.end());
        return a;
    }

    void parse_line(std::string_view line, StreamState& st, const EventSink& sink) const override {
        auto j = parse_json(line);
        if (!j) return;
        RunResult& res = st.result;
        const std::string type = str(j->find("type"));
        if (type == "system" && str(j->find("subtype")) == "init") {
            res.session_id = str(j->find("session_id"));
            res.model = str(j->find("model"));
        } else if (type == "assistant") {
            const Json* content = get(*j, {"message", "content"});
            if (!content || !content->is_array()) return;
            for (const Json& c : content->as_array()) {
                const std::string ct = str(c.find("type"));
                if (ct == "text") {
                    st.last_text = str(c.find("text"));
                    emit(sink, AgentEvent::Kind::Text, st.last_text);
                } else if (ct == "tool_use") {
                    ToolCall call{str(c.find("name")), tool_detail(c.find("input")), false};
                    pending_[str(c.find("id"))] = res.tool_calls.size();
                    res.tool_calls.push_back(call);
                    emit(sink, AgentEvent::Kind::ToolCall, call.detail, call.name);
                }
            }
        } else if (type == "user") {
            const Json* content = get(*j, {"message", "content"});
            if (!content || !content->is_array()) return;
            for (const Json& c : content->as_array()) {
                if (str(c.find("type")) != "tool_result") continue;
                const Json* err = c.find("is_error");
                bool failed = err && err->is_bool() && err->as_bool();
                auto it = pending_.find(str(c.find("tool_use_id")));
                std::string name;
                if (it != pending_.end() && it->second < res.tool_calls.size()) {
                    res.tool_calls[it->second].failed = failed;
                    name = res.tool_calls[it->second].name;
                }
                emit(sink, AgentEvent::Kind::ToolResult, failed ? "failed" : "ok", name);
            }
        } else if (type == "result") {
            res.final_text = str(j->find("result"));
            if (res.session_id.empty()) res.session_id = str(j->find("session_id"));
            res.stop_reason = str(j->find("subtype"));
            res.usage.cost_usd = dbl(j->find("total_cost_usd"));
            res.usage.turns = static_cast<int>(num(j->find("num_turns")));
            if (const Json* u = j->find("usage")) {
                res.usage.input_tokens = num(u->find("input_tokens"));
                res.usage.output_tokens = num(u->find("output_tokens"));
                res.usage.cached_input_tokens = num(u->find("cache_read_input_tokens"));
            }
            const Json* is_error = j->find("is_error");
            if (is_error && is_error->is_bool() && is_error->as_bool())
                res.error = res.final_text.empty() ? "claude reported an error (" + res.stop_reason + ")" : res.final_text;
            emit(sink, AgentEvent::Kind::Usage, std::to_string(res.usage.turns) + " turns");
        }
    }

private:
    // tool_use id -> index in tool_calls. A runner instance serves one run
    // at a time.
    mutable std::map<std::string, std::size_t> pending_;
};

}  // namespace

std::unique_ptr<AgentRunner> make_claude_runner(RunnerSettings settings) {
    return std::make_unique<ClaudeRunner>(std::move(settings));
}

}  // namespace myteam
