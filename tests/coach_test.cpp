#include <gtest/gtest.h>

#include <algorithm>
#include <functional>

#include "myteam/coach.hpp"
#include "myteam/guard.hpp"
#include "myteam/memory.hpp"
#include "test_util.hpp"

using namespace myteam;
using taskml::TaskStatus;
using myteam::test::TempDir;

namespace {

class FakeRunner : public AgentRunner {
public:
    using Respond = std::function<std::string(const RunRequest&)>;
    FakeRunner(Respond respond, std::vector<RunRequest>* log) : AgentRunner({}), respond_(std::move(respond)), log_(log) {}
    std::string id() const override { return "fake"; }
    std::string start_problem() const override { return ""; }
    std::vector<std::string> command(const RunRequest&) const override { return {"fake"}; }
    void parse_line(std::string_view, StreamState&, const EventSink&) const override {}
    RunResult run(const RunRequest& req, const EventSink&) override {
        log_->push_back(req);
        fs::create_directories(req.run_dir);
        RunResult r;
        r.final_text = respond_(req);
        r.ok = r.final_text != "FAIL" && r.final_text != "RATE";
        if (r.final_text == "RATE") {
            r.failure = Failure::RateLimit;
            r.error = "rate limited";
        }
        r.exit_code = r.ok ? 0 : 1;
        r.usage.turns = 1;
        r.usage.cost_usd = 0.01;
        return r;
    }

private:
    Respond respond_;
    std::vector<RunRequest>* log_;
};

/// A crew: the coach, the Gatekeeper, and two experts in the units api and web.
struct Crew {
    TempDir dir;
    std::vector<RunRequest> runs;
    std::function<std::string(const RunRequest&)> respond;
    std::optional<Project> project;
    std::optional<Team> team;

    explicit Crew(const std::string& plan) {
        dir.write("team/team.md", "---\nid: crew\nname: Crew\ncoach: coach\ngatekeeper: gatekeeper\n"
                                  "models: {default: claude:sonnet}\n---\nThe crew.\n");
        dir.write("team/members/coach.md", "---\nname: Coach\nrole: coach\nmodels: {default: claude:fable}\n---\nDecide.\n");
        dir.write("team/members/gatekeeper.md", "---\nname: Gatekeeper\nrole: gatekeeper\nmodels: {default: claude:opus}\n---\nDoubt.\n");
        dir.write("team/members/designer.md", "---\nname: Designer\nrole: API expert\nunit: api\n---\nAPIs.\n");
        dir.write("team/members/webdev.md", "---\nname: Webdev\nrole: web expert\nunit: web\n---\nFabric.\n");
        dir.write("org/PLAN.md", plan);
        dir.write("org/.myteam/config.yaml", "methodology: coach\nbrief: ORG.md\nguard:\n  checkpoints: false\n");
        dir.write("org/ORG.md", "The suite org.\n");
        dir.write("org/api/.myteam/config.yaml", "rules:\n  - \"Use toolkit.\"\nguard:\n  checkpoints: false\n");
        dir.write("org/api/.myteam/project.md", "The api unit.\n");
        dir.write("org/web/.myteam/project.md", "The web unit.\n");
        project = Project::open(dir / "org");
        team = load_team(dir / "team");
    }

    Coach coach() {
        return Coach(*project, *team, [this](const std::string& id) -> std::unique_ptr<AgentRunner> {
            if (id != "claude") return nullptr;
            return std::make_unique<FakeRunner>([this](const RunRequest& r) { return respond(r); }, &runs);
        });
    }
    CoachStep step() {
        Coach c = coach();
        CoachOptions o;
        return c.step(o);
    }
    Plan plan() const { return Plan::load(dir / "org/PLAN.md"); }
    std::vector<std::string> whats() const {
        std::vector<std::string> out;
        for (const auto& r : runs) out.push_back(r.member + ":" + r.what);
        return out;
    }
};

}  // namespace

// --- the runner --------------------------------------------------------------------------

