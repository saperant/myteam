#include <gtest/gtest.h>

#include <functional>

#include "myteam/blockers.hpp"
#include "myteam/loop.hpp"
#include "test_util.hpp"

using namespace myteam;
using taskml::TaskStatus;
using myteam::test::TempDir;

namespace {

struct Reply {
    std::string text;
    bool ok = true;
    std::string error;
    Failure failure = Failure::None;
    std::string session = "s-1";
};

class ScriptedRunner : public AgentRunner {
public:
    using Script = std::function<Reply(const RunRequest&)>;
    ScriptedRunner(Script s, std::vector<RunRequest>* log) : AgentRunner({}), script_(std::move(s)), log_(log) {}
    std::string id() const override { return "claude"; }
    std::string start_problem() const override { return ""; }  // a fake: it can start
    std::vector<std::string> command(const RunRequest&) const override { return {"fake"}; }
    void parse_line(std::string_view, StreamState&, const EventSink&) const override {}
    RunResult run(const RunRequest& req, const EventSink&) override {
        log_->push_back(req);
        fs::create_directories(req.run_dir);
        Reply rep = script_(req);
        RunResult r;
        r.final_text = rep.text;
        r.ok = rep.ok;
        r.exit_code = rep.ok ? 0 : 1;
        r.error = rep.error;
        r.failure = rep.ok ? Failure::None : rep.failure;
        r.session_id = rep.session;
        return r;
    }

private:
    Script script_;
    std::vector<RunRequest>* log_;
};

struct World {
    TempDir dir;
    std::vector<RunRequest> runs;
    ScriptedRunner::Script script;
    std::optional<Project> project;
    std::optional<Team> team;

