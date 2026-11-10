#include <gtest/gtest.h>

#include <functional>
#include <sstream>

#include "myteam/loop.hpp"
#include "myteam/mcp.hpp"
#include "myteam/util.hpp"
#include "test_util.hpp"

using namespace myteam;
using taskml::Json;
using taskml::TaskStatus;
using myteam::test::TempDir;

namespace {

struct World {
    TempDir dir;
    std::optional<Project> project;
    std::optional<Team> team;
    explicit World(const std::string& plan) {
        dir.write("team/team.md", "---\nid: t\nname: T\nproduct_owner: po\nmodels: {default: claude:sonnet}\n---\n");
        dir.write("team/members/po.md", "---\nname: Po\nrole: product-owner\n---\nPrioritize.\n");
        dir.write("team/members/dev.md", "---\nname: Dev\nrole: developer\n---\nCode.\n");
        dir.write("team/members/qa.md", "---\nname: Qa\nrole: tester\n---\nTest.\n");
        dir.write("proj/PLAN.md", plan);
        project = Project::open(dir / "proj");
        team = load_team(dir / "team");
    }
    McpServer server(const fs::path& report = {}) { return McpServer({*project, *team, "dev", "t", report}); }
    Plan plan() const { return Plan::load(dir / "proj/PLAN.md"); }
};

const char* kPlan =
    "# P\n\n- [ ] Task @dev ^t\n  - A note\n  - ( ) Returns JSON\n  - ( ) Handles errors\n- [ ] Other @qa ^o\n";

std::string text_of(const Json& result) { return result.find("content")->as_array()[0].find("text")->as_string(); }
bool is_error(const Json& result) { return result.find("isError")->as_bool(); }

Json args(std::initializer_list<std::pair<const char*, Json>> kv) {
    Json a = Json::object();
    for (const auto& [k, v] : kv) a.set(k, v);
    return a;
}

}  // namespace

TEST(PlanEditorMore, SetCriterion) {
    TempDir d;
    d.write("PLAN.md", kPlan);
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    ed.set_criterion(*p.by_id("t"), 0, taskml::CriterionStatus::Verified, "curl /api returns JSON");
    ed.set_criterion(*p.by_id("t"), 1, taskml::CriterionStatus::Failed);
    ASSERT_TRUE(ed.commit().ok);
    std::string t = d.read("PLAN.md");
    EXPECT_NE(t.find("  - (x) Returns JSON\n    - evidence: \"curl /api returns JSON\"\n  - (!) Handles errors\n"),
              std::string::npos)
        << t;
    // Again: the evidence line is replaced, not added.
    Plan q = Plan::load(d / "PLAN.md");
    PlanEditor ed2(q);
    ed2.set_criterion(*q.by_id("t"), 0, taskml::CriterionStatus::Verified, "the \"tests\" pass");
    ASSERT_TRUE(ed2.commit().ok);
    Plan r = Plan::load(d / "PLAN.md");
    ASSERT_EQ(r.by_id("t")->task->criteria.size(), 2u);
    EXPECT_EQ(*r.by_id("t")->task->criteria[0].evidence, "the 'tests' pass");
}

TEST(Report, CriterionBlocks) {
    AgentReport r = parse_report(
        "<criterion n=\"2\" status=\"verified\">pytest -k errors</criterion><criterion n=\"x\">bad</criterion>"
        "<criterion n=\"1\" status=\"failed\"></criterion><criterion n=\"3\" status=\"maybe\">?</criterion>");
    ASSERT_EQ(r.criteria.size(), 2u);
    EXPECT_EQ(r.criteria[0].number, 2);
    EXPECT_EQ(r.criteria[0].evidence, "pytest -k errors");
    EXPECT_EQ(r.criteria[1].status, taskml::CriterionStatus::Failed);
}

