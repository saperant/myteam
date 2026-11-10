#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <sys/stat.h>

#include "myteam/blockers.hpp"
#include "myteam/commits.hpp"
#include "myteam/loop.hpp"
#include "test_util.hpp"

using namespace myteam;
using taskml::TaskStatus;
using myteam::test::TempDir;

namespace {

// Answers every run with what `respond` returns; may change files first.
class FakeRunner : public AgentRunner {
public:
    using Respond = std::function<std::string(const RunRequest&)>;
    FakeRunner(Respond respond, std::vector<RunRequest>* log)
        : AgentRunner({}), respond_(std::move(respond)), log_(log) {}
    std::string id() const override { return "fake"; }
    std::string start_problem() const override { return ""; }  // a fake: it can start
    std::vector<std::string> command(const RunRequest&) const override { return {"fake"}; }
    void parse_line(std::string_view, StreamState&, const EventSink&) const override {}

    RunResult run(const RunRequest& req, const EventSink& sink) override {
        log_->push_back(req);
        fs::create_directories(req.run_dir);
        RunResult r;
        r.final_text = respond_(req);
        r.ok = r.final_text != "FAIL";
        r.exit_code = r.ok ? 0 : 1;
        if (!r.ok) r.error = "exit code 1: boom";
        r.usage.turns = 2;
        if (sink) sink({AgentEvent::Kind::Text, r.final_text, ""});
        return r;
    }

private:
    Respond respond_;
    std::vector<RunRequest>* log_;
};

struct Fixture {
    TempDir dir;
    std::vector<RunRequest> runs;
    FakeRunner::Respond respond;
    std::optional<Project> project;
    std::optional<Team> team;

    explicit Fixture(const std::string& plan) {
        dir.write("team/team.md", "---\nid: t\nname: T\nfacilitator: po\nrepairer: dev\nmodels: {default: claude:sonnet, premium: claude:opus}\n"
                                  "allocation:\n  keywords: {test: qa}\n---\n");
        dir.write("team/members/po.md", "---\nname: Po\nrole: product-owner\n---\nPrioritize.\n");
        dir.write("team/members/dev.md", "---\nname: Dev\nrole: developer\nhandoffs:\n  - when: done\n    to: qa\n---\nCode.\n");
        dir.write("team/members/qa.md", "---\nname: Qa\nrole: tester\n---\nTest.\n");
        dir.write("proj/PLAN.md", plan);
        dir.write("proj/.myteam/project.md", "The demo project.\n");
        project = Project::open(dir / "proj");
        team = load_team(dir / "team");
    }

    Loop loop() {
        return Loop(*project, *team, [this](const std::string& id) -> std::unique_ptr<AgentRunner> {
            if (id != "claude") return nullptr;
            return std::make_unique<FakeRunner>([this](const RunRequest& r) { return respond(r); }, &runs);
        });
    }
    Plan plan() const { return Plan::load(dir / "proj/PLAN.md"); }
    std::string changelog() const { return dir.read("proj/CHANGELOG.md"); }
};

}  // namespace