TEST(CoachRunner, TalkRunsHaveNoToolsAndTheirOwnSystemPrompt) {
    RunRequest r;
    r.talk = true;
    r.system_prompt = "You are the coach.";
    r.model = "opus";
    auto a = make_runner("claude")->command(r);
    auto has = [&](const std::string& x) { return std::find(a.begin(), a.end(), x) != a.end(); };
    auto after = [&](const std::string& x) {
        auto it = std::find(a.begin(), a.end(), x);
        return it == a.end() || it + 1 == a.end() ? std::string("<none>") : *(it + 1);
    };
    EXPECT_EQ(after("--tools"), "");
    EXPECT_EQ(after("--system-prompt"), "You are the coach.");
    EXPECT_EQ(after("--setting-sources"), "");
    EXPECT_TRUE(has("--strict-mcp-config"));
    EXPECT_TRUE(has("--disable-slash-commands"));
    EXPECT_FALSE(has("--permission-mode"));
    EXPECT_FALSE(has("--allowedTools"));
}

TEST(CoachRunner, WorkersAreSlimAndCannotReadTheOrgsPlan) {
    RunRequest r;
    r.deny_read = {"/org/PLAN.md", "/org/.myteam"};
    auto a = make_runner("claude")->command(r);
    auto has = [&](const std::string& x) { return std::find(a.begin(), a.end(), x) != a.end(); };
    EXPECT_TRUE(has("--strict-mcp-config"));
    EXPECT_TRUE(has("--exclude-dynamic-system-prompt-sections"));
    auto it = std::find(a.begin(), a.end(), "--disallowedTools");
    ASSERT_NE(it, a.end());
    EXPECT_NE((it + 1)->find("Read(//org/PLAN.md)"), std::string::npos) << *(it + 1);
    EXPECT_NE((it + 1)->find("Grep(//org/.myteam/**)"), std::string::npos) << *(it + 1);
    // slim: false keeps the user's own MCP servers and skills.
    RunnerSettings s;
    s.slim = false;
    auto b = make_runner("claude", s)->command(RunRequest{});
    EXPECT_EQ(std::find(b.begin(), b.end(), "--strict-mcp-config"), b.end());
}

TEST(CoachRunner, TheGatekeeperRunsCommandsReadOnly) {
    RunRequest r;
    r.permission = Permission::ReadOnly;
    auto a = make_runner("claude")->command(r);
    EXPECT_EQ(std::find(a.begin(), a.end(), "--allowedTools"), a.end());  // no shell by default
    r.read_only_shell = true;
    a = make_runner("claude")->command(r);
    auto it = std::find(a.begin(), a.end(), "--allowedTools");
    ASSERT_NE(it, a.end());
    EXPECT_NE((it + 1)->find("Bash"), std::string::npos);
    EXPECT_NE(std::find(a.begin(), a.end(), "plan"), a.end());  // still plan mode: no file writes
}

// --- the guard ---------------------------------------------------------------------------

TEST(CoachGuard, HiddenPathsAreRefusedInReadsAndCommands) {
    TempDir d;
    d.write("org/PLAN.md", "# P\n");
    d.write("org/api/x.txt", "x");
    Project p = Project::open(d / "org/api");
    GuardContext ctx = guard_context(p, p.config().guard, d / "org/api");
    ctx.hidden = {fs::weakly_canonical(d / "org/PLAN.md"), fs::weakly_canonical(d / "org/.myteam")};
    auto call = [&](const std::string& tool, const std::string& key, const std::string& value) {
        taskml::Json in = taskml::Json::object();
        in.set("tool_name", tool);
        taskml::Json ti = taskml::Json::object();
        ti.set(key, value);
        in.set("tool_input", ti);
        return check_tool_call(ctx, in);
    };
    EXPECT_FALSE(call("Bash", "command", "cat ../PLAN.md").allow);
    EXPECT_FALSE(call("Bash", "command", "grep -n x ../PLAN.md | head").allow);
    EXPECT_FALSE(call("Bash", "command", "ls ../.myteam/runs").allow);
    EXPECT_FALSE(call("Read", "file_path", (d / "org/PLAN.md").string()).allow);
    EXPECT_TRUE(call("Bash", "command", "cat x.txt && ls ..").allow);
    EXPECT_TRUE(call("Read", "file_path", (d / "org/api/x.txt").string()).allow);
}

// --- memory ------------------------------------------------------------------------------