TEST(Mcp, Protocol) {
    World w(kPlan);
    McpServer s = w.server();
    std::istringstream in(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":"two","method":"tools/list"})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"no/such"})"
        "\n"
        "not json\n"
        R"({"jsonrpc":"2.0","id":4,"method":"ping"})"
        "\n");
    std::ostringstream out;
    s.serve(in, out);
    auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 5u) << out.str();  // no answer to the notification
    Json init = Json::parse(lines[0]);
    EXPECT_EQ(init.find("result")->find("protocolVersion")->as_string(), "2025-03-26");
    EXPECT_EQ(init.find("result")->find("serverInfo")->find("name")->as_string(), "myteam");
    Json list = Json::parse(lines[1]);
    EXPECT_EQ(list.find("id")->as_string(), "two");
    const auto& tools = list.find("result")->find("tools")->as_array();
    EXPECT_EQ(tools.size(), 11u);
    EXPECT_EQ(tools[0].find("name")->as_string(), "plan_get_task");
    EXPECT_TRUE(tools[0].find("annotations")->find("readOnlyHint")->as_bool());
    EXPECT_FALSE(tools[3].find("annotations")->find("readOnlyHint")->as_bool());
    EXPECT_EQ(Json::parse(lines[2]).find("error")->find("code")->as_number(), -32601);
    EXPECT_EQ(Json::parse(lines[3]).find("error")->find("code")->as_number(), -32700);
    EXPECT_TRUE(Json::parse(lines[4]).find("result")->is_object());
}

TEST(Mcp, ReadingTools) {
    World w(kPlan);
    McpServer s = w.server();
    Json t = s.call("plan_get_task", Json::object());
    EXPECT_FALSE(is_error(t));
    EXPECT_NE(text_of(t).find("1. ( ) Returns JSON"), std::string::npos);
    EXPECT_NE(text_of(s.call("plan_get_task", args({{"task", "^o"}}))).find("**Other**"), std::string::npos);
    EXPECT_TRUE(is_error(s.call("plan_get_task", args({{"task", "^zz"}}))));
    // No listing of the whole plan: an agent's task is in its prompt.
    EXPECT_TRUE(is_error(s.call("plan_list_tasks", Json::object())));
    EXPECT_NE(text_of(s.call("plan_team", Json::object())).find("- @qa - Qa, tester"), std::string::npos);
    EXPECT_TRUE(is_error(s.call("plan_nothing", Json::object())));
}