TEST(Loop, DispatchesAppliesReportAndHandsOff) {
    Fixture f("---\nproject: Demo\n---\n\n## Work\n\nNotes for humans.\n\n- [ ] Build it #p0 @dev\n- [ ] Unrelated @someone\n");
    f.respond = [](const RunRequest&) {
        return "Built.\n<task_status>done</task_status>\n<work_summary>Wrote build.cpp</work_summary>\n"
               "<handoff to=\"qa\">Test the build\nCheck the error paths too.</handoff>\n"
               "<handoff to=\"ghost\">nobody</handoff>";
    };
    Loop loop = f.loop();
    std::vector<std::string> events;
    LoopOptions o;
    o.on_event = [&](const DispatchPlan&, const AgentEvent& e) { events.push_back(e.text); };
    IterationResult r = loop.iterate(o);

    ASSERT_EQ(f.runs.size(), 1u);
    EXPECT_EQ(f.runs[0].model, "opus");  // #p0: premium
    EXPECT_NE(f.runs[0].prompt.find("You are Dev (@dev)"), std::string::npos);
    EXPECT_NE(f.runs[0].prompt.find("The demo project."), std::string::npos);
    EXPECT_EQ(f.runs[0].prompt.find("Unrelated"), std::string::npos);
    EXPECT_EQ(f.runs[0].workdir, f.project->dir());
    ASSERT_EQ(r.dispatches.size(), 1u);
    const DispatchOutcome& d = r.dispatches[0];
    EXPECT_EQ(d.problems, (std::vector<std::string>{
                              "handoff ^test-the-build has no acceptance criteria (accept: lines): @qa cannot tell when it is done",
                              "handoff to unknown member @ghost"}));
    EXPECT_FALSE(events.empty());

    Plan p = f.plan();
    ASSERT_TRUE(p.ok());
    const TaskRef* built = p.by_id("build-it");  // id given before dispatch
    ASSERT_NE(built, nullptr);
    EXPECT_EQ(built->task->status, TaskStatus::Completed);
    const TaskRef* handoff = p.by_id("test-the-build");
    ASSERT_NE(handoff, nullptr);
    EXPECT_EQ(*handoff->task->assignee, "qa");
    EXPECT_EQ(handoff->task->priority, 0);
    EXPECT_EQ(handoff->task->depends_on, (std::vector<std::string>{"build-it"}));
    // The sender finished in the same report: the task it handed off hears how.
    ASSERT_EQ(handoff->task->notes.size(), 3u) << p.text();
    EXPECT_EQ(handoff->task->notes[0], "Check the error paths too.");
    EXPECT_EQ(handoff->task->notes[1], "Handed off by @dev");
    EXPECT_EQ(handoff->task->notes[2].rfind("Dependency ^build-it done (", 0), 0u) << handoff->task->notes[2];
    EXPECT_NE(p.text().find("Notes for humans."), std::string::npos);

    std::string log = f.changelog();
    EXPECT_NE(log.find("# Changelog"), std::string::npos);
    EXPECT_NE(log.find("Build it (^build-it)"), std::string::npos);
    EXPECT_NE(log.find("- Plan: handoff ^test-the-build → @qa"), std::string::npos);
    EXPECT_NE(log.find("Wrote build.cpp"), std::string::npos);
    EXPECT_TRUE(fs::exists(d.run_dir / "run.json"));

    // Next round: the handoff is ready for qa.
    f.respond = [](const RunRequest&) { return "<task_status reviewer=\"dev\">review</task_status><work_summary>Tested</work_summary>"; };
    IterationResult r2 = loop.iterate(o);
    ASSERT_EQ(r2.dispatches.size(), 1u);
    EXPECT_EQ(r2.dispatches[0].plan.member->handle, "qa");
    EXPECT_EQ(f.plan().by_id("test-the-build")->task->status, TaskStatus::Review);
    EXPECT_EQ(task_reviewer(*f.plan().by_id("test-the-build")->task), "dev");
}

// Review is not a waiting state: a final message that puts the task in review with no
// reviewer, or with its author as one, leaves it in progress and says why.
TEST(Loop, ReviewWithoutAReviewerOrByTheAuthorStaysInProgress) {
    for (const char* reply : {"<task_status>review</task_status>", "<task_status reviewer=\"@dev\">review</task_status>",
                              "<task_status reviewer=\"ghost\">review</task_status>"}) {
        Fixture f("- [ ] Build @dev ^b\n");
        f.respond = [&](const RunRequest&) { return std::string(reply) + "<work_summary>Built</work_summary>"; };
        Loop loop = f.loop();
        LoopOptions o;
        IterationResult r = loop.iterate(o);
        ASSERT_EQ(r.dispatches.size(), 1u) << reply;
        Plan pl = f.plan();
        const TaskRef* b = pl.by_id("b");
        EXPECT_EQ(b->task->status, TaskStatus::InProgress) << reply;
        EXPECT_EQ(b->task->notes.back().rfind("Not in review ", 0), 0u) << b->task->notes.back();
        EXPECT_FALSE(r.dispatches[0].problems.empty());
    }
    Fixture f("- [ ] Build @dev ^b\n");
    f.respond = [](const RunRequest&) { return "<task_status reviewer=\"qa\">review</task_status>"; };
    Loop loop = f.loop();
    LoopOptions o;
    loop.iterate(o);
    Plan pl = f.plan();
    const TaskRef* b = pl.by_id("b");
    EXPECT_EQ(b->task->status, TaskStatus::Review);
    EXPECT_EQ(b->task->notes.back().rfind("Review by @qa (author @dev) ", 0), 0u) << b->task->notes.back();
}

