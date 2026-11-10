// Mistral Vibe: vibe --prompt-file <f> --output streaming --output-dir <d>
//
// Vibe chooses its model from VIBE_ACTIVE_MODEL (an alias from its config).
// On exit it writes <d>/export.json: version, effective config, usage,
// cost, stop reason, outcome and session id. The streaming format is read
// leniently: assistant "message" entries give text, "effect" entries tool
// activity; export.json is authoritative for usage and the session.
#include "myteam/util.hpp"
#include "runners/runners.hpp"

namespace myteam {

namespace {

using namespace runner_detail;

// Text of a message's content: a string or [{type: text, text}].
std::string content_text(const Json* c) {
    if (!c) return {};
    if (c->is_string()) return c->as_string();
    std::string out;
    if (c->is_array())
        for (const Json& part : c->as_array())
            if (str(part.find("type")) == "text") out += str(part.find("text"));
    return out;
}

class VibeRunner : public AgentRunner {
public:
    using AgentRunner::AgentRunner;
    std::string id() const override { return "vibe"; }

    std::vector<std::string> command(const RunRequest& r) const override {
        std::vector<std::string> a = {"vibe", "--prompt-file", (r.run_dir / "prompt.md").string(),
                                      "--output", "streaming", "--output-dir", (r.run_dir / "vibe").string(),
                                      "--trust"};
        if (!r.workdir.empty()) a.insert(a.end(), {"--workdir", r.workdir.string()});
        switch (r.permission) {
            case Permission::ReadOnly:
                for (const char* t : {"read_file", "grep", "glob"}) a.insert(a.end(), {"--enabled-tools", t});
                break;
            case Permission::EditFiles: a.insert(a.end(), {"--agent", "accept-edits"}); break;
            case Permission::Full: a.push_back("--auto-approve"); break;
        }
        if (r.resume_session) a.insert(a.end(), {"--resume", *r.resume_session});
        if (r.max_turns > 0) a.insert(a.end(), {"--max-turns", std::to_string(r.max_turns)});
        if (r.max_cost_usd > 0) a.insert(a.end(), {"--max-price", std::to_string(r.max_cost_usd)});
        if (r.timeout_seconds > 0) a.insert(a.end(), {"--time-limit", std::to_string(r.timeout_seconds)});
        a.insert(a.end(), settings_.args.begin(), settings_.args.end());
        return a;
    }

    std::string stdin_text(const RunRequest&) const override { return {}; }

    std::map<std::string, std::string> environment(const RunRequest& r) const override {
        if (r.model.empty()) return {};
        return {{"VIBE_ACTIVE_MODEL", r.model}};
    }

    void parse_line(std::string_view line, StreamState& st, const EventSink& sink) const override {
        auto j = parse_json(line);
        if (!j) return;
        RunResult& res = st.result;
        if (res.session_id.empty()) res.session_id = str(j->find("session_id"));
        const std::string type = str(j->find("type"));
        if (type == "message") {
            std::string role = str(j->find("role"));
            if (role.empty()) role = str(get(*j, {"message", "role"}));
            if (role != "assistant") return;
            const Json* content = j->find("content");
            if (!content) content = get(*j, {"message", "content"});
            std::string text = content_text(content);
            if (text.empty()) return;
            st.last_text = text;
            emit(sink, AgentEvent::Kind::Text, text);
        } else if (type == "effect") {
            std::string name;
            for (const char* k : {"tool", "name", "kind", "effect"}) {
                name = str(j->find(k));
                if (!name.empty()) break;
            }
            if (name.empty()) return;
            const Json* input = j->find("input");
            if (!input) input = j->find("args");
            if (!input) input = j->find("arguments");
            std::string status = str(j->find("status"));
            ToolCall call{name, tool_detail(input), status == "failed" || status == "error"};
            res.tool_calls.push_back(call);
            emit(sink, AgentEvent::Kind::ToolCall, call.detail, call.name);
        } else if (type == "error") {
            std::string msg = str(j->find("message"));
            res.error = msg.empty() ? "vibe reported an error" : msg;
            emit(sink, AgentEvent::Kind::Error, res.error);
        }
    }

    void finish(const RunRequest& r, StreamState& st) const override {
        auto text = try_read_file(r.run_dir / "vibe" / "export.json");
        if (!text) return;
        auto j = parse_json(*text);
        if (!j) return;
        RunResult& res = st.result;
        if (auto s = str(j->find("session_id")); !s.empty()) res.session_id = s;
        res.stop_reason = str(j->find("stop_reason"));
        if (res.model.empty()) res.model = str(j->find("model"));
        if (const Json* u = j->find("usage")) {
            res.usage.input_tokens = num(u->find("input_tokens"));
            res.usage.output_tokens = num(u->find("output_tokens"));
            res.usage.cached_input_tokens = num(u->find("cached_input_tokens"));
        }
        for (const char* k : {"cost", "cost_usd", "total_cost", "price"})
            if (const Json* c = j->find(k); c && c->is_number()) {
                res.usage.cost_usd = c->as_number();
                break;
            }
        if (const Json* steps = j->find("steps"); steps && steps->is_number())
            res.usage.turns = static_cast<int>(steps->as_number());
        const std::string outcome = str(j->find("outcome"));
        if (!outcome.empty() && outcome != "success" && outcome != "completed" && res.error.empty())
            res.error = "vibe stopped: " + outcome + (res.stop_reason.empty() ? "" : " (" + res.stop_reason + ")");
    }
};

}  // namespace

std::unique_ptr<AgentRunner> make_vibe_runner(RunnerSettings settings) {
    return std::make_unique<VibeRunner>(std::move(settings));
}

}  // namespace myteam