TEST(Mcp, ReportingToolsCheckAndRecord) {
    World w(kPlan);
    fs::path run = w.dir / "run";
    {
        McpServer s = w.server(run / kMcpReportFile);
        EXPECT_TRUE(is_error(s.call("plan_report_status", args({{"status", "finished?"}, {"summary", "x"}}))));
        EXPECT_TRUE(is_error(s.call("plan_handoff", args({{"to", "ghost"}, {"instructions", "x"}}))));
        EXPECT_NE(text_of(s.call("plan_handoff", args({{"to", "ghost"}, {"instructions", "x"}}))).find("@qa"),
                  std::string::npos);
        EXPECT_TRUE(is_error(s.call("plan_handoff", args({{"to", "dev"}, {"instructions", "x"}}))));
        // Without an acceptance criterion: refused, saying how.
        Json none = s.call("plan_handoff", args({{"to", "@QA"}, {"instructions", "Test the API\nAll endpoints."}}));
        EXPECT_TRUE(is_error(none));
        EXPECT_NE(text_of(none).find("accept:"), std::string::npos);
        EXPECT_FALSE(is_error(s.call("plan_handoff", args({{"to", "@QA"}, {"instructions", "Test the API\nAll endpoints.\naccept: every endpoint answers"}}))));
        EXPECT_TRUE(is_error(s.call("plan_report_blocker", args({{"on", "^nope"}, {"text", "x"}}))));
        EXPECT_TRUE(is_error(s.call("plan_report_blocker", args({{"on", "^o"}, {"to", "qa"}, {"text", "x"}}))));
        EXPECT_TRUE(is_error(s.call("plan_report_blocker", args({{"on", "^t"}, {"text", "x"}}))));
        EXPECT_FALSE(is_error(s.call("plan_report_blocker", args({{"to", "operator"}, {"text", "The API key"}}))));
        EXPECT_TRUE(is_error(s.call("plan_check_criterion", args({{"number", 3}, {"status", "verified"}}))));
        EXPECT_FALSE(is_error(s.call("plan_check_criterion", args({{"number", 1}, {"status", "failed"}}))));
        EXPECT_FALSE(is_error(s.call("plan_check_criterion", args({{"number", 1}, {"status", "verified"}, {"evidence", "curl"}}))));
        // Skipped: it could not be checked yet; it needs the reason, and stays open.
        EXPECT_TRUE(is_error(s.call("plan_check_criterion", args({{"number", 2}, {"status", "pending"}}))));
        EXPECT_TRUE(is_error(s.call("plan_check_criterion", args({{"number", 2}, {"status", "skipped"}}))));
        Json sk = s.call("plan_check_criterion", args({{"number", 2}, {"status", "skipped"}, {"evidence", "no errors yet"}}));
        EXPECT_FALSE(is_error(sk));
        EXPECT_NE(text_of(sk).find("skipped"), std::string::npos);
        EXPECT_FALSE(is_error(s.call("plan_report_org_issue", args({{"area", "tools"}, {"text", "No linter"}}))));
        EXPECT_TRUE(is_error(s.call("plan_report_finding", args({{"text", " "}}))));
        EXPECT_FALSE(is_error(s.call("plan_report_finding", args({{"text", "linter has no OPENAPI view"}}))));
        EXPECT_FALSE(is_error(s.call("plan_report_finding", args({{"text", "linter has no OPENAPI view"}}))));
        // Review names its reviewer, who is not the author.
        Json nr = s.call("plan_report_status", args({{"status", "review"}, {"summary", "x"}}));
        EXPECT_TRUE(is_error(nr));
        EXPECT_NE(text_of(nr).find("reviewer"), std::string::npos);
        EXPECT_TRUE(is_error(s.call("plan_report_status", args({{"status", "review"}, {"summary", "x"}, {"reviewer", "@Dev"}}))));
        EXPECT_TRUE(is_error(s.call("plan_report_status", args({{"status", "review"}, {"summary", "x"}, {"reviewer", "ghost"}}))));
        Json rv = s.call("plan_report_status", args({{"status", "review"}, {"summary", "x"}, {"reviewer", "qa"}}));
        EXPECT_FALSE(is_error(rv));
        EXPECT_NE(text_of(rv).find("reviewer @qa"), std::string::npos);
        EXPECT_EQ(load_mcp_report(run).reviewer, "qa");
        Json st = s.call("plan_report_status", args({{"status", "blocked"}, {"summary", "Waiting for the key"}}));
        EXPECT_FALSE(is_error(st));
    }
    AgentReport r = load_mcp_report(run);
    EXPECT_EQ(*r.status, TaskStatus::Blocked);
    EXPECT_EQ(r.reviewer, "");  // a later status that is not review drops it
    EXPECT_EQ(r.summary, "Waiting for the key");
    ASSERT_EQ(r.handoffs.size(), 1u);
    EXPECT_EQ(r.handoffs[0].to, "qa");
    ASSERT_EQ(r.blockers.size(), 1u);
    EXPECT_EQ(r.blockers[0].kind, BlockerReport::Kind::Operator);
    ASSERT_EQ(r.criteria.size(), 2u);  // the later check of 1 replaced the first
    EXPECT_EQ(r.criteria[0].status, taskml::CriterionStatus::Verified);
    EXPECT_EQ(r.criteria[0].evidence, "curl");
    EXPECT_EQ(r.criteria[1].status, taskml::CriterionStatus::Pending);
    EXPECT_EQ(r.criteria[1].evidence, "skipped: no errors yet");
    EXPECT_EQ(r.org_issues.size(), 1u);
    EXPECT_EQ(r.findings, std::vector<std::string>{"linter has no OPENAPI view"});  // once
    // A new server for the same run keeps what was recorded.
    McpServer again = w.server(run / kMcpReportFile);
    EXPECT_EQ(again.report().handoffs.size(), 1u);
}

TEST(Mcp, MergeReports) {
    AgentReport text = parse_report(
        "<task_status>done</task_status><work_summary>Text</work_summary>"
        "<handoff to=\"qa\">Test the API</handoff><criterion n=\"1\" status=\"failed\">x</criterion>");
    AgentReport tools;
    tools.status = TaskStatus::Review;
    tools.status_text = "review";
    tools.handoffs.push_back({"qa", "Test the API\nmore"});
    tools.handoffs.push_back({"po", "Decide"});
    tools.criteria.push_back({1, taskml::CriterionStatus::Verified, "curl"});
    merge_reports(text, tools);
    EXPECT_EQ(*text.status, TaskStatus::Review);
    EXPECT_EQ(text.summary, "Text");
    EXPECT_EQ(text.handoffs.size(), 2u);
    ASSERT_EQ(text.criteria.size(), 1u);
    EXPECT_EQ(text.criteria[0].status, taskml::CriterionStatus::Verified);
}