TEST(Loop, FailedRunLeavesANote) {
    Fixture f("- [ ] Build @dev ^b\n");
    f.respond = [](const RunRequest&) { return "FAIL"; };
    Loop loop = f.loop();
    IterationResult r = loop.iterate({});
    ASSERT_EQ(r.dispatches.size(), 1u);
    EXPECT_FALSE(r.dispatches[0].run.ok);
    Plan p = f.plan();
    const TaskRef* t = p.by_id("b");
    EXPECT_EQ(t->task->status, TaskStatus::InProgress);
    ASSERT_EQ(t->task->notes.size(), 1u);
    EXPECT_NE(t->task->notes[0].find("failed"), std::string::npos);
}

TEST(Loop, BlockedReportAddsReason) {
    Fixture f("- [ ] Build @dev ^b\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>blocked</task_status><work_summary>Needs the API key from ops.</work_summary>";
    };
    Loop loop = f.loop();
    loop.iterate({});
    Plan p = f.plan();
    const TaskRef* t = p.by_id("b");
    EXPECT_EQ(t->task->status, TaskStatus::Blocked);
    ASSERT_EQ(t->task->notes.size(), 1u);
    EXPECT_NE(t->task->notes[0].find(" (@dev): Needs the API key from ops."), std::string::npos) << t->task->notes[0];
    EXPECT_EQ(t->task->notes[0].rfind("Blocked 20", 0), 0u);
}

TEST(Loop, RepairsABrokenPlanFirst) {
    Fixture f("- [ ] A @dev ^x\n- [ ] B @dev ^x\n");
    fs::path plan = f.dir / "proj/PLAN.md";
    f.respond = [plan](const RunRequest& r) {
        if (r.prompt.find("no longer validates") != std::string::npos) {
            myteam::write_file_atomic(plan, "- [ ] A @dev ^x\n- [ ] B @dev ^y\n");
            return std::string("<task_status>done</task_status>");
        }
        return std::string("<task_status>done</task_status>");
    };
    Loop loop = f.loop();
    LoopOptions o;
    o.max_dispatches = 1;
    IterationResult r = loop.iterate(o);
    EXPECT_EQ(r.repair_attempts, 1);
    EXPECT_FALSE(r.needs_human);
    ASSERT_EQ(r.dispatches.size(), 2u);  // repair + one task
    EXPECT_EQ(r.dispatches[0].plan.member->handle, "dev");  // the repairer
    EXPECT_TRUE(f.plan().ok());
}

TEST(Loop, GivesUpAfterMaxRepairs) {
    Fixture f("- [ ] A ^x\n- [ ] B ^x\n");
    f.respond = [](const RunRequest&) { return std::string("<task_status>done</task_status>"); };
    Loop loop = f.loop();
    LoopOptions o;
    o.max_repairs = 2;
    IterationResult r = loop.iterate(o);
    EXPECT_EQ(r.repair_attempts, 2);
    EXPECT_TRUE(r.needs_human);
    EXPECT_NE(f.changelog().find("Human needed"), std::string::npos);
}

TEST(Loop, DryRunChangesNothing) {
    std::string text = "- [ ] Build @dev\n- [ ] Test @qa\n";
    Fixture f(text);
    f.respond = [](const RunRequest&) { return std::string(); };
    Loop loop = f.loop();
    LoopOptions o;
    o.dry_run = true;
    IterationResult r = loop.iterate(o);
    EXPECT_EQ(r.dispatches.size(), 2u);
    EXPECT_TRUE(f.runs.empty());
    EXPECT_EQ(f.dir.read("proj/PLAN.md"), text);
    EXPECT_FALSE(fs::exists(f.dir / "proj/CHANGELOG.md"));
}

TEST(Loop, InteractiveDeclineAndUnknownRunner) {
    Fixture f("- [ ] Build @dev ^b\n");
    f.respond = [](const RunRequest&) { return std::string("<task_status>done</task_status>"); };
    Loop loop = f.loop();
    LoopOptions o;
    o.confirm = [](const DispatchPlan&) { return false; };
    IterationResult r = loop.iterate(o);
    ASSERT_EQ(r.dispatches.size(), 1u);
    EXPECT_TRUE(r.dispatches[0].skipped);
    EXPECT_TRUE(f.runs.empty());

    LoopOptions other;
    other.runner = "codex";  // the factory only knows claude
    IterationResult r2 = loop.iterate(other);
    ASSERT_EQ(r2.dispatches.size(), 1u);
    EXPECT_NE(r2.dispatches[0].run.error.find("unknown runner"), std::string::npos);
}