TEST(CoachMemory, LessonsAreKeptOnceAndTheNewestFit) {
    TempDir d;
    Project p = Project::open(d.path());
    add_lessons(p, "designer", "a", {"toolkit check needs the venv's python"}, "2026-11-10");
    add_lessons(p, "designer", "b", {"toolkit check needs the venv's python", "Queue models are in toolkit-bench"}, "2026-11-11");
    Memory m = load_memory(p, "designer");
    ASSERT_EQ(m.lessons.size(), 2u);
    EXPECT_EQ(m.lessons[0], "2026-11-11 ^b: toolkit check needs the venv's python");
    set_coaching(p, "designer", "Price parts at prototype volume.");
    const std::string s = memory_section(p, "designer");
    EXPECT_NE(s.find("From the coach:\n\nPrice parts"), std::string::npos) << s;
    EXPECT_NE(s.find("Queue models"), std::string::npos) << s;
    for (int i = 0; i < 40; ++i)
        add_lessons(p, "designer", "", {"lesson " + std::to_string(i) + " on topic t" + std::to_string(i * 7919)}, "2026-11-12", 30);
    EXPECT_EQ(load_memory(p, "designer").lessons.size(), 30u);
    EXPECT_LE(memory_section(p, "designer", 500).size(), 600u);
}

// --- decisions ---------------------------------------------------------------------------

TEST(CoachDecisions, BlocksAreParsedAndRendered) {
    const std::string text =
        "The queue comes first.\n"
        "<task to=\"@designer\" id=\"queue\" priority=\"p1\" depends=\"^arch schema\">Specify the retry queue\n"
        "It must fail safe.\naccept: a failed job is retried\naccept: toolkit check passes\n</task>\n"
        "<change id=\"^old\" status=\"cancel\">Superseded</change>\n"
        "<answer id=\"q1\">Use 30 seconds.</answer>\n"
        "<ask to=\"designer, webdev gatekeeper\" topic=\"timeout\">Who owns the cache timeout?</ask>\n"
        "<coaching to=\"webdev\">Check the latency report.</coaching>\n"
        "<notes>Watch the budget.</notes>\n<to_customer>Queue first.</to_customer>\n";
    CoachDecisions d = parse_decisions(text);
    EXPECT_EQ(d.reasoning, "The queue comes first.");
    ASSERT_EQ(d.tasks.size(), 1u);
    EXPECT_EQ(d.tasks[0].to, "designer");
    EXPECT_EQ(d.tasks[0].id, "queue");
    EXPECT_EQ(d.tasks[0].priority, 1);
    EXPECT_EQ(d.tasks[0].depends, (std::vector<std::string>{"arch", "schema"}));
    EXPECT_EQ(d.tasks[0].notes, (std::vector<std::string>{"It must fail safe."}));
    EXPECT_EQ(d.tasks[0].criteria.size(), 2u);
    ASSERT_EQ(d.changes.size(), 1u);
    EXPECT_EQ(d.changes[0].id, "old");
    EXPECT_EQ(d.changes[0].status, TaskStatus::Cancelled);
    ASSERT_EQ(d.asks.size(), 1u);
    EXPECT_EQ(d.asks[0].to, (std::vector<std::string>{"designer", "webdev", "gatekeeper"}));
    EXPECT_EQ(d.coaching.size(), 1u);
    EXPECT_EQ(*d.notes, "Watch the budget.");
    EXPECT_TRUE(d.needs_review());
    // What the Gatekeeper reads, and what is held, parses back to the same.
    CoachDecisions again = parse_decisions(render_decisions(d));
    EXPECT_EQ(again.tasks[0].criteria, d.tasks[0].criteria);
    EXPECT_EQ(again.asks[0].to, d.asks[0].to);
    EXPECT_EQ(again.changes[0].status, TaskStatus::Cancelled);
    EXPECT_FALSE(parse_decisions("<notes>x</notes><ask to=\"a\">q</ask>").needs_review());
    EXPECT_TRUE(parse_decisions("Nothing to do.").empty());
    EXPECT_FALSE(parse_gate_review("ok <object>No criteria.</object>").approve);
    EXPECT_TRUE(parse_gate_review("<approve>Fine.</approve>").approve);
    EXPECT_EQ(parse_verdict("<verdict status=\"reject\">No test.</verdict>").accept, false);
    EXPECT_FALSE(parse_verdict("nothing").accept.has_value());
}

// --- the step ----------------------------------------------------------------------------