// One escalation, in a tool call and again in the final
// message, worded a little differently, is one escalation.
TEST(Mcp, AnEscalationWordedTwiceInOneRunMergesOnce) {
    AgentReport text = parse_report(
        "<blocker to=\"operator\">Linter cannot skip a rule for one generated file</blocker>"
        "<finding>linter: a nested config cannot have a numeric key (TypeError)</finding>");
    AgentReport tools;
    tools.blockers.push_back({BlockerReport::Kind::Operator, "", "", "linter cannot skip a rule for one generated file (no we/wel)"});
    tools.blockers.push_back({BlockerReport::Kind::Operator, "", "", "The container lacks cpio"});
    tools.findings.push_back("linter: nested config cannot have a numeric key");
    merge_reports(text, tools);
    EXPECT_EQ(text.blockers.size(), 2u);
    EXPECT_EQ(text.findings.size(), 1u);
}

TEST(Mcp, ADuplicateEscalationIsRefusedPointingToTheOpenOne) {
    World w("# P\n\n- [~] Task @dev ^t\n"
            "- [ ] Apply the audio-noc README addition in ../interfaces/audio-noc/README.md @operator ^apply-readme\n"
            "- [x] The container lacks cpio @operator ^cpio\n");
    McpServer s = w.server(w.dir / "run" / kMcpReportFile);
    Json b = s.call("plan_report_blocker",
                    args({{"to", "operator"}, {"text", "Apply the audio-noc README addition in ../interfaces/audio-noc/README.md "
                                                       "(an Interface spec section)\nThe guard refuses my writes."}}));
    EXPECT_TRUE(is_error(b));
    EXPECT_NE(text_of(b).find("^apply-readme"), std::string::npos) << text_of(b);
    EXPECT_NE(text_of(b).find("on=\"^apply-readme\""), std::string::npos);
    Json f = s.call("plan_report_finding", args({{"text", "The audio-noc README addition in interfaces/audio-noc/README.md is not applied"}}));
    EXPECT_TRUE(is_error(f));
    EXPECT_NE(text_of(f).find("^apply-readme"), std::string::npos);
    // Waiting for the open one is the way.
    EXPECT_FALSE(is_error(s.call("plan_report_blocker", args({{"on", "^apply-readme"}, {"text", "the README"}}))));
    // A finished one is no reason to refuse; a new matter is recorded.
    EXPECT_FALSE(is_error(s.call("plan_report_blocker", args({{"to", "operator"}, {"text", "The container lacks cpio"}}))));
    // The same in this run again: exact is a no-op, reworded is refused.
    EXPECT_FALSE(is_error(s.call("plan_report_blocker", args({{"to", "operator"}, {"text", "The container lacks cpio"}}))));
    EXPECT_TRUE(is_error(s.call("plan_report_blocker", args({{"to", "operator"}, {"text", "cpio lacks in the container"}}))));
    AgentReport r = load_mcp_report(w.dir / "run");
    EXPECT_EQ(r.blockers.size(), 2u);
    EXPECT_TRUE(r.findings.empty());
}

TEST(Mcp, RunnersGetTheServer) {
    RunRequest r;
    r.prompt = "x";
    r.run_dir = "/tmp/run";
    r.mcp = McpServerSpec{"myteam", {"/usr/bin/myteam", "mcp", "--task", "t"}};
    auto claude = make_runner("claude")->command(r);
    auto it = std::find(claude.begin(), claude.end(), "--mcp-config");
    ASSERT_NE(it, claude.end());
    Json cfg = Json::parse(*(it + 1));
    EXPECT_EQ(cfg.find("mcpServers")->find("myteam")->find("command")->as_string(), "/usr/bin/myteam");
    EXPECT_EQ(cfg.find("mcpServers")->find("myteam")->find("args")->as_array().size(), 3u);
    auto allowed = std::find(claude.begin(), claude.end(), "--allowedTools");
    ASSERT_NE(allowed, claude.end());
    EXPECT_EQ(*(allowed + 1), "mcp__myteam,Bash");  // edit permission: commands too
    auto codex = make_runner("codex")->command(r);
    EXPECT_NE(std::find(codex.begin(), codex.end(), "mcp_servers.myteam.command=\"/usr/bin/myteam\""), codex.end());
    EXPECT_NE(std::find(codex.begin(), codex.end(), "mcp_servers.myteam.args=[\"mcp\",\"--task\",\"t\"]"), codex.end());
    EXPECT_FALSE(make_runner("vibe")->supports_mcp());
}

