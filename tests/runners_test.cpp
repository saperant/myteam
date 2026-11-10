#include <gtest/gtest.h>

#include <algorithm>
#include <sys/stat.h>

#include "myteam/process.hpp"
#include "myteam/project.hpp"
#include "myteam/runner.hpp"
#include "test_util.hpp"

using namespace myteam;
using myteam::test::TempDir;

namespace {

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool has_pair(const std::vector<std::string>& v, const std::string& a, const std::string& b) {
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i] == a && v[i + 1] == b) return true;
    return false;
}

RunRequest request(const TempDir& d) {
    RunRequest r;
    r.member = "builder";
    r.model = "m1";
    r.prompt = "Do the thing.";
    r.workdir = d.path();
    r.run_dir = d / "run";
    return r;
}

std::vector<AgentEvent> feed(const AgentRunner& r, StreamState& st, const std::vector<std::string>& lines) {
    std::vector<AgentEvent> events;
    EventSink sink = [&](const AgentEvent& e) { events.push_back(e); };
    for (const auto& l : lines) r.parse_line(l, st, sink);
    return events;
}

void make_script(const TempDir& d, const std::string& name, const std::string& body) {
    d.write(name, "#!/bin/sh\n" + body);
    ::chmod((d / name).c_str(), 0755);
}

}  // namespace

TEST(Process, LinesStdinExitAndTimeout) {
    std::vector<std::string> lines;
    ProcessOptions o;
    o.argv = {"/bin/sh", "-c", "cat; echo second; printf 'no newline'; exit 3"};
    o.stdin_text = std::string(200000, 'x') + "\n";  // larger than a pipe buffer
    ProcessResult r = run_process(o, [&](std::string_view l) { lines.emplace_back(l); });
    EXPECT_EQ(r.exit_code, 3);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0].size(), 200000u);
    EXPECT_EQ(lines[2], "no newline");

    ProcessOptions slow;
    slow.argv = {"/bin/sh", "-c", "sleep 30"};
    slow.timeout_seconds = 1;
    ProcessResult t = run_process(slow, {});
    EXPECT_TRUE(t.timed_out);

    ProcessOptions missing;
    missing.argv = {"/nonexistent/agent"};
    ProcessResult m = run_process(missing, {});
    EXPECT_EQ(m.exit_code, -1);
    EXPECT_FALSE(m.error.empty());
}

TEST(Claude, CommandAndStream) {
    TempDir d;
    RunnerSettings s;
    s.isolate = true;
    s.args = {"--extra"};
    auto r = make_runner("claude", s);
    RunRequest req = request(d);
    req.permission = Permission::ReadOnly;
    req.resume_session = "sess-1";
    auto cmd = r->command(req);
    EXPECT_TRUE(has_pair(cmd, "--output-format", "stream-json"));
    EXPECT_TRUE(has_pair(cmd, "--model", "m1"));
    EXPECT_TRUE(has_pair(cmd, "--permission-mode", "plan"));
    EXPECT_TRUE(has_pair(cmd, "--resume", "sess-1"));
    EXPECT_TRUE(has(cmd, "--bare"));
    EXPECT_EQ(cmd.back(), "--extra");
    EXPECT_EQ(r->stdin_text(req), "Do the thing.");

    StreamState st;
    auto ev = feed(*r, st, {
        R"({"type":"system","subtype":"init","session_id":"abc","model":"claude-sonnet"})",
        "not json at all",
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"Looking."},{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"make test"}}]}})",
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","is_error":true}]}})",
        R"({"type":"result","subtype":"success","result":"All done <task_status>done</task_status>","session_id":"abc","total_cost_usd":0.25,"num_turns":4,"is_error":false,"usage":{"input_tokens":1200,"output_tokens":300,"cache_read_input_tokens":800}})",
    });
    const RunResult& res = st.result;
    EXPECT_EQ(res.session_id, "abc");
    EXPECT_EQ(res.model, "claude-sonnet");
    ASSERT_EQ(res.tool_calls.size(), 1u);
    EXPECT_EQ(res.tool_calls[0].name, "Bash");
    EXPECT_EQ(res.tool_calls[0].detail, "make test");
    EXPECT_TRUE(res.tool_calls[0].failed);
    EXPECT_EQ(res.final_text, "All done <task_status>done</task_status>");
    EXPECT_DOUBLE_EQ(res.usage.cost_usd, 0.25);
    EXPECT_EQ(res.usage.turns, 4);
    EXPECT_EQ(res.usage.cached_input_tokens, 800);
    EXPECT_TRUE(res.error.empty());
    EXPECT_GE(ev.size(), 3u);
}