TEST(CoachStep, WorkIsCheckedThenTheCoachDecidesWithTheGatekeeper) {
    Crew c("---\nproject: suite\ngoal: An API within $300\n---\n\n# Suite\n\n## Work\n\n"
           "- [ ] Draw the queue #p1 @designer ^queue\n  - ( ) A failed job is retried\n");
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.member == "designer" && r.what == "^queue")
            return "<task_status>done</task_status><work_summary>queue.py, check passes</work_summary>"
                   "<criterion n=\"1\" status=\"verified\">bench</criterion><lesson>toolkit needs the venv</lesson>";
        if (r.what == "gate:^queue")
            return "<criterion n=\"1\" status=\"verified\">ran it</criterion><verdict status=\"accept\">Checked.</verdict>";
        if (r.what == "coach")
            return "Next the budget.\n<task to=\"designer\" id=\"budget\" depends=\"queue\">Export the budget\naccept: total under $300\n</task>"
                   "<ask to=\"web gatekeeper\" topic=\"timeout\">Who owns the cache timeout?</ask>"
                   "<to_customer>The queue is done.</to_customer><notes>Budget next.</notes>";
        if (r.what == "gatekeeper") return "<approve>Criteria are checkable.</approve>";
        if (r.what.rfind("council:", 0) == 0)
            return r.member == "gatekeeper" ? "<view>Prove it with a latency report.</view>" : "<view>web owns it.</view>";
        return "FAIL";
    };
    CoachStep s = c.step();
    EXPECT_EQ(c.whats(), (std::vector<std::string>{"designer:^queue", "gatekeeper:gate:^queue", "coach:coach", "gatekeeper:gatekeeper",
                                                    "webdev:council:timeout", "gatekeeper:council:timeout"}));
    EXPECT_TRUE(s.problems.empty()) << (s.problems.empty() ? "" : s.problems.front());
    EXPECT_TRUE(s.decided);
    // The expert worked in its unit, without the org's plan.
    const RunRequest& work = c.runs[0];
    EXPECT_EQ(work.workdir, c.project->dir() / "api");
    EXPECT_NE(std::find(work.deny_read.begin(), work.deny_read.end(), fs::weakly_canonical(c.dir / "org/PLAN.md").string()),
              work.deny_read.end());
    EXPECT_NE(work.prompt.find("Use toolkit."), std::string::npos);  // the unit's rules
    EXPECT_NE(work.prompt.find("The api unit."), std::string::npos);  // the unit's brief
    EXPECT_NE(work.prompt.find("Goal: An API within $300"), std::string::npos);
    // The Gatekeeper checked it in the same unit, read-only; the coach and its reviewer only talked.
    EXPECT_EQ(c.runs[1].workdir, c.project->dir() / "api");
    EXPECT_EQ(c.runs[1].permission, Permission::ReadOnly);
    EXPECT_TRUE(c.runs[1].read_only_shell);
    EXPECT_NE(c.runs[1].prompt.find("Done by @designer"), std::string::npos);
    EXPECT_TRUE(c.runs[2].talk);
    EXPECT_EQ(c.runs[2].model, "fable");
    EXPECT_NE(c.runs[2].prompt.find("^queue accepted by the Gatekeeper"), std::string::npos) << c.runs[2].prompt;
    EXPECT_NE(c.runs[2].prompt.find("lesson: "), std::string::npos);
    EXPECT_TRUE(c.runs[3].talk);
    EXPECT_EQ(c.runs[3].model, "opus");
    EXPECT_NE(c.runs[3].prompt.find("<task to=\"designer\" id=\"budget\""), std::string::npos);
    EXPECT_NE(c.runs[5].prompt.find("web owns it."), std::string::npos);  // the skeptic hears the experts

    Plan p = c.plan();
    ASSERT_TRUE(p.ok()) << p.text();
    EXPECT_EQ(p.by_id("queue")->task->status, TaskStatus::Completed);
    const TaskRef* budget = p.by_id("budget");
    ASSERT_NE(budget, nullptr) << p.text();
    EXPECT_EQ(*budget->task->assignee, "designer");
    EXPECT_EQ(budget->task->depends_on, (std::vector<std::string>{"queue"}));
    EXPECT_EQ(load_memory(*c.project, "designer").lessons.size(), 1u);
    EXPECT_EQ(load_memory(*c.project, "coach").other, "Budget next.");
    auto conv = load_conversation(*c.project);
    ASSERT_EQ(conv.size(), 1u);
    EXPECT_EQ(conv[0].text, "The queue is done.");
    // The council's views wait for the coach's next decision.
    CoachState st = load_coach_state(*c.project);
    ASSERT_EQ(st.news.size(), 1u);
    EXPECT_NE(st.news[0].find("@gatekeeper: Prove it with a latency report."), std::string::npos) << st.news[0];

    // The next step: the new task runs in api with Designer's memory; the coach reads the council.
    c.runs.clear();
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "^budget") return "<task_status>in_progress</task_status><work_summary>half</work_summary>";
        if (r.what == "coach") return "Fine.";
        return "FAIL";
    };
    c.step();
    ASSERT_EQ(c.whats(), (std::vector<std::string>{"designer:^budget", "coach:coach"}));
    EXPECT_NE(c.runs[0].prompt.find("toolkit needs the venv"), std::string::npos);
    EXPECT_NE(c.runs[1].prompt.find("Council on \"timeout\""), std::string::npos);
}