TEST(Loop, RecordsOrgIssuesForMyteam) {
    Fixture f("- [ ] Build @dev ^b\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Built</work_summary>"
               "<org_issue area=\"context\">The context does not say how to run the tests</org_issue>";
    };
    Loop loop = f.loop();
    IterationResult r = loop.iterate({});
    ASSERT_EQ(r.dispatches.size(), 1u);
    EXPECT_NE(f.runs[0].prompt.find("<org_issue"), std::string::npos);
    EXPECT_EQ(r.dispatches[0].feedback.size(), 1u);
    Plan fb = Plan::load(f.project->feedback_path());
    ASSERT_TRUE(fb.ok());
    ASSERT_EQ(fb.tasks().size(), 1u);
    EXPECT_EQ(fb.tasks()[0].task->description, "The context does not say how to run the tests");
    EXPECT_NE(f.changelog().find("- Feedback for myteam: new ^"), std::string::npos);
    // The plan is not touched by it.
    EXPECT_EQ(f.plan().text().find("context does not say"), std::string::npos);
}

TEST(Loop, HandoffsCarryTheirCriteria) {
    Fixture f("# P\n\n- [ ] Build it @dev ^build\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Built.</work_summary>\n"
               "<handoff to=\"qa\">Test the build\nRun it on the samples.\naccept: every sample passes\n"
               "- ( ) the error paths are covered</handoff>";
    };
    LoopOptions o;
    f.loop().iterate(o);
    Plan p = f.plan();
    const TaskRef* t = nullptr;
    for (const TaskRef& r : p.tasks())
        if (r.task->description == "Test the build") t = &r;
    ASSERT_NE(t, nullptr) << p.text();
    ASSERT_EQ(t->task->criteria.size(), 2u) << p.text();
    EXPECT_EQ(t->task->criteria[0].description, "every sample passes");
    EXPECT_EQ(t->task->criteria[1].description, "the error paths are covered");
    EXPECT_NE(std::find(t->task->notes.begin(), t->task->notes.end(), "Run it on the samples."), t->task->notes.end());
}

TEST(Loop, AgentsGetTheProjectsVirtualenvAndTheBriefSaysWhereItGoesOn) {
    TempDir d;
    // A CLI that tells what python it would run.
    d.write("cli", "#!/bin/sh\nread x\necho \"{\\\"type\\\":\\\"result\\\",\\\"subtype\\\":\\\"success\\\",\\\"result\\\":"
                   "\\\"$VIRTUAL_ENV|$(command -v python)\\\",\\\"num_turns\\\":1}\"\n");
    ::chmod((d / "cli").c_str(), 0755);
    fs::create_directories(d / "proj/.venv/bin");
    fs::create_symlink("/bin/sh", d / "proj/.venv/bin/python");
    RunnerSettings s;
    s.command = (d / "cli").string();
    RunRequest req;
    req.prompt = "x";
    req.workdir = d / "proj";
    req.run_dir = d / "run";
    RunResult r = make_runner("claude", s)->run(req);
    EXPECT_EQ(r.final_text, (d / "proj/.venv").string() + "|" + (d / "proj/.venv/bin/python").string());
    // A venv made for another system (its python missing) is left alone.
    fs::remove(d / "proj/.venv/bin/python");
    fs::create_symlink("/nonexistent/python3.14", d / "proj/.venv/bin/python");
    EXPECT_EQ(make_runner("claude", s)->run(req).final_text.rfind("|", 0), 0u);

    // A long brief is cut at a line, saying where the rest is.
    d.write("proj/.myteam/config.yaml", "brief_limit: 60\n");
    d.write("proj/.myteam/project.md", "# Brief\n\nline one is here\nline two is here\nline three is here\nfour\n");
    std::string b = Project::open(d / "proj").brief();
    EXPECT_EQ(b.find("line three"), std::string::npos) << b;
    EXPECT_NE(b.find("line two is here\n\n[The brief goes on"), std::string::npos) << b;
    EXPECT_NE(b.find("Read the rest in .myteam/project.md"), std::string::npos) << b;
}