TEST(Codex, CommandAndStream) {
    TempDir d;
    auto r = make_runner("codex");
    RunRequest req = request(d);
    req.permission = Permission::EditFiles;
    auto cmd = r->command(req);
    EXPECT_EQ(cmd[1], "exec");
    EXPECT_TRUE(has(cmd, "--json"));
    EXPECT_TRUE(has_pair(cmd, "--sandbox", "workspace-write"));
    EXPECT_TRUE(has_pair(cmd, "--model", "m1"));
    EXPECT_EQ(cmd.back(), "-");
    req.resume_session = "th-9";
    auto resumed = r->command(req);
    EXPECT_TRUE(has_pair(resumed, "resume", "th-9"));
    req.permission = Permission::Full;
    EXPECT_TRUE(has(r->command(req), "--dangerously-bypass-approvals-and-sandbox"));

    StreamState st;
    feed(*r, st, {
        R"({"type":"thread.started","thread_id":"th-1"})",
        R"({"type":"turn.started"})",
        R"({"type":"item.started","item":{"id":"i1","type":"command_execution","command":"bash -lc ls","status":"in_progress"}})",
        R"({"type":"item.completed","item":{"id":"i1","type":"command_execution","command":"bash -lc ls","exit_code":0,"status":"completed"}})",
        R"({"type":"item.completed","item":{"id":"i2","type":"file_change","changes":[{"path":"a.cpp","kind":"update"}],"status":"completed"}})",
        R"({"type":"item.completed","item":{"id":"i3","type":"agent_message","text":"Done. <task_status>done</task_status>"}})",
        R"({"type":"turn.completed","usage":{"input_tokens":100,"cached_input_tokens":50,"output_tokens":20}})",
    });
    const RunResult& res = st.result;
    EXPECT_EQ(res.session_id, "th-1");
    ASSERT_EQ(res.tool_calls.size(), 2u);
    EXPECT_EQ(res.tool_calls[0].detail, "bash -lc ls");
    EXPECT_FALSE(res.tool_calls[0].failed);
    EXPECT_EQ(res.tool_calls[1].detail, "a.cpp");
    EXPECT_EQ(st.last_text, "Done. <task_status>done</task_status>");
    EXPECT_EQ(res.usage.turns, 1);
    EXPECT_EQ(res.usage.input_tokens, 100);

    StreamState failed;
    feed(*r, failed, {R"({"type":"turn.failed","error":{"message":"quota"}})"});
    EXPECT_EQ(failed.result.error, "quota");
}

TEST(Vibe, CommandEnvAndExport) {
    TempDir d;
    auto r = make_runner("vibe");
    RunRequest req = request(d);
    req.permission = Permission::ReadOnly;
    req.max_turns = 7;
    req.timeout_seconds = 60;
    auto cmd = r->command(req);
    EXPECT_TRUE(has_pair(cmd, "--prompt-file", (req.run_dir / "prompt.md").string()));
    EXPECT_TRUE(has_pair(cmd, "--output", "streaming"));
    EXPECT_TRUE(has_pair(cmd, "--enabled-tools", "read_file"));
    EXPECT_TRUE(has_pair(cmd, "--max-turns", "7"));
    EXPECT_TRUE(has_pair(cmd, "--time-limit", "60"));
    EXPECT_EQ(r->stdin_text(req), "");
    EXPECT_EQ(r->environment(req).at("VIBE_ACTIVE_MODEL"), "m1");

    StreamState st;
    feed(*r, st, {
        R"({"type":"message","role":"assistant","content":[{"type":"text","text":"Working on it"}]})",
        R"({"type":"effect","tool":"bash","input":{"command":"ls"}})",
        R"({"type":"message","role":"user","content":"ignored"})",
    });
    EXPECT_EQ(st.last_text, "Working on it");
    ASSERT_EQ(st.result.tool_calls.size(), 1u);
    d.write("run/vibe/export.json",
            R"({"session_id":"v-1","stop_reason":"end_turn","outcome":"success","usage":{"input_tokens":10,"output_tokens":5},"cost":0.01})");
    r->finish(req, st);
    EXPECT_EQ(st.result.session_id, "v-1");
    EXPECT_EQ(st.result.usage.output_tokens, 5);
    EXPECT_DOUBLE_EQ(st.result.usage.cost_usd, 0.01);
    EXPECT_TRUE(st.result.error.empty());
}