TEST(CoachStep, TheGatekeeperMayObjectOnceAndTheCoachMayOverrule) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n");
    append_conversation(*c.project, "customer", "", "Build a schema.");
    int coach_turn = 0;
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "coach") {
            ++coach_turn;
            if (coach_turn == 1) return "<task to=\"designer\" id=\"everything\">Do everything</task>";
            return "I disagree.<task to=\"designer\" id=\"everything\">Do everything</task><overrule>It is a spike.</overrule>";
        }
        if (r.what == "gatekeeper") return "<object>No criteria to check.</object>";
        return "FAIL";
    };
    CoachStep s1 = c.step();
    EXPECT_EQ(c.plan().by_id("everything"), nullptr);  // held
    CoachState st = load_coach_state(*c.project);
    EXPECT_EQ(st.objection, "No criteria to check.");
    EXPECT_NE(st.held.find("Do everything"), std::string::npos);
    EXPECT_FALSE(s1.waiting);

    c.runs.clear();
    c.step();
    // The coach saw the objection, and overruled it: no second review.
    ASSERT_EQ(c.whats(), (std::vector<std::string>{"coach:coach"}));
    EXPECT_NE(c.runs[0].prompt.find("The Gatekeeper objected"), std::string::npos);
    EXPECT_NE(c.plan().by_id("everything"), nullptr);
    st = load_coach_state(*c.project);
    EXPECT_TRUE(st.objection.empty());
    EXPECT_NE(read_file(c.dir / "org/CHANGELOG.md").find("Overruled the Gatekeeper: It is a spike."), std::string::npos);
}

TEST(CoachStep, WorkSentBackTooOftenWaitsForTheCoach) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n- [ ] Draw it @designer ^draw\n  - ( ) It exists\n");
    c.project->config().coach.max_rejections = 1;
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "^draw") return "<task_status>done</task_status><work_summary>drew it</work_summary>";
        if (r.what == "gate:^draw") return "<verdict status=\"reject\">There is no file.</verdict>";
        if (r.what == "coach") return "Wait.";
        return "FAIL";
    };
    c.step();
    Plan p1 = c.plan();
    EXPECT_EQ(p1.by_id("draw")->task->status, TaskStatus::Pending);
    EXPECT_EQ(p1.by_id("draw")->task->notes.back().rfind("Sent back by the Gatekeeper @gatekeeper", 0), 0u);
    c.step();
    Plan p2 = c.plan();
    EXPECT_EQ(p2.by_id("draw")->task->status, TaskStatus::Blocked) << p2.text();
    bool coach_task = false;
    for (const TaskRef& r : p2.tasks())
        if (r.task->assignee && *r.task->assignee == "coach" && r.task->description.find("Decide on ^draw") != std::string::npos)
            coach_task = true;
    EXPECT_TRUE(coach_task) << p2.text();
}

TEST(CoachStep, AnIdleCrewAsksTheCoachOnceThenWaits) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n");
    c.respond = [&](const RunRequest& r) -> std::string { return r.what == "coach" ? "Nothing yet." : "FAIL"; };
    CoachStep s1 = c.step();
    EXPECT_TRUE(s1.decided);
    CoachStep s2 = c.step();
    EXPECT_FALSE(s2.decided);
    EXPECT_TRUE(s2.waiting);
    EXPECT_EQ(c.runs.size(), 1u);
    // The customer writes: the coach decides again.
    append_conversation(*c.project, "customer", "", "Start with the schema.");
    CoachStep s3 = c.step();
    EXPECT_TRUE(s3.decided);
    EXPECT_NE(c.runs.back().prompt.find("Start with the schema."), std::string::npos);
}