TEST(Loop, TheSameJobHandedOffTwiceIsOneTaskAndALongListArrives) {
    // Verifier's case: qa already has the job; dev hands it off again.
    Fixture f("# P\n\n- [ ] Migrate @dev ^mig\n- [ ] Run the migrations against PostgreSQL and SQLite @qa ^run-mig\n");
    std::string list;
    for (int i = 1; i <= 20; ++i) list += std::to_string(i) + ". defect number " + std::to_string(i) + "\n";
    f.respond = [&](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Done.</work_summary>\n"
               "<handoff to=\"qa\">Run the migrations against PostgreSQL and SQLite\naccept: both pass</handoff>\n"
               "<handoff to=\"qa\">Fix the defects listed below\n" + list + "accept: all 20 are fixed</handoff>\n";
    };
    LoopOptions o;
    o.only_task = "^mig";
    IterationResult r = f.loop().iterate(o);
    ASSERT_EQ(r.dispatches.size(), 1u);
    Plan p = f.plan();
    int runs = 0;
    const TaskRef* defects = nullptr;
    for (const TaskRef& t : p.tasks()) {
        if (t.task->description.find("Run the migrations") != std::string::npos) ++runs;
        if (t.task->description == "Fix the defects listed below") defects = &t;
    }
    EXPECT_EQ(runs, 1) << p.text();  // not a second task
    const auto& notes = p.by_id("run-mig")->task->notes;
    ASSERT_FALSE(notes.empty());
    EXPECT_NE(notes.back().find("Also handed off by @dev from ^mig"), std::string::npos) << p.text();
    ASSERT_NE(defects, nullptr) << p.text();
    // all 20 lines, who handed it off, and that ^mig (its dependency) is done
    EXPECT_EQ(defects->task->notes.size(), 22u);
    EXPECT_EQ(defects->task->notes.back().rfind("Dependency ^mig done (", 0), 0u) << p.text();
    EXPECT_EQ(defects->task->criteria.size(), 1u);
}

TEST(Loop, AQuestionForTheProductOwnerGoesToTheDesk) {
    Fixture f("# P\n\n- [ ] Validate prices @dev ^prices\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>blocked</task_status><work_summary>Need a decision.</work_summary>\n"
               "<blocker to=\"po\">Are free (price 0) products allowed?</blocker>";
    };
    LoopOptions o;
    f.loop().iterate(o);
    Plan p = f.plan();
    const TaskRef* q = nullptr;
    for (const TaskRef& t : p.tasks())
        if (t.task->description.find("free (price 0)") != std::string::npos) q = &t;
    ASSERT_NE(q, nullptr) << p.text();
    EXPECT_TRUE(is_customer_question(*q->task)) << p.text();  // the PO's desk answers it, from what it knows
    EXPECT_EQ(q->task->assignee.value_or(""), "po");
}

TEST(Loop, BlockedOnItsOwnHandoffIsNoDeadlock) {
    // shop: a handoff waits for the task it came from; that task then reports
    // a blocker on the handoff. Both waiting for each other broke the plan edit.
    Fixture f("# P\n\n- [~] Beef product @dev ^beef\n- [ ] Extend the batch model @qa ^extend\n  - depends: ^beef\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>blocked</task_status><work_summary>Needs the batch model.</work_summary>\n"
               "<blocker on=\"^extend\">The batch model must come first.</blocker>";
    };
    LoopOptions o;
    o.only_task = "^beef";
    IterationResult r = f.loop().iterate(o);
    ASSERT_EQ(r.dispatches.size(), 1u);
    for (const auto& pr : r.dispatches[0].problems) ADD_FAILURE() << pr;
    Plan p = f.plan();
    ASSERT_TRUE(p.ok()) << p.text();
    EXPECT_EQ(p.by_id("beef")->task->status, TaskStatus::Blocked);
    EXPECT_EQ(p.by_id("beef")->task->blocked_by, std::vector<std::string>{"extend"});
    EXPECT_TRUE(p.by_id("extend")->task->depends_on.empty()) << p.text();
    EXPECT_TRUE(p.ready(*p.by_id("extend")));  // it can start now
    const auto& notes = p.by_id("extend")->task->notes;
    EXPECT_TRUE(std::any_of(notes.begin(), notes.end(),
                            [](const std::string& n) { return n.find("No longer waits for ^beef") != std::string::npos; }));
}

