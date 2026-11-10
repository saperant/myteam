// Codex: codex exec --json [resume <id>] -   (prompt on stdin)
//
// Stream: {"type":"thread.started","thread_id"}, {"type":"turn.started"},
// {"type":"item.started"|"item.completed","item":{"id","type",...}} with item
// types agent_message (text), reasoning, command_execution (command,
// exit_code), file_change (changes[].path), mcp_tool_call (server, tool),
// web_search (query), error (message); {"type":"turn.completed","usage"},
// {"type":"turn.failed","error":{"message"}}, {"type":"error","message"}.
#include <map>

#include "myteam/util.hpp"
#include "runners/runners.hpp"

namespace myteam {

namespace {

using namespace runner_detail;

class CodexRunner : public AgentRunner {
public:
    using AgentRunner::AgentRunner;
    std::string id() const override { return "codex"; }

    bool supports_mcp() const override { return true; }

    std::vector<std::string> command(const RunRequest& r) const override {
        std::vector<std::string> a = {"codex", "exec", "--json", "--skip-git-repo-check"};
        if (r.mcp && !r.mcp->command.empty()) {
            // Config overrides are TOML values; JSON strings are TOML basic strings.
            const std::string key = "mcp_servers." + r.mcp->name;
            Json args = Json::array();
            for (std::size_t i = 1; i < r.mcp->command.size(); ++i) args.push_back(r.mcp->command[i]);
            a.insert(a.end(), {"-c", key + ".command=" + Json(r.mcp->command[0]).dump(), "-c", key + ".args=" + args.dump()});
        }
        if (!r.workdir.empty()) a.insert(a.end(), {"--cd", r.workdir.string()});
        if (!r.model.empty()) a.insert(a.end(), {"--model", r.model});
        switch (r.permission) {
            case Permission::ReadOnly: a.insert(a.end(), {"--sandbox", "read-only"}); break;
            case Permission::EditFiles: a.insert(a.end(), {"--sandbox", "workspace-write"}); break;
            case Permission::Full: a.push_back("--dangerously-bypass-approvals-and-sandbox"); break;
        }
        a.insert(a.end(), {"--output-last-message", (r.run_dir / "last-message.md").string()});
        a.insert(a.end(), settings_.args.begin(), settings_.args.end());
        if (r.resume_session) a.insert(a.end(), {"resume", *r.resume_session});
        a.push_back("-");
        return a;
    }

    void parse_line(std::string_view line, StreamState& st, const EventSink& sink) const override {
        auto j = parse_json(line);
        if (!j) return;
        RunResult& res = st.result;
        const std::string type = str(j->find("type"));
        if (type == "thread.started") {
            res.session_id = str(j->find("thread_id"));
        } else if (type == "turn.started") {
            ++res.usage.turns;
        } else if (type == "turn.completed") {
            if (const Json* u = j->find("usage")) {
                res.usage.input_tokens += num(u->find("input_tokens"));
                res.usage.output_tokens += num(u->find("output_tokens"));
                res.usage.cached_input_tokens += num(u->find("cached_input_tokens"));
            }
            emit(sink, AgentEvent::Kind::Usage, std::to_string(res.usage.turns) + " turns");
        } else if (type == "turn.failed") {
            res.error = str(get(*j, {"error", "message"}));
            if (res.error.empty()) res.error = "turn failed";
            emit(sink, AgentEvent::Kind::Error, res.error);
        } else if (type == "error") {
            std::string msg = str(j->find("message"));
            // Reconnect notices are errors in the stream but not fatal.
            if (!starts_with(lower(msg), "reconnecting")) res.error = msg;
            emit(sink, AgentEvent::Kind::Error, msg);
        } else if (type == "item.started" || type == "item.completed") {
            const Json* item = j->find("item");
            if (item) on_item(*item, type == "item.completed", st, sink);
        }
    }

    void finish(const RunRequest& r, StreamState& st) const override {
        if (auto last = try_read_file(r.run_dir / "last-message.md"); last && !trim(*last).empty())
            st.result.final_text = *last;
    }

private:
    void on_item(const Json& item, bool completed, StreamState& st, const EventSink& sink) const {
        RunResult& res = st.result;
        const std::string it = str(item.find("type"));
        const std::string id = str(item.find("id"));
        if (it == "agent_message") {
            if (!completed) return;
            st.last_text = str(item.find("text"));
            emit(sink, AgentEvent::Kind::Text, st.last_text);
        } else if (it == "command_execution") {
            if (!completed || !started_.count(id)) {
                if (!started_.count(id)) {
                    started_[id] = res.tool_calls.size();
                    res.tool_calls.push_back({"shell", tool_detail(item.find("command")), false});
                    emit(sink, AgentEvent::Kind::ToolCall, res.tool_calls.back().detail, "shell");
                }
            }
            if (completed) {
                const Json* code = item.find("exit_code");
                bool failed = (code && code->is_number() && code->as_number() != 0) ||
                              str(item.find("status")) == "failed";
                res.tool_calls[started_[id]].failed = failed;
                emit(sink, AgentEvent::Kind::ToolResult, failed ? "failed" : "ok", "shell");
            }
        } else if (it == "file_change" && completed) {
            std::string paths;
            if (const Json* ch = item.find("changes"); ch && ch->is_array())
                for (const Json& c : ch->as_array()) paths += (paths.empty() ? "" : ", ") + str(c.find("path"));
            res.tool_calls.push_back({"edit", paths, str(item.find("status")) == "failed"});
            emit(sink, AgentEvent::Kind::ToolCall, paths, "edit");
        } else if (it == "mcp_tool_call" && completed) {
            std::string name = str(item.find("server")) + "." + str(item.find("tool"));
            res.tool_calls.push_back({name, tool_detail(item.find("arguments")), str(item.find("status")) == "failed"});
            emit(sink, AgentEvent::Kind::ToolCall, res.tool_calls.back().detail, name);
        } else if (it == "web_search" && completed) {
            res.tool_calls.push_back({"web_search", str(item.find("query")), false});
            emit(sink, AgentEvent::Kind::ToolCall, res.tool_calls.back().detail, "web_search");
        } else if (it == "error") {
            emit(sink, AgentEvent::Kind::Error, str(item.find("message")));
        }
    }

    mutable std::map<std::string, std::size_t> started_;
};

}  // namespace

std::unique_ptr<AgentRunner> make_codex_runner(RunnerSettings settings) {
    return std::make_unique<CodexRunner>(std::move(settings));
}

}  // namespace myteam