namespace {

// A claude that "calls" the tools: it serves its own MCP session.
class ToolUsingRunner : public AgentRunner {
public:
    ToolUsingRunner(World* w, std::vector<RunRequest>* log) : AgentRunner({}), w_(w), log_(log) {}
    std::string id() const override { return "claude"; }
    std::string start_problem() const override { return ""; }  // a fake: it can start
    bool supports_mcp() const override { return true; }
    std::vector<std::string> command(const RunRequest&) const override { return {"fake"}; }
    void parse_line(std::string_view, StreamState&, const EventSink&) const override {}
    RunResult run(const RunRequest& req, const EventSink&) override {
        log_->push_back(req);
        fs::create_directories(req.run_dir);
        RunResult r;
        r.ok = true;
        if (!req.mcp) {
            r.final_text = "<task_status>done</task_status>";
            return r;
        }
        const auto& c = req.mcp->command;
        auto value = [&](const char* flag) {
            auto it = std::find(c.begin(), c.end(), flag);
            return it != c.end() && it + 1 != c.end() ? *(it + 1) : std::string();
        };
        McpServer s({*w_->project, *w_->team, value("--member"), value("--task"),
                     fs::path(value("--run-dir")) / kMcpReportFile});
        s.call("plan_check_criterion", args({{"number", 2}, {"status", "verified"}, {"evidence", "pytest"}}));
        s.call("plan_check_criterion", args({{"number", 1}, {"status", "skipped"}, {"evidence", "no client yet"}}));
        s.call("plan_report_finding", args({{"text", "The linter has no C++20 mode\nWorked around with -std=c++17 checks."}}));
        s.call("plan_handoff", args({{"to", "qa"}, {"instructions", "Test the error paths\naccept: each error has a test"}}));
        s.call("plan_report_status", args({{"status", "review"}, {"summary", "Done through the tools."}, {"reviewer", "qa"}}));
        r.final_text = "All done.";  // no blocks at all
        return r;
    }

private:
    World* w_;
    std::vector<RunRequest>* log_;
};

}  // namespace

TEST(Mcp, LoopAppliesWhatTheToolsRecorded) {
    World w(kPlan);
    std::vector<RunRequest> runs;
    Loop loop(*w.project, *w.team, [&](const std::string&) { return std::make_unique<ToolUsingRunner>(&w, &runs); });
    LoopOptions o;
    o.only_task = "^t";
    o.mcp_command = {"/usr/bin/myteam", "mcp"};
    IterationResult r = loop.iterate(o);
    ASSERT_EQ(runs.size(), 1u);
    ASSERT_TRUE(runs[0].mcp.has_value());
    const auto& cmd = runs[0].mcp->command;
    EXPECT_EQ(cmd[0], "/usr/bin/myteam");
    EXPECT_NE(std::find(cmd.begin(), cmd.end(), "dev"), cmd.end());
    EXPECT_NE(runs[0].prompt.find("You also have the myteam tools"), std::string::npos);
    ASSERT_EQ(r.dispatches.size(), 1u);
    EXPECT_EQ(r.dispatches[0].failure, "none");
    Plan p = w.plan();
    EXPECT_EQ(p.by_id("t")->task->status, TaskStatus::Review);
    EXPECT_EQ(p.by_id("t")->task->criteria[1].status, taskml::CriterionStatus::Verified);
    // The skipped one stays open in the plan, with its reason as evidence.
    EXPECT_EQ(p.by_id("t")->task->criteria[0].status, taskml::CriterionStatus::Pending);
    EXPECT_NE(w.dir.read("proj/PLAN.md").find("( ) Returns JSON\n    - evidence: \"skipped: no client yet\""), std::string::npos);
    EXPECT_TRUE(p.by_id("test-the-error-paths"));
    // The finding is a task for the operator; the reporter's task is not blocked by it.
    const TaskRef* f = nullptr;
    for (const TaskRef& x : p.tasks())
        if (x.task->description == "The linter has no C++20 mode") f = &x;
    ASSERT_TRUE(f) << w.dir.read("proj/PLAN.md");
    EXPECT_EQ(*f->task->assignee, "operator");
    EXPECT_NE(std::find(f->task->tags.begin(), f->task->tags.end(), "finding"), f->task->tags.end());
    EXPECT_TRUE(p.by_id("t")->task->blocked_by.empty());

    // Without an MCP command, no tools and no mention of them.
    World w2(kPlan);
    std::vector<RunRequest> runs2;
    Loop loop2(*w2.project, *w2.team, [&](const std::string&) { return std::make_unique<ToolUsingRunner>(&w2, &runs2); });
    LoopOptions o2;
    loop2.iterate(o2);
    ASSERT_FALSE(runs2.empty());
    EXPECT_FALSE(runs2[0].mcp.has_value());
    EXPECT_EQ(runs2[0].prompt.find("myteam tools"), std::string::npos);
}