    explicit World(const std::string& plan) {
        dir.write("team/team.md", "---\nid: t\nname: T\nproduct_owner: po\nmodels: {default: claude:sonnet}\n---\n");
        dir.write("team/members/po.md", "---\nname: Chief\nrole: product-owner\n---\n");
        dir.write("team/members/dev.md", "---\nname: Dev\nrole: developer\n---\n");
        dir.write("team/members/arch.md", "---\nname: Arch\nrole: architect\n---\n");
        dir.write("proj/PLAN.md", plan);
        project = Project::open(dir / "proj");
        team = load_team(dir / "team");
    }
    RunnerFactory runners() {
        return [this](const std::string& id) -> std::unique_ptr<AgentRunner> {
            if (id != "claude") return nullptr;
            return std::make_unique<ScriptedRunner>([this](const RunRequest& r) { return script(r); }, &runs);
        };
    }
    Loop loop() { return Loop(*project, *team, runners()); }
    Plan plan() const { return Plan::load(dir / "proj/PLAN.md"); }
    std::string text() const { return dir.read("proj/PLAN.md"); }
};

bool contains(const std::vector<std::string>& v, std::string_view s) {
    for (const auto& x : v)
        if (x.find(s) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST(Blockers, ParseKinds) {
    AgentReport r = parse_report(R"(<task_status>blocked</task_status>
<blocker on="^schema">Needs the final schema</blocker>
<blocker to="@Arch">Decide REST or gRPC</blocker>
<blocker to="customer">Which export formats are needed?</blocker>
<blocker to="operator">Write access to the test bucket
The key goes in ~/.aws/credentials</blocker>
<blocker kind="access">VPN access</blocker>
<blocker>Something unclear</blocker>)");
    ASSERT_EQ(r.blockers.size(), 6u);
    EXPECT_EQ(r.blockers[0].kind, BlockerReport::Kind::Task);
    EXPECT_EQ(r.blockers[0].on, "schema");
    EXPECT_EQ(r.blockers[1].kind, BlockerReport::Kind::Member);
    EXPECT_EQ(r.blockers[1].to, "arch");
    EXPECT_EQ(r.blockers[2].kind, BlockerReport::Kind::Customer);
    EXPECT_EQ(r.blockers[3].kind, BlockerReport::Kind::Operator);
    EXPECT_EQ(r.blockers[4].kind, BlockerReport::Kind::Operator);
    EXPECT_EQ(r.blockers[5].kind, BlockerReport::Kind::Operator);
}

TEST(Blockers, RecordEveryKindThenUnblock) {
    World w("## Work\n\n- [~] Upload results #p1 @dev ^upload\n  - A note\n- [ ] Schema @arch ^schema\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    std::vector<BlockerReport> bs(4);
    bs[0] = {BlockerReport::Kind::Task, "schema", "", "Needs the final schema"};
    bs[1] = {BlockerReport::Kind::Member, "", "arch", "Decide REST or gRPC\nBoth are fine for us"};
    bs[2] = {BlockerReport::Kind::Customer, "", "", "Which formats are needed?"};
    bs[3] = {BlockerReport::Kind::Operator, "", "", "Write access to the test bucket"};
    BlockerEdits be = record_blockers(p, ed, *p.by_id("upload"), *w.team->find("dev"), *w.team, bs, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    EXPECT_EQ(be.new_tasks.size(), 3u);

    Plan q = w.plan();
    ASSERT_TRUE(q.ok()) << w.text();
    const TaskRef* up = q.by_id("upload");
    EXPECT_EQ(up->task->status, TaskStatus::Blocked);
    ASSERT_EQ(up->task->blocked_by.size(), 4u);
    EXPECT_EQ(up->task->blocked_by[0], "schema");
    EXPECT_EQ(up->task->subtasks.size(), 0u);
    const TaskRef* decide = q.by_id("decide-rest-or-grpc");
    ASSERT_NE(decide, nullptr) << w.text();
    EXPECT_EQ(*decide->task->assignee, "arch");
    EXPECT_EQ(decide->task->priority, 1);
    EXPECT_EQ(decide->task->notes, (std::vector<std::string>{"Both are fine for us", "Needed by ^upload (@dev)"}));
    const TaskRef* ask = q.by_id("ask-the-customer-which");
    ASSERT_NE(ask, nullptr) << w.text();
    EXPECT_EQ(*ask->task->assignee, "po");
    EXPECT_TRUE(is_customer_question(*ask->task));
    const TaskRef* op = q.by_id("write-access-to-the-test");
    ASSERT_NE(op, nullptr) << w.text();
    EXPECT_TRUE(is_operator_task(*op->task));
    // A note per blocker, saying what blocks.
    EXPECT_TRUE(contains(up->task->notes, "(@dev) by ^schema: Needs the final schema"));
    EXPECT_TRUE(contains(up->task->notes, "by ^write-access-to-the-test: Write access to the test bucket"));

    BlockerReview rv = review_blockers(q);
    ASSERT_EQ(rv.blocked.size(), 1u);
    EXPECT_EQ(rv.blocked[0].open.size(), 4u);
    EXPECT_EQ(rv.for_operator.size(), 1u);
    EXPECT_EQ(rv.customer_questions.size(), 1u);

    // The PO's question is not team work.
    for (const TaskRef* t : ready_assigned_tasks(q, *w.team)) EXPECT_FALSE(is_customer_question(*t->task));

    // Finish all four (here by hand), then unblock.
    std::string t = w.text();
    for (const char* id : {"schema", "decide-rest-or-grpc", "ask-the-customer-which", "write-access-to-the-test"}) {
        // The task's own line: "- [ ] ... ^id" at the end of a line.
        std::size_t pos = 0;
        while ((pos = t.find(std::string("^") + id + "\n", pos)) != std::string::npos) {
            std::size_t start = t.rfind('\n', pos) + 1;
            if (t.compare(start, 5, "- [ ]") == 0) {
                t.replace(start, 5, "- [x]");
                break;
            }
            ++pos;
        }
    }
    w.dir.write("proj/PLAN.md", t);
    Plan r = w.plan();
    PlanEditor ued(r);
    auto un = unblock_resolved(r, ued, {{"schema", "@arch: schema.sql is final"}});
    ASSERT_EQ(un.size(), 1u);
    ASSERT_TRUE(ued.commit().ok);
    Plan p_again = w.plan();
    const TaskRef* again = p_again.by_id("upload");
    EXPECT_EQ(again->task->status, TaskStatus::Pending);
    EXPECT_TRUE(contains(again->task->notes, "^schema done (@arch: schema.sql is final)"));
}

TEST(Blockers, NoBlockerBlockIsStillDescribed) {
    World w("- [ ] Build @dev ^b\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    BlockerEdits be = record_blockers(p, ed, *p.by_id("b"), *w.team->find("dev"), *w.team, {}, "The API key is missing.\nMore.");
    ASSERT_TRUE(ed.commit().ok);
    EXPECT_EQ(be.problems.size(), 1u);
    Plan p_b = w.plan();
    const TaskRef* b = p_b.by_id("b");
    EXPECT_EQ(b->task->status, TaskStatus::Blocked);
    EXPECT_TRUE(contains(b->task->notes, "(@dev): The API key is missing."));
    Plan p_rv = w.plan();
    BlockerReview rv = review_blockers(p_rv);
    ASSERT_EQ(rv.blocked.size(), 1u);
    EXPECT_TRUE(rv.blocked[0].unlinked);
}

TEST(Blockers, AFindingIsAnOperatorTaskThatDoesNotBlock) {
    World w("- [~] Schema @dev #p1 #sprint-1 ^b\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    const std::string id = record_finding(p, ed, *p.by_id("b"), *w.team->find("dev"),
                                          "toolkit has no YAML interface type\nUsed JSON for now.");
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(ed.commit().ok);
    Plan q = w.plan();
    const TaskRef* f = q.by_id(id);
    ASSERT_TRUE(f);
    EXPECT_TRUE(is_operator_task(*f->task));
    EXPECT_EQ(f->task->description, "toolkit has no YAML interface type");
    EXPECT_TRUE(contains(f->task->tags, "finding"));
    EXPECT_TRUE(contains(f->task->tags, "sprint-1"));
    EXPECT_TRUE(contains(f->task->notes, "Found by @dev on ^b"));
    EXPECT_EQ(q.by_id("b")->task->status, TaskStatus::InProgress);
    EXPECT_TRUE(q.by_id("b")->task->blocked_by.empty());
    // The same open finding again: nothing new.
    PlanEditor ed2(q);
    EXPECT_EQ(record_finding(q, ed2, *q.by_id("b"), *w.team->find("dev"), "toolkit has no YAML interface type"), "");
    EXPECT_FALSE(ed2.changed());
    // The same gap twice in one report: once.
    std::vector<std::string> added;
    EXPECT_FALSE(record_finding(q, ed2, *q.by_id("b"), *w.team->find("dev"),
                                "toolkit has no queue adapter/retry policy with ttl and backoff", &added).empty());
    EXPECT_EQ(record_finding(q, ed2, *q.by_id("b"), *w.team->find("dev"), "toolkit has no queue adapter/retry policy", &added), "");
    // Addressed to one of the org's other teams (a tool's support team): theirs, not the operator's.
    Team t = *w.team;
    t.external["toolkit"] = "the build tool's support team";
    EXPECT_EQ(finding_address("To \\@toolkit: no OAuth login template", &t), "toolkit");
    EXPECT_EQ(finding_address("@toolkit no template", &t), "toolkit");
    EXPECT_EQ(finding_address("To @nobody: x", &t), "");
    std::string rest;
    EXPECT_EQ(finding_address("toolkit has no queue part", &t, &rest), "toolkit");
    EXPECT_EQ(rest, "toolkit has no queue part");  // kept whole
    EXPECT_EQ(finding_address("The schema has no queue part", &t), "");
    std::vector<std::string> more_added;
    const std::string fid = record_finding(q, ed2, *q.by_id("b"), *w.team->find("dev"),
                                           "To @toolkit: no template for the OAuth login page", &more_added, &t);
    ASSERT_FALSE(fid.empty());
    ASSERT_TRUE(ed2.commit().ok);
    Plan q2 = w.plan();
    EXPECT_EQ(normalize_handle(q2.by_id(fid)->task->assignee.value_or("")), "toolkit");
    EXPECT_EQ(q2.by_id(fid)->task->description, "no template for the OAuth login page");
}

// The same request to the operator twice, worded a little differently: one task.
TEST(Blockers, ADuplicateEscalationWaitsForTheOpenOne) {
    World w("- [~] Adapter @dev #p1 ^a\n- [~] Router @dev #p1 ^r\n"
            "- [ ] Linter cannot skip a rule for one generated file #p1 @operator ^linter-skip\n"
            "  - Needed by ^a (@dev)\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    BlockerReport b{BlockerReport::Kind::Operator, "", "", "linter cannot skip a rule for one generated file"};
    BlockerEdits be = record_blockers(p, ed, *p.by_id("r"), *w.team->find("dev"), *w.team, {b}, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    EXPECT_TRUE(be.new_tasks.empty());
    Plan q = w.plan();
    ASSERT_TRUE(q.ok()) << w.text();
    EXPECT_EQ(q.by_id("r")->task->blocked_by, (std::vector<std::string>{"linter-skip"}));
    EXPECT_TRUE(contains(q.by_id("r")->task->notes, "by ^linter-skip: linter cannot skip"));
    EXPECT_TRUE(contains(q.by_id("linter-skip")->task->notes, "Needed by ^r (@dev)"));
    EXPECT_EQ(review_blockers(q).for_operator.size(), 1u);

    // A different matter still gets its own task.
    PlanEditor ed2(q);
    BlockerReport other{BlockerReport::Kind::Operator, "", "", "The container lacks cpio"};
    EXPECT_EQ(record_blockers(q, ed2, *q.by_id("a"), *w.team->find("dev"), *w.team, {other}, "").new_tasks.size(), 1u);

    // A finding worded differently but on the same subject: nothing new.
    World f("- [~] Events @dev #p2 ^g\n"
            "- [ ] linter rule sets cannot ignore one generated file (no per-file override), and autofix runs on "
            "the whole tree #p2 @operator #finding ^linter-rule-sets\n");
    Plan fp = f.plan();
    PlanEditor fed(fp);
    EXPECT_EQ(record_finding(fp, fed, *fp.by_id("g"), *f.team->find("dev"),
                             "linter rule sets cannot ignore one generated file (no per-file override, per docs/12), and "
                             "autofix runs on the whole tree, not on the changed files"),
              "");
    EXPECT_FALSE(fed.changed());
}

TEST(PlanEditor, AddBlockedBy) {
    Plan p = Plan::from_text("- [ ] A ^a\n  - Note\n- [ ] B ^b\n- [ ] C ^c\n  - blocked-by: ^b\n");
    PlanEditor ed(p);
    ed.add_blocked_by(*p.by_id("a"), "b");
    ed.add_blocked_by(*p.by_id("c"), "a");
    ed.add_blocked_by(*p.by_id("c"), "b");  // already there
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok()) << ed.text();
    EXPECT_EQ(q.by_id("a")->task->blocked_by, (std::vector<std::string>{"b"}));
    EXPECT_EQ(q.by_id("c")->task->blocked_by, (std::vector<std::string>{"b", "a"}));
}

TEST(LoopBlockers, OperatorBlockerResolvedByHand) {
    World w("- [ ] Upload @dev ^upload\n");
    int calls = 0;
    w.script = [&](const RunRequest& r) -> Reply {
        if (++calls == 1)
            return {"<task_status>blocked</task_status><work_summary>Stuck.</work_summary>"
                    "<blocker to=\"operator\">Write access to the test bucket</blocker>"};
        EXPECT_NE(r.prompt.find("Resolution of ^write-access-to-the-test:"), std::string::npos) << r.prompt;
        EXPECT_NE(r.prompt.find("- Key is in ~/.aws"), std::string::npos);
        return {"<task_status>done</task_status>"};
    };
    Loop loop = w.loop();
    IterationResult r1 = loop.iterate({});
    ASSERT_EQ(r1.dispatches.size(), 1u);
    EXPECT_EQ(w.plan().by_id("upload")->task->status, TaskStatus::Blocked);
    // Nothing for the team now: the operator task is not a member's.
    IterationResult idle = loop.iterate({});
    EXPECT_TRUE(idle.dispatches.empty());

    // The operator does it and marks it done, with a note.
    std::string t = w.text();
    t.replace(t.find("- [ ] Write access"), 5, "- [x]");
    t += "  - Key is in ~/.aws\n";
    w.dir.write("proj/PLAN.md", t);
    IterationResult r2 = loop.iterate({});
    ASSERT_EQ(r2.unblocked.size(), 1u);
    ASSERT_EQ(r2.dispatches.size(), 1u);
    EXPECT_EQ(w.plan().by_id("upload")->task->status, TaskStatus::Completed);
}

TEST(LoopBlockers, MemberFinishingUnblocksWithItsSummary) {
    World w("- [ ] Build API @dev ^api\n");
    w.script = [&](const RunRequest& r) -> Reply {
        if (r.member == "dev" && w.runs.size() == 1)
            return {"<task_status>blocked</task_status><blocker to=\"arch\">Decide REST or gRPC</blocker>"};
        if (r.member == "arch") return {"<task_status>done</task_status><work_summary>REST, see docs/adr-1.md</work_summary>"};
        return {"<task_status>done</task_status>"};
    };
    Loop loop = w.loop();
    loop.iterate({});
    IterationResult r2 = loop.iterate({});  // arch decides
    ASSERT_EQ(r2.dispatches.size(), 1u);
    EXPECT_EQ(r2.dispatches[0].plan.member->handle, "arch");
    EXPECT_TRUE(contains(r2.dispatches[0].plan_changes, "unblocked ^api"));
    Plan p_api = w.plan();
    const TaskRef* api = p_api.by_id("api");
    EXPECT_EQ(api->task->status, TaskStatus::Pending);
    EXPECT_TRUE(contains(api->task->notes, "(@arch: REST, see docs/adr-1.md)"));
}

TEST(LoopFailures, AuthRestoresStatusAndStopsTheRunner) {
    World w("- [ ] One @dev ^one\n- [ ] Two @arch ^two\n");
    w.script = [](const RunRequest&) { return Reply{"", false, "Not logged in", Failure::Auth}; };
    Loop loop = w.loop();
    IterationResult r = loop.iterate({});
    EXPECT_EQ(w.runs.size(), 1u);  // the second dispatch is skipped
    EXPECT_TRUE(r.needs_human);
    EXPECT_EQ(w.plan().by_id("one")->task->status, TaskStatus::Pending);
    EXPECT_TRUE(w.plan().by_id("one")->task->notes.empty());
    EXPECT_TRUE(contains(r.notes, "^two skipped"));
}

TEST(LoopFailures, RateLimitIsNotHuman) {
    World w("- [ ] One @dev ^one\n");
    w.script = [](const RunRequest&) { return Reply{"", false, "429 rate limit", Failure::RateLimit}; };
    Loop loop = w.loop();
    IterationResult r = loop.iterate({});
    EXPECT_FALSE(r.needs_human);
    EXPECT_EQ(w.plan().by_id("one")->task->status, TaskStatus::Pending);
}

TEST(LoopFailures, LimitNotesAndResumes) {
    World w("- [ ] Long work @dev ^long\n");
    int calls = 0;
    w.script = [&](const RunRequest& r) -> Reply {
        if (++calls == 1) return {"", false, "max_turns reached", Failure::Limit, "sess-7"};
        EXPECT_EQ(r.resume_session.value_or(""), "sess-7");
        EXPECT_NE(r.prompt.find("Continue where you stopped"), std::string::npos);
        return {"<task_status>done</task_status>"};
    };
    Loop loop = w.loop();
    loop.iterate({});
    Plan p_t = w.plan();
    const TaskRef* t = p_t.by_id("long");
    EXPECT_EQ(t->task->status, TaskStatus::InProgress);
    EXPECT_TRUE(contains(t->task->notes, "stopped at a limit"));
    loop.iterate({});
    EXPECT_EQ(w.plan().by_id("long")->task->status, TaskStatus::Completed);
}

TEST(LoopFailures, RepeatedCrashesBlockOnTheOperator) {
    World w("- [ ] Fragile @dev ^fragile\n");
    w.script = [](const RunRequest&) { return Reply{"", false, "segfault", Failure::Crash}; };
    Loop loop = w.loop();
    LoopOptions o;
    o.max_failures = 3;
    loop.iterate(o);
    loop.iterate(o);
    EXPECT_EQ(w.plan().by_id("fragile")->task->status, TaskStatus::InProgress);
    loop.iterate(o);
    Plan p = w.plan();
    const TaskRef* t = p.by_id("fragile");
    EXPECT_EQ(t->task->status, TaskStatus::Blocked);
    ASSERT_EQ(t->task->blocked_by.size(), 1u);
    const TaskRef* op = p.by_id(t->task->blocked_by[0]);
    ASSERT_NE(op, nullptr);
    EXPECT_TRUE(is_operator_task(*op->task));
    EXPECT_NE(op->task->description.find("3 in a row"), std::string::npos);
}

TEST(LoopFailures, RepeatedNoReportBlocksOnTheOperator) {
    World w("- [ ] Quiet @dev ^quiet\n");
    w.script = [](const RunRequest&) { return Reply{"I did some things."}; };
    Loop loop = w.loop();
    for (int i = 0; i < 3; ++i) loop.iterate({});
    EXPECT_EQ(w.plan().by_id("quiet")->task->status, TaskStatus::Blocked);
}

TEST(Context, AsciiCriteria) {
    World w("- [ ] T @dev ^t\n  - ( ) Pending one\n  - (x) Done one\n  - ✓ Unicode one\n");
    Plan p = w.plan();
    AgentContext ctx = build_task_context({*w.team, *w.team->find("dev"), p, ""}, *p.by_id("t"));
    std::string s = ctx.render();
    EXPECT_NE(s.find("1. ( ) Pending one"), std::string::npos);
    EXPECT_NE(s.find("2. (x) Done one"), std::string::npos);
    EXPECT_NE(s.find("3. (x) Unicode one"), std::string::npos);
    EXPECT_NE(s.find("<blocker to=\"operator\">"), std::string::npos);
}

TEST(Unblock, ADeclinedBlockerHoldsWhatWaitedForADecision) {
    // A declined fetch must not release the check against what it would have fetched.
    TempDir d;
    d.write("PLAN.md", "# P\n\n- [-] Fetch the vendor PDF @operator ^fetch\n  - Declined (@operator 2026-11-08): the library is replaced\n"
                       "- [!] Check against the PDF @dev ^check\n  - blocked-by: ^fetch\n  - Blocked 2026-11-08 (@dev) by ^fetch: needs it\n"
                       "- [x] Get a browser @operator ^b\n- [!] Docs @dev ^docs\n  - blocked-by: ^b\n  - Blocked 2026-11-08 (@dev) by ^b: needs it\n");
    Plan p = Plan::load(d / "PLAN.md");
    EXPECT_TRUE(is_declined(*p.by_id("fetch")->task));
    EXPECT_FALSE(is_declined(*p.by_id("b")->task));
    PlanEditor ed(p);
    auto un = unblock_resolved(p, ed, {});
    ASSERT_TRUE(ed.commit().ok);
    ASSERT_EQ(un.size(), 1u);  // only ^docs
    Plan after = Plan::load(d / "PLAN.md");
    EXPECT_EQ(after.by_id("docs")->task->status, TaskStatus::Pending);
    const taskml::Task& c = *after.by_id("check")->task;
    EXPECT_EQ(c.status, TaskStatus::Blocked) << after.text();
    EXPECT_TRUE(c.blocked_by.empty());
    EXPECT_TRUE(blocked_without_link(c));  // the steward has it
    EXPECT_NE(after.text().find("^fetch declined, not done"), std::string::npos) << after.text();
}

TEST(Unblock, AFinishedTaskDropsItsFinishedBlockers) {
    // shop: ^v was done while ^b still blocked it on paper; when ^b is done, the link goes.
    TempDir d;
    d.write("PLAN.md", "# P\n\n- [x] Verify @qa ^v\n  - blocked-by: ^b\n- [x] Get a browser @operator ^b\n"
                       "- [ ] Docs @dev ^docs\n  - blocked-by: ^b\n");
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    unblock_resolved(p, ed, {});
    ASSERT_TRUE(ed.commit().ok);
    Plan after = Plan::load(d / "PLAN.md");
    EXPECT_TRUE(after.by_id("v")->task->blocked_by.empty()) << after.text();
    EXPECT_NE(after.text().find("No longer blocked by ^b"), std::string::npos);
    // An open task drops it too (a dead link reads as live);
    // a note keeps how the blocker was resolved.
    EXPECT_TRUE(after.by_id("docs")->task->blocked_by.empty()) << after.text();
    EXPECT_TRUE(contains(after.by_id("docs")->task->notes, "Blocker ^b done")) << after.text();
}

// A blocker link to a completed, cancelled or
// merged-as-duplicate task does not stay on the task that named it.
TEST(DeadBlockers, ACompletedBlockerLeavesTheTaskThatStillWaitsForAnother) {
    TempDir d;
    d.write("PLAN.md", "# P\n\n- [x] Routes @api ^pin\n- [ ] Caches @api ^clk\n"
                       "- [!] Schema page @dev ^page\n  - blocked-by: ^pin, ^clk\n  - Blocked 2026-11-08 (@dev) by ^pin: needs it\n");
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    std::vector<std::string> dropped;
    EXPECT_TRUE(unblock_resolved(p, ed, {}, &dropped).empty());
    ASSERT_TRUE(ed.commit().ok);
    Plan after = Plan::load(d / "PLAN.md");
    const taskml::Task& t = *after.by_id("page")->task;
    EXPECT_EQ(t.status, TaskStatus::Blocked);
    EXPECT_EQ(t.blocked_by, std::vector<std::string>{"clk"}) << after.text();
    ASSERT_EQ(dropped.size(), 1u);
    EXPECT_EQ(dropped[0], "^page: ^pin done");
    // When the last one is done, it is released and keeps no link.
    d.write("PLAN.md", "# P\n\n- [x] Routes @api ^pin\n- [x] Caches @api ^clk\n"
                       "- [!] Schema page @dev ^page\n  - blocked-by: ^clk\n  - Blocked 2026-11-08 (@dev) by ^pin: needs it\n");
    Plan p2 = Plan::load(d / "PLAN.md");
    PlanEditor ed2(p2);
    ASSERT_EQ(unblock_resolved(p2, ed2, {}).size(), 1u);
    ASSERT_TRUE(ed2.commit().ok);
    Plan a2 = Plan::load(d / "PLAN.md");
    EXPECT_EQ(a2.by_id("page")->task->status, TaskStatus::Pending);
    EXPECT_TRUE(a2.by_id("page")->task->blocked_by.empty()) << a2.text();
    EXPECT_TRUE(contains(a2.by_id("page")->task->notes, "^clk done"));
}

TEST(DeadBlockers, ACancelledBlockerIsDroppedAndReleasesTheTask) {
    TempDir d;
    d.write("PLAN.md", "# P\n\n- [-] Old check @qa ^old\n"
                       "- [!] Record @dev ^rec\n  - blocked-by: ^old\n  - Blocked 2026-11-08 (@dev) by ^old: needs it\n"
                       "- [~] Draft @dev ^draft\n  - blocked-by: ^old\n");
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    std::vector<std::string> dropped;
    ASSERT_EQ(unblock_resolved(p, ed, {}, &dropped).size(), 1u);
    ASSERT_TRUE(ed.commit().ok);
    Plan after = Plan::load(d / "PLAN.md");
    EXPECT_EQ(after.by_id("rec")->task->status, TaskStatus::Pending);
    EXPECT_TRUE(after.by_id("rec")->task->blocked_by.empty()) << after.text();
    EXPECT_TRUE(contains(after.by_id("rec")->task->notes, "^old cancelled"));
    EXPECT_TRUE(after.by_id("draft")->task->blocked_by.empty()) << after.text();
    EXPECT_EQ(after.by_id("draft")->task->status, TaskStatus::InProgress);
    EXPECT_EQ(dropped, (std::vector<std::string>{"^rec: ^old cancelled", "^draft: ^old cancelled"}));
}

TEST(DeadBlockers, ABlockerMergedAsADuplicateHandsTheLinkToItsLiveTwin) {
    // A blocker closed as a duplicate of a task still open does not unblock as if the work
    // were done.
    TempDir d;
    d.write("PLAN.md", "# P\n\n- [-] Cache page @api ^cache-2\n  - Duplicate of ^cache (@scrum-master 2026-11-08)\n"
                       "- [ ] Cache page @api ^cache\n"
                       "- [!] Schema @dev ^schema\n  - blocked-by: ^cache-2\n  - Blocked 2026-11-08 (@dev) by ^cache-2: needs it\n"
                       "- [ ] Later @dev ^later\n  - depends: ^cache-2\n");
    Plan p = Plan::load(d / "PLAN.md");
    // Read side: a dependency on the cancelled copy is not met while its twin is open.
    EXPECT_EQ(p.unmet_dependencies(*p.by_id("later")), std::vector<std::string>{"cache-2"});
    EXPECT_EQ(p.active_blockers(*p.by_id("schema")), std::vector<std::string>{"cache-2"});
    EXPECT_EQ(follow_duplicates(p, "cache-2"), "cache");
    PlanEditor ed(p);
    std::vector<std::string> dropped;
    EXPECT_TRUE(unblock_resolved(p, ed, {}, &dropped).empty());
    ASSERT_TRUE(ed.commit().ok);
    Plan after = Plan::load(d / "PLAN.md");
    const taskml::Task& b = *after.by_id("schema")->task;
    EXPECT_EQ(b.status, TaskStatus::Blocked) << after.text();
    EXPECT_EQ(b.blocked_by, std::vector<std::string>{"cache"}) << after.text();
    EXPECT_TRUE(contains(b.notes, "Blocker ^cache-2 merged into ^cache")) << after.text();
    EXPECT_EQ(dropped, std::vector<std::string>{"^schema: ^cache-2 merged into ^cache, now blocked by ^cache"});

    // The twin done: released, no link left.
    d.write("PLAN.md", "# P\n\n- [-] Cache page @api ^cache-2\n  - Merged into ^cache\n- [x] Cache page @api ^cache\n"
                       "- [!] Schema @dev ^schema\n  - blocked-by: ^cache-2\n  - Blocked 2026-11-08 (@dev) by ^cache-2: needs it\n");
    Plan p2 = Plan::load(d / "PLAN.md");
    PlanEditor ed2(p2);
    ASSERT_EQ(unblock_resolved(p2, ed2, {}).size(), 1u);
    ASSERT_TRUE(ed2.commit().ok);
    Plan a2 = Plan::load(d / "PLAN.md");
    EXPECT_TRUE(a2.by_id("schema")->task->blocked_by.empty()) << a2.text();
    EXPECT_TRUE(contains(a2.by_id("schema")->task->notes, "^cache-2 merged into ^cache (done)")) << a2.text();

    // Two copies closed as each other's duplicate: no live copy, so not released.
    d.write("PLAN.md", "# P\n\n- [-] A @api ^a\n  - Duplicate of ^b\n- [-] A @api ^b\n  - Duplicate of ^a\n"
                       "- [!] Schema @dev ^schema\n  - blocked-by: ^a\n  - Blocked 2026-11-08 (@dev) by ^a: needs it\n");
    Plan p3 = Plan::load(d / "PLAN.md");
    PlanEditor ed3(p3);
    EXPECT_TRUE(unblock_resolved(p3, ed3, {}).empty());
    ASSERT_TRUE(ed3.commit().ok);
    Plan a3 = Plan::load(d / "PLAN.md");
    EXPECT_EQ(a3.by_id("schema")->task->status, TaskStatus::Blocked);
    EXPECT_TRUE(blocked_without_link(*a3.by_id("schema")->task)) << a3.text();  // the steward has it
    EXPECT_NE(a3.text().find("no live copy"), std::string::npos) << a3.text();
}

TEST(DeadBlockers, ABlockerNamedByItsDuplicateLinksTheLiveTwin) {
    World w("- [-] Cache page @api ^cache-2\n  - Duplicate of ^cache\n- [ ] Cache page @api ^cache\n- [~] Schema @dev ^schema\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    BlockerReport br;
    br.kind = BlockerReport::Kind::Task;
    br.on = "cache-2";
    br.text = "needs the cache page";
    record_blockers(p, ed, *p.by_id("schema"), *w.team->find("dev"), *w.team, {br}, "");
    ASSERT_TRUE(ed.commit().ok);
    Plan q = w.plan();
    EXPECT_EQ(q.by_id("schema")->task->blocked_by, std::vector<std::string>{"cache"}) << q.text();
}

TEST(DeadBlockers, TheLoopSweepsOldDeadLinksAndPutsEachOnRecord) {
    // The first step after an upgrade is the one-off sweep of the links already in the plan.
    World w("- [x] Routes @dev ^pin\n- [-] Old @dev ^old\n"
            "- [ ] Schema @dev ^schema\n  - blocked-by: ^pin, ^old\n");
    w.script = [](const RunRequest&) -> Reply { return {"<task_status>done</task_status>"}; };
    Loop loop = w.loop();
    IterationResult r = loop.iterate({});
    EXPECT_TRUE(contains(r.notes, "dropped blocker link ^schema: ^pin done")) << join(r.notes, "\n");
    EXPECT_TRUE(contains(r.notes, "dropped blocker link ^schema: ^old cancelled"));
    const std::string log = w.dir.read("proj/CHANGELOG.md");
    EXPECT_NE(log.find("Dead blocker links dropped\n\n- ^schema: ^pin done\n- ^schema: ^old cancelled\n"), std::string::npos) << log;
    EXPECT_TRUE(w.plan().by_id("schema")->task->blocked_by.empty()) << w.text();
}

TEST(Blockers, ALongRequestKeepsItsLines) {
    // toolkit's 14-line text for the operator lost its last lines.
    World w("## Work\n\n- [~] Skill @dev ^skill\n");
    std::string text = "Replace step 4 with the text below:";
    for (int i = 1; i <= 45; ++i) text += "\nline " + std::to_string(i);
    Plan p = w.plan();
    PlanEditor ed(p);
    std::vector<BlockerReport> bs{{BlockerReport::Kind::Operator, "", "", text}};
    record_blockers(p, ed, *p.by_id("skill"), *w.team->find("dev"), *w.team, bs, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    const std::string out = w.text();
    EXPECT_NE(out.find("line 14\n"), std::string::npos) << out;
    EXPECT_NE(out.find("line 40\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("line 41\n"), std::string::npos) << out;
    EXPECT_NE(out.find("5 more lines in the sender's report"), std::string::npos) << out;
}

TEST(PlanEdit, ALinkThatClosesALongCycleIsNotMade) {
    // A cycle through several tasks: the link is left out, not the whole edit refused.
    Plan p = Plan::from_text("# P\n\n- [ ] A ^a\n- [ ] B ^b\n  - depends: ^c\n- [ ] C ^c\n  - depends: ^a\n- [ ] D ^d\n");
    PlanEditor ed(p);
    ed.add_depends(*p.by_id("a"), "b");  // b -> c -> a: a cycle
    ed.add_depends(*p.by_id("a"), "d");  // fine
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok()) << ed.text();
    EXPECT_EQ(q.by_id("a")->task->depends_on, std::vector<std::string>{"d"});
    const auto& n = q.by_id("a")->task->notes;
    EXPECT_TRUE(std::any_of(n.begin(), n.end(), [](const std::string& x) { return x.find("Not made to wait for ^b") != std::string::npos; }));
}

// --- The twin check on escalations and findings ----------------------------------------

TEST(Twins, AnEscalationToATeammateWithTheJobOpenWaitsForIt) {
    // A blocker to a member, the product owner or another team made a new task every time,
    // even when the same job was open for them (only the operator's were checked).
    World w("## Work\n\n- [~] Upload results #p1 @dev ^upload\n- [ ] Decide REST or gRPC @arch ^decide\n"
            "- [ ] Ask the customer: Which formats are needed? @po #customer-question ^ask\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    std::vector<BlockerReport> bs(2);
    bs[0] = {BlockerReport::Kind::Member, "", "arch", "Decide REST or gRPC."};
    bs[1] = {BlockerReport::Kind::Customer, "", "", "Which formats are needed"};
    BlockerEdits be = record_blockers(p, ed, *p.by_id("upload"), *w.team->find("dev"), *w.team, bs, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    EXPECT_EQ(ed.twin_merges().size(), 2u);
    Plan q = w.plan();
    EXPECT_EQ(q.tasks().size(), p.tasks().size()) << w.text();
    EXPECT_EQ(q.by_id("upload")->task->blocked_by, (std::vector<std::string>{"decide", "ask"}));
    EXPECT_TRUE(contains(q.by_id("decide")->task->notes, "Filed again"));
}

TEST(Twins, AnEscalationThatIsTheBlockedTaskItselfIsNotALink) {
    World w("## Work\n\n- [~] Decide REST or gRPC @arch ^decide\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    std::vector<BlockerReport> bs{{BlockerReport::Kind::Member, "", "arch", "Decide REST or gRPC"}};
    BlockerEdits be = record_blockers(p, ed, *p.by_id("decide"), *w.team->find("arch"), *w.team, bs, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    EXPECT_TRUE(w.plan().by_id("decide")->task->blocked_by.empty()) << w.text();
    EXPECT_TRUE(contains(be.problems, "itself"));
}

TEST(Twins, AFindingForAnotherTeamWithTheJobOpenIsNotFiledAgain) {
    World w("- [~] Schema @dev #p1 #sprint-1 ^b\n- [ ] toolkit: no YAML interface type! @toolkit ^yaml\n");
    Team t = *w.team;
    t.external["toolkit"] = "the build tool team";
    Plan p = w.plan();
    PlanEditor ed(p);
    EXPECT_EQ(record_finding(p, ed, *p.by_id("b"), *t.find("dev"), "@toolkit: toolkit no YAML interface type", nullptr, &t), "");
    EXPECT_EQ(p.tasks().size(), 2u);
}

TEST(LoopFailures, ClaudesPlanLimitIsARateLimit) {
    // Not a crash, which would make "find out why runs fail" tasks for the operator.
    EXPECT_EQ(classify_failure(1, false, "You've hit your 5-hour limit \xC2\xB7 resets 6:30pm (UTC)", "", ""),
              Failure::RateLimit);
    EXPECT_EQ(classify_failure(1, false, "", "", "Claude usage limit reached. Your limit will reset at 7pm"), Failure::RateLimit);
    EXPECT_EQ(classify_failure(1, false, "segfault", "", ""), Failure::Crash);
}

TEST(Blockers, ABlockerOnATaskNoOneMadeIsFiledForTheTeam) {
    // A blocker on an id no task has is the team's work, not an operator task.
    World w("## Work\n\n- [~] Choose the rate limit @dev ^limit\n");
    Plan p = w.plan();
    PlanEditor ed(p);
    std::vector<BlockerReport> bs{{BlockerReport::Kind::Task, "v9-prot-ttl-tran", "", "Needs the V9_PROT maximum from the tran run"}};
    BlockerEdits be = record_blockers(p, ed, *p.by_id("limit"), *w.team->find("dev"), *w.team, bs, "");
    ASSERT_TRUE(ed.commit().ok) << ed.text();
    Plan q = w.plan();
    const TaskRef* t = q.by_id("v9-prot-ttl-tran");
    ASSERT_NE(t, nullptr) << w.text();
    EXPECT_FALSE(t->task->assignee.has_value()) << w.text();  // the team's, not the operator's
    EXPECT_EQ(q.by_id("limit")->task->blocked_by, std::vector<std::string>{"v9-prot-ttl-tran"});
}

// Dependents hear when a gating task closes.
TEST(Dependents, EachOpenDependentHearsHowItsDependencyClosed) {
    TempDir d;
    d.write("PLAN.md",
            "# P\n\n- [x] Pick the Orm @api ^orm\n- [-] Old idea @api ^old\n- [-] Twin @api ^twin\n  - Duplicate of ^orm\n"
            "- [ ] Layout @api ^layout\n  - depends: ^orm, ^old, ^twin\n"
            "- [ ] Docs @api ^docs\n  - depends: ^orm\n  - blocked-by: ^orm\n"
            "- [x] Done already @api ^fin\n  - depends: ^orm\n- [ ] Waits @api ^w\n  - depends: ^open\n- [ ] Open @api ^open\n");
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    unblock_resolved(p, ed, {{"orm", "@api: the OAuth"}});
    ASSERT_TRUE(ed.commit().ok);
    Plan after = Plan::load(d / "PLAN.md");
    const auto& n = after.by_id("layout")->task->notes;
    EXPECT_TRUE(contains(n, "Dependency ^orm done (")) << after.text();
    EXPECT_TRUE(contains(n, ": @api: the OAuth")) << after.text();
    EXPECT_TRUE(contains(n, "Dependency ^old cancelled (")) << after.text();
    EXPECT_TRUE(contains(n, "Dependency ^twin merged into ^orm (")) << after.text();
    EXPECT_TRUE(contains(n, "check that this task still applies")) << after.text();
    // Its blocker note says it already; a finished task and one whose dependency is open get none.
    for (const char* id : {"docs", "fin", "w"})
        EXPECT_FALSE(contains(after.by_id(id)->task->notes, "Dependency ^")) << id << "\n" << after.text();
    EXPECT_TRUE(contains(after.by_id("docs")->task->notes, "Blocker ^orm done"));
    // Once: the next look (every path calls unblock_resolved again) adds nothing.
    PlanEditor ed2(after);
    EXPECT_TRUE(note_closed_dependencies(after, ed2).empty());
    EXPECT_FALSE(ed2.changed());
}