TEST(Loop, TheBriefMarksFilesThatAreNotThereYet) {
    TempDir d;
    d.write("proj/scripts/test.sh", "#!/bin/sh\n");
    d.write("proj/.myteam/project.md", "# Brief\n\nRun `scripts/test.sh`, then `scripts/verify.sh`. See `docs/security.md`.\n");
    std::string b = Project::open(d / "proj").brief();
    EXPECT_NE(b.find("`scripts/test.sh`, then"), std::string::npos) << b;
    EXPECT_NE(b.find("`scripts/verify.sh` (not in the project yet)"), std::string::npos) << b;
    EXPECT_NE(b.find("`docs/security.md` (not in the project yet)"), std::string::npos) << b;
}

// ^notes-said-the-task-for, ^the-task-notes-say-for, ^cancelling-an-org-task: a handoff may
// name another team of the org or the operator; their task does not wait for the sender's
// task, and the sender's task is not blocked by it.
TEST(Loop, HandoffToAUnitOrTheOperatorDoesNotBlockTheSender) {
    Fixture f("# P\n\n- [ ] Plan the unit tasks #sprint-5 @dev ^plan\n");
    f.team->external["web"] = "the web front end";
    f.respond = [](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Planned.</work_summary>\n"
               "<handoff to=\"web\">Freeze the cache link\naccept: the link is in interfaces/</handoff>\n"
               "<handoff to=\"operator\">Commit the edited ORG.md\naccept: ORG.md is committed</handoff>\n"
               "<handoff to=\"nobody\">Lost\naccept: x</handoff>";
    };
    LoopOptions o;
    auto r = f.loop().iterate(o);
    Plan p = f.plan();
    const TaskRef* unit = nullptr;
    const TaskRef* op = nullptr;
    for (const TaskRef& t : p.tasks()) {
        if (t.task->description == "Freeze the cache link") unit = &t;
        if (t.task->description == "Commit the edited ORG.md") op = &t;
    }
    ASSERT_NE(unit, nullptr) << p.text();
    ASSERT_NE(op, nullptr) << p.text();
    EXPECT_EQ(*unit->task->assignee, "web");
    EXPECT_EQ(*op->task->assignee, "operator");
    EXPECT_TRUE(unit->task->depends_on.empty());
    EXPECT_TRUE(op->task->depends_on.empty());
    // The tag the org carries tasks by comes along.
    EXPECT_NE(std::find(unit->task->tags.begin(), unit->task->tags.end(), "sprint-5"), unit->task->tags.end());
    ASSERT_EQ(unit->task->criteria.size(), 1u);
    const TaskRef* plan = p.by_id("plan");
    EXPECT_EQ(plan->task->status, TaskStatus::Completed) << p.text();
    EXPECT_EQ(p.text().find("blocked-by"), std::string::npos) << p.text();
    EXPECT_EQ(p.text().find("Lost"), std::string::npos);
    bool unknown = false;
    for (const auto& pr : r.dispatches.at(0).problems) unknown = unknown || pr == "handoff to unknown member @nobody";
    EXPECT_TRUE(unknown);
}

TEST(Loop, HandoffToTheOperatorJoinsAnOpenOperatorTask) {
    Fixture f("# P\n\n- [ ] Plan it @dev ^plan\n- [ ] Commit the edited ORG.md @operator ^commit-org\n");
    f.respond = [](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Planned.</work_summary>\n"
               "<handoff to=\"operator\">Commit the edited ORG.md\naccept: ORG.md is committed</handoff>";
    };
    LoopOptions o;
    f.loop().iterate(o);
    Plan p = f.plan();
    int n = 0;
    for (const TaskRef& t : p.tasks()) n += t.task->description == "Commit the edited ORG.md";
    EXPECT_EQ(n, 1) << p.text();
    const auto& notes = p.by_id("commit-org")->task->notes;
    ASSERT_FALSE(notes.empty());
    EXPECT_NE(notes.back().find("Also handed off by @dev"), std::string::npos);
}

// --- The twin check on handoffs, and the loop's cleanup of old twins --------------------

TEST(Twins, AHandoffToAUnitWithTheJobOpenJoinsIt) {
    Fixture f("# P\n\n- [ ] Plan the unit tasks @dev ^plan\n- [ ] Freeze the cache link. @web ^freeze\n");
    f.team->external["web"] = "the web front end";
    f.respond = [](const RunRequest&) {
        return "<task_status>done</task_status><work_summary>Planned.</work_summary>\n"
               "<handoff to=\"web\">freeze the cache link\naccept: the link is in interfaces/</handoff>\n";
    };
    LoopOptions o;
    f.loop().iterate(o);
    Plan p = f.plan();
    EXPECT_EQ(p.tasks().size(), 2u) << p.text();
}