TEST(Runner, RunsAFakeClaude) {
    TempDir d;
    // A stand-in for the claude CLI: reads the prompt, answers in stream-json.
    make_script(d, "fake-claude",
                "read first\n"
                "echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"s-42\"}'\n"
                "echo \"{\\\"type\\\":\\\"result\\\",\\\"subtype\\\":\\\"success\\\",\\\"result\\\":\\\"got: $first\\\",\\\"num_turns\\\":1}\"\n"
                "echo 'to stderr' >&2\n");
    RunnerSettings s;
    s.command = (d / "fake-claude").string();
    auto r = make_runner("claude", s);
    EXPECT_TRUE(r->available());
    std::vector<AgentEvent::Kind> kinds;
    RunResult res = r->run(request(d), [&](const AgentEvent& e) { kinds.push_back(e.kind); });
    EXPECT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.final_text, "got: Do the thing.");
    EXPECT_EQ(res.session_id, "s-42");
    EXPECT_EQ(d.read("run/prompt.md"), "Do the thing.");
    EXPECT_NE(d.read("run/stream.jsonl").find("s-42"), std::string::npos);
    EXPECT_EQ(d.read("run/stderr.log"), "to stderr\n");
    EXPECT_EQ(kinds.front(), AgentEvent::Kind::Started);
    EXPECT_EQ(kinds.back(), AgentEvent::Kind::Finished);

    make_script(d, "failing", "echo 'auth failed' >&2\nexit 2\n");
    s.command = (d / "failing").string();
    RunResult bad = make_runner("claude", s)->run(request(d));
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.error.find("auth failed"), std::string::npos);
}

TEST(Runner, Registry) {
    EXPECT_NE(make_runner("claude"), nullptr);
    EXPECT_NE(make_runner("codex"), nullptr);
    EXPECT_NE(make_runner("vibe"), nullptr);
    EXPECT_EQ(make_runner("gpt"), nullptr);
    EXPECT_EQ(*permission_from_string("read-only"), Permission::ReadOnly);
    EXPECT_FALSE(permission_from_string("sometimes"));
}

TEST(Runner, StartProblemFindsAMissingInterpreter) {
    TempDir d;
    auto script = [&](const std::string& name, const std::string& text) {
        d.write(name, text);
        ::chmod((d / name).c_str(), 0755);
        return (d / name).string();
    };
    // A uv tool's launcher whose venv python points at an interpreter this system lacks.
    fs::create_directories(d / "venv/bin");
    fs::create_symlink("/nonexistent/python3.14", d / "venv/bin/python");
    auto vibe = make_runner("vibe", {script("vibe", "#!" + (d / "venv/bin/python").string() + "\nimport x\n")});
    std::string why = vibe->start_problem();
    EXPECT_NE(why.find("its interpreter"), std::string::npos) << why;
    EXPECT_NE(why.find("/nonexistent/python3.14 is missing"), std::string::npos) << why;
    EXPECT_NE(why.find("runners.vibe.command"), std::string::npos) << why;
    // The run fails at once, as not available (so the loop falls back), and says why.
    RunRequest req;
    req.prompt = "x";
    req.run_dir = d / "run";
    RunResult r = vibe->run(req);
    EXPECT_EQ(r.failure, Failure::NotAvailable);
    EXPECT_EQ(r.error, why);

    auto env_missing = make_runner("codex", {script("codex", "#!/usr/bin/env -S no-such-node --flag\n")});
    EXPECT_NE(env_missing->start_problem().find("no-such-node, which is not on $PATH"), std::string::npos);
    EXPECT_EQ(make_runner("claude", {script("claude", "#!/bin/sh\necho hi\n")})->start_problem(), "");
    EXPECT_EQ(make_runner("claude", {script("bin", "\x7f" "ELF")})->start_problem(), "");
    EXPECT_NE(make_runner("claude", {"no-such-cli-here"})->start_problem().find("not installed"), std::string::npos);
}