TEST(CoachStep, ARateLimitStopsTheStep) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n- [ ] Draw it @designer ^draw\n");
    c.respond = [&](const RunRequest&) -> std::string { return "RATE"; };
    CoachStep s = c.step();
    EXPECT_TRUE(s.rate_limited);
    EXPECT_TRUE(s.waiting);
    EXPECT_EQ(c.runs.size(), 1u);  // no coach run on top
    EXPECT_EQ(c.plan().by_id("draw")->task->status, TaskStatus::Pending);
}

#include "myteam/inspect.hpp"

TEST(CoachInspect, WhatAUiReadsAfterAStep) {
    Crew c("---\nproject: suite\ngoal: A schema\n---\n\n## Work\n\n- [ ] Draw it @designer ^draw\n  - ( ) It exists\n");
    append_conversation(*c.project, "customer", "", "Draw the schema.");
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "^draw") return "<task_status>done</task_status><work_summary>drew it</work_summary><lesson>use the venv</lesson>";
        if (r.what == "gate:^draw") return "<verdict status=\"accept\">Seen.</verdict>";
        if (r.what == "coach") return "<to_customer>It is drawn.</to_customer><ask to=\"api\" topic=\"next\">What next?</ask><notes>n</notes>";
        if (r.what.rfind("council:", 0) == 0) return "<view>The budget.</view>";
        return "FAIL";
    };
    c.step();
    taskml::Json o = inspect::overview(*c.project, *c.team);
    EXPECT_EQ(o.find("goal")->as_string(), "A schema");
    EXPECT_EQ(o.find("counts")->find("done")->as_number(), 1);
    EXPECT_EQ(o.find("needs_you")->as_array().size(), 1u);  // the coach wrote to you
    EXPECT_GT(o.find("coach")->find("cost_usd")->as_number(), 0);
    taskml::Json t = inspect::team(*c.project, *c.team);
    const auto& members = t.find("members")->as_array();
    auto designer = std::find_if(members.begin(), members.end(), [](const taskml::Json& m) { return m.find("handle")->as_string() == "designer"; });
    ASSERT_NE(designer, members.end());
    EXPECT_EQ(designer->find("unit")->as_string(), "api");
    EXPECT_EQ(designer->find("memory")->find("lessons")->as_array().size(), 1u);
    EXPECT_EQ(members[0].find("kind")->as_string(), "coach");
    taskml::Json conv = inspect::conversation(*c.project);
    EXPECT_EQ(conv.find("messages")->as_array().size(), 2u);
    EXPECT_TRUE(conv.find("waiting_for_you")->as_bool());
    taskml::Json co = inspect::council(*c.project);
    ASSERT_EQ(co.find("council")->as_array().size(), 1u);
    const taskml::Json& one = co.find("council")->as_array()[0];
    EXPECT_EQ(one.find("question")->as_string(), "What next?");
    EXPECT_EQ(one.find("views")->as_array()[0].find("text")->as_string(), "The budget.");
    std::set<std::string> kinds;
    const taskml::Json runs = inspect::runs(*c.project);
    for (const auto& r : runs.find("runs")->as_array()) kinds.insert(r.find("kind")->as_string());
    EXPECT_EQ(kinds, (std::set<std::string>{"task", "check", "coach", "council"}));
    EXPECT_EQ(inspect::coach(*c.project, *c.team).find("notes")->as_string(), "n");
}

TEST(CoachStep, ASecondObjectionReachesTheCoachWhole) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n");
    append_conversation(*c.project, "customer", "", "Go.");
    int turn = 0;
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "coach") return ++turn == 1 ? "<task to=\"designer\" id=\"a\">A\naccept: x</task>" : "<task to=\"designer\" id=\"a\">A\naccept: x</task>";
        if (r.what == "gatekeeper") return "<object>Re-issue with three edits:\n1. one\n2. two\n3. three</object>";
        return "FAIL";
    };
    c.step();  // the first objection holds the decisions
    c.step();  // the revision applies; the second objection is news, whole
    const CoachState st = load_coach_state(*c.project);
    ASSERT_FALSE(st.news.empty());
    EXPECT_NE(st.news.back().find("3. three"), std::string::npos) << st.news.back();
    EXPECT_NE(c.plan().by_id("a"), nullptr);
}