TEST(Twins, TheLoopMergesOldTwinsAndPutsEachOnRecord) {
    Fixture f("# P\n\n- [ ] Write the docs @qa ^docs\n- [ ] Write the docs @qa ^docs-2\n"
              "- [ ] Ship @dev ^ship\n  - blocked-by: ^docs-2\n");
    f.respond = [](const RunRequest&) { return std::string("<task_status>done</task_status><work_summary>ok</work_summary>"); };
    LoopOptions o;
    auto r = f.loop().iterate(o);
    Plan p = f.plan();
    EXPECT_EQ(p.by_id("docs-2")->task->status, TaskStatus::Cancelled) << p.text();
    EXPECT_EQ(duplicate_of(*p.by_id("docs-2")->task), "docs");
    EXPECT_NE(f.changelog().find("- ^docs-2 -> ^docs  Write the docs"), std::string::npos) << f.changelog();
    // The blocker link moved to the kept task (which the fake run then finished).
    const auto& notes = p.by_id("ship")->task->notes;
    EXPECT_TRUE(std::any_of(notes.begin(), notes.end(),
                            [](const std::string& n) { return n.find("Blocker ^docs-2 merged into ^docs") != std::string::npos; }))
        << p.text();
}

// A commit that names another open task (a commit may do its work): a note asks the assignee to close it or check it,
// once per commit, and the stand-up lists it as a flag until the task is finished.
TEST(CommitNames, IdsInAMessage) {
    EXPECT_EQ(task_ids_named("docs: x (^esc-api-irq-file). Also ^b-2, ^b-2 again; a^not\nTask: ^own"),
              (std::vector<std::string>{"esc-api-irq-file", "b-2", "own"}));
    EXPECT_TRUE(task_ids_named("no ids here ^ alone").empty());
}

TEST(CommitNames, NotesOpenTasksAndTheStandUpListsThem) {
    Plan p = Plan::from_text(
        "- [ ] Own @maint ^own\n"
        "- [ ] Open @api ^open\n"
        "- [?] Reviewed @web ^rev\n"
        "- [x] Done @api ^done\n"
        "- [-] Gone @api ^gone\n");
    ASSERT_TRUE(p.ok());
    RunCommit c;
    c.ok = true;
    c.commit = "0123456789abcdef";
    c.summary = "docs: the thing";
    c.message = "docs: the thing (^own)\n\nDoes ^open, ^rev, ^done, ^gone and ^missing.\n";
    RunCommit failed = c;
    failed.ok = false;
    failed.commit.clear();
    PlanEditor ed(p);
    auto changes = note_tasks_commits_name(p, ed, {c, failed}, "own", "maint", "2026-11-09");
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_NE(changes[0].find("^open"), std::string::npos);
    EXPECT_NE(changes[0].find("@api"), std::string::npos);
    EXPECT_NE(changes[1].find("^rev"), std::string::npos);
    ASSERT_TRUE(ed.check().ok);
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok());
    EXPECT_TRUE(q.by_id("own")->task->notes.empty());
    EXPECT_TRUE(q.by_id("done")->task->notes.empty());
    ASSERT_EQ(q.by_id("open")->task->notes.size(), 1u);
    const std::string& n = q.by_id("open")->task->notes[0];
    EXPECT_EQ(n.rfind("Commit `0123456789` by @maint on ^own names this task", 0), 0u) << n;
    EXPECT_NE(n.find("@api, close it or check its criteria"), std::string::npos) << n;

    // The same commit again: no second note.
    PlanEditor ed2(q);
    EXPECT_TRUE(note_tasks_commits_name(q, ed2, {c}, "own", "maint", "2026-11-09").empty());
    EXPECT_FALSE(ed2.changed());

}

TEST(CommitNames, TheMessageSurvivesTheRequestFile) {
    RunCommit c;
    c.ok = true;
    c.commit = "abc";
    c.summary = "s";
    c.message = "s (^x)\n\nbody\n";
    EXPECT_EQ(commit_from_json(commit_to_json(c)).message, c.message);
}