TEST(Runner, ClaudeMayReadOnlyTheProjectsWebDomains) {
    RunRequest r;
    r.prompt = "x";
    r.permission = Permission::ReadOnly;
    r.web_domains = {"example.org", "eur-lex.europa.eu"};
    auto a = make_runner("claude")->command(r);
    auto it = std::find(a.begin(), a.end(), "--allowedTools");
    ASSERT_NE(it, a.end());
    EXPECT_EQ(*(it + 1), "WebFetch(domain:example.org),WebFetch(domain:www.example.org),"
                         "WebFetch(domain:eur-lex.europa.eu),WebFetch(domain:www.eur-lex.europa.eu)");
    r.web_domains.clear();
    auto none = make_runner("claude")->command(r);
    EXPECT_EQ(std::find(none.begin(), none.end(), "--allowedTools"), none.end());
}

TEST(Runner, InTheSandboxClaudeMayReadItsOwnTmp) {
    RunRequest r;
    r.prompt = "x";
    r.permission = Permission::ReadOnly;
    auto plain = make_runner("claude")->command(r);
    EXPECT_EQ(std::find(plain.begin(), plain.end(), "--allowedTools"), plain.end());  // the real /tmp: no
    r.sandbox = {"/usr/bin/bwrap", "--"};
    auto a = make_runner("claude")->command(r);
    auto it = std::find(a.begin(), a.end(), "--allowedTools");
    ASSERT_NE(it, a.end());
    EXPECT_EQ(*(it + 1), "Read(//tmp/**)");
}

TEST(Runner, ClaudeMayReadTheProjectsReadableDirectories) {
    TempDir d;
    d.write("org/interfaces/schema/README.md", "# Schema\n");
    d.write("org/api/.myteam/config.yaml", "team: t\nreadable: [\"../interfaces\", \"../missing\"]\n");
    Project p = Project::open(d / "org" / "api");
    const auto paths = p.read_paths();
    ASSERT_EQ(paths.size(), 1u);  // what is not there is left out
    EXPECT_EQ(fs::path(paths[0]), fs::weakly_canonical(d / "org" / "interfaces"));
    RunRequest r;
    r.prompt = "x";
    r.permission = Permission::ReadOnly;
    r.read_paths = paths;
    auto a = make_runner("claude")->command(r);
    auto it = std::find(a.begin(), a.end(), "--allowedTools");
    ASSERT_NE(it, a.end());
    EXPECT_EQ(*(it + 1), "Read(/" + paths[0] + "/**)");
    EXPECT_EQ((it + 1)->rfind("Read(//", 0), 0u);  // an absolute path's rule

    // A directory it writes: Edit and Write rules too, with edit permission only.
    r.write_paths = paths;
    r.permission = Permission::EditFiles;
    auto w = make_runner("claude")->command(r);
    auto wi = std::find(w.begin(), w.end(), "--allowedTools");
    ASSERT_NE(wi, w.end());
    EXPECT_NE((wi + 1)->find("Edit(/" + paths[0] + "/**),Write(/" + paths[0] + "/**)"), std::string::npos) << *(wi + 1);
    r.permission = Permission::ReadOnly;
    auto ro = make_runner("claude")->command(r);
    EXPECT_EQ((std::find(ro.begin(), ro.end(), "--allowedTools") + 1)->find("Edit("), std::string::npos);
}