TEST(Runners, CommandsAllowedWithEditPermission) {
    RunRequest r;
    r.prompt = "x";
    r.run_dir = "/tmp/run";
    auto args_of = [](const std::vector<std::string>& a) {
        auto it = std::find(a.begin(), a.end(), "--allowedTools");
        return it == a.end() ? std::string("-") : *(it + 1);
    };
    EXPECT_EQ(args_of(make_runner("claude")->command(r)), "Bash");
    r.commands = {"pytest:*", "python3 -m pytest:*"};
    EXPECT_EQ(args_of(make_runner("claude")->command(r)), "Bash(pytest:*),Bash(python3 -m pytest:*)");
    r.commands = {};
    EXPECT_EQ(args_of(make_runner("claude")->command(r)), "-");
    r.commands = {"all"};
    r.permission = Permission::ReadOnly;
    EXPECT_EQ(args_of(make_runner("claude")->command(r)), "-");
}

TEST(Mcp, GetTaskTakesIdToo) {
    World w(kPlan);
    McpServer s = w.server();
    std::string t = text_of(s.call("plan_get_task", args({{"id", "^o"}})));
    EXPECT_NE(t.find("Other"), std::string::npos) << t;
}

// ^notes-said-the-task-for, ^the-task-notes-say-for, ^cancelling-an-org-task: plan_handoff
// takes another team's handle and @operator, and says the sender's task does not wait.
TEST(Mcp, HandoffNamesAUnitOrTheOperator) {
    World w(kPlan);
    w.team->external["web"] = "the web front end";
    fs::path run = w.dir / "run";
    McpServer s = w.server(run / kMcpReportFile);
    Json unit = s.call("plan_handoff", args({{"to", "@web"}, {"instructions", "Freeze the link\naccept: it is frozen"}}));
    ASSERT_FALSE(is_error(unit)) << text_of(unit);
    EXPECT_NE(text_of(unit).find("yours does not wait"), std::string::npos);
    Json op = s.call("plan_handoff", args({{"to", "operator"}, {"instructions", "Commit ORG.md\naccept: committed"}}));
    ASSERT_FALSE(is_error(op)) << text_of(op);
    // An unknown handle names the teams it could be.
    std::string ghost = text_of(s.call("plan_handoff", args({{"to", "ghost"}, {"instructions", "x\naccept: y"}})));
    EXPECT_NE(ghost.find("@web"), std::string::npos) << ghost;
    EXPECT_NE(ghost.find("@operator"), std::string::npos) << ghost;
    // A teammate's handoff still waits, and says nothing about it.
    Json qa = s.call("plan_handoff", args({{"to", "qa"}, {"instructions", "Test the API\naccept: it answers"}}));
    EXPECT_EQ(text_of(qa).find("does not wait"), std::string::npos);
    AgentReport r = report_from_json(Json::parse(w.dir.read("run/" + std::string(kMcpReportFile))));
    ASSERT_EQ(r.handoffs.size(), 3u);
    EXPECT_EQ(r.handoffs[0].to, "web");
    EXPECT_EQ(r.handoffs[1].to, "operator");
}