TEST(CoachStep, FewSlotsGoByPriorityAndToEveryExpertFirst) {
    // Two tool tasks first in the plan, then api's p0: api's runs, and the second tool task waits.
    Crew c("---\nproject: suite\n---\n\n## Work\n\n- [ ] Tool one @webdev ^t1\n- [ ] Tool two @webdev ^t2\n"
           "- [ ] The manual #p0 @designer ^ds\n");
    c.project->config().coach.tasks_per_step = 2;
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "coach") return "Fine.";
        return "<task_status>in_progress</task_status><work_summary>w</work_summary>";
    };
    c.step();
    std::vector<std::string> tasks;
    for (const auto& r : c.runs)
        if (r.what[0] == '^') tasks.push_back(r.what);
    EXPECT_EQ(tasks, (std::vector<std::string>{"^ds", "^t1"}));
}

TEST(CoachStep, AmongEqualPriorityWhoeverRanLeastRecentlyGoesFirst) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n- [ ] Tool one #p0 @webdev ^t1\n- [ ] Tool two #p0 @webdev ^t2\n"
           "- [ ] The manual #p0 @designer ^ds\n");
    c.project->config().coach.tasks_per_step = 1;
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "coach") return "Fine.";
        return "<task_status>in_progress</task_status><work_summary>w</work_summary>";
    };
    std::vector<std::string> order;
    for (int i = 0; i < 3; ++i) {
        c.runs.clear();
        c.step();
        for (const auto& r : c.runs)
            if (r.what[0] == '^') order.push_back(r.member);
    }
    // One slot a step: webdev (first in plan), then designer (never ran), then webdev again.
    EXPECT_EQ(order, (std::vector<std::string>{"webdev", "designer", "webdev"}));
}

TEST(CoachStep, RoutineNewsWaitsAndDocsAreCheckedLighter) {
    Crew c("---\nproject: suite\n---\n\n## Work\n\n- [ ] Grind on @designer ^grind\n"
           "- [ ] Document the queue in docs/USER_GUIDE.md #docs @webdev ^doc\n  - ( ) It is there\n");
    for (auto& m : c.team->members)
        if (m.handle == "gatekeeper") m.models.economy = ModelRef{"claude", "haiku"};
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "^grind") return "<task_status>in_progress</task_status><work_summary>more</work_summary>";
        if (r.what == "^doc") return "<task_status>done</task_status><work_summary>written</work_summary>";
        if (r.what == "gate:^doc") return "<verdict status=\"accept\">There.</verdict>";
        if (r.what == "coach") return "Fine.";
        return "FAIL";
    };
    c.step();  // the docs task is done and checked (an acceptance is urgent): the coach decides
    auto model_of = [&](const std::string& what) {
        for (const auto& r : c.runs)
            if (r.what == what) return r.model;
        return std::string("<none>");
    };
    EXPECT_EQ(model_of("gate:^doc"), "haiku");
    EXPECT_NE(model_of("coach"), "<none>");
    // Now only routine progress: the coach waits for quiet_steps (2) steps.
    c.runs.clear();
    c.step();
    EXPECT_EQ(model_of("coach"), "<none>");
    c.runs.clear();
    c.step();
    EXPECT_EQ(model_of("coach"), "<none>");
    c.runs.clear();
    c.step();
    EXPECT_NE(model_of("coach"), "<none>");
}

TEST(CoachStep, AVerdictListReachesTheNoteWhole) {
    Crew c("---\nproject: p\n---\n\n## Work\n\n- [ ] Draw it @designer ^draw\n  - ( ) It exists\n");
    c.respond = [&](const RunRequest& r) -> std::string {
        if (r.what == "^draw") return "<task_status>done</task_status><work_summary>drew it</work_summary>";
        if (r.what == "gate:^draw") return "<verdict status=\"reject\">Three things:\n(1) one\n(2) two\n(3) three</verdict>";
        if (r.what == "coach") return "Fine.";
        return "FAIL";
    };
    c.step();
    const Plan p = c.plan();
    const TaskRef* t = p.by_id("draw");
    ASSERT_NE(t, nullptr) << p.text();
    ASSERT_FALSE(t->task->notes.empty()) << p.text();
    const std::string note = t->task->notes.back();
    EXPECT_NE(note.find("Three things: (1) one (2) two (3) three"), std::string::npos) << note;
}
