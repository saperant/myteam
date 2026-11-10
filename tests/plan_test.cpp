#include <gtest/gtest.h>

#include "myteam/plan.hpp"
#include "test_util.hpp"

using namespace myteam;
using taskml::TaskStatus;
using myteam::test::TempDir;

namespace {

const char* kPlan = R"(---
project: Demo
---

# Demo

Some prose that must survive edits.

## Backend

- [x] Design #p0 @systems ^design
- [ ] Build API #p1 @builder ^api
  - depends: ^design
  - A note
  - [ ] Endpoints @builder
- [ ] Write tests @verifier
  - depends: ^api

## Later

- [ ] Docs
  - blocked-by: ^api
)";

}  // namespace

TEST(Plan, IndexesTasks) {
    Plan p = Plan::from_text(kPlan);
    ASSERT_TRUE(p.ok());
    ASSERT_EQ(p.tasks().size(), 5u);
    const TaskRef* api = p.find("^api");
    ASSERT_NE(api, nullptr);
    EXPECT_EQ(api->sections, (std::vector<std::string>{"Demo", "Backend"}));
    EXPECT_EQ(p.find("api"), api);
    EXPECT_EQ(p.find(std::to_string(api->task->line)), api);
    const TaskRef* endpoints = p.find("L15");
    ASSERT_NE(endpoints, nullptr);
    EXPECT_EQ(endpoints->parents.size(), 1u);
    EXPECT_EQ(endpoints->key(), "L15");
    EXPECT_TRUE(p.ready(*api));
    const TaskRef* tests = p.find("L16");
    EXPECT_FALSE(p.ready(*tests));
    EXPECT_EQ(p.unmet_dependencies(*tests), (std::vector<std::string>{"api"}));
    EXPECT_EQ(p.dependents(*api).size(), 2u);
    EXPECT_EQ(p.directive("project"), "Demo");
}

TEST(Plan, SemanticErrorsCount) {
    Plan p = Plan::from_text("- [ ] a ^x\n  - depends: ^nope\n");
    EXPECT_FALSE(p.ok());
}

TEST(PlanEditor, StatusAssigneeIdAndNotes) {
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    ed.set_status(*p.find("^api"), TaskStatus::InProgress);
    ed.set_assignee(*p.find("^api"), "@fixer");
    ed.set_assignee(*p.find("L21"), "scribe");  // Docs: had none
    std::string id = ed.ensure_id(*p.find("L21"));
    EXPECT_EQ(id, "docs");
    ed.add_note(*p.find("^api"), "Started by fixer");
    ASSERT_TRUE(ed.check().ok);
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok());
    const TaskRef* api = q.find("^api");
    EXPECT_EQ(api->task->status, TaskStatus::InProgress);
    EXPECT_EQ(*api->task->assignee, "fixer");
    EXPECT_EQ(api->task->notes, (std::vector<std::string>{"A note", "Started by fixer"}));
    EXPECT_EQ(api->task->subtasks.size(), 1u);
    EXPECT_EQ(*q.find("^docs")->task->assignee, "scribe");
    EXPECT_NE(ed.text().find("Some prose that must survive edits."), std::string::npos);
}

TEST(PlanEditor, AddsTasks) {
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    NewTask t;
    t.description = "Test the #api endpoints";
    t.assignee = "verifier";
    t.priority = 1;
    t.depends_on = {"api"};
    t.notes = {"Cover errors", "Handed off by @builder"};
    std::string sib = ed.add_sibling(*p.find("^api"), t);
    NewTask sub;
    sub.description = "Endpoints";  // id collides with nothing; "endpoints"
    std::string child = ed.add_subtask(*p.find("^api"), sub);
    sub.assignee = "verifier";  // another assignee: not a twin, and the id is taken
    std::string again = ed.add_subtask(*p.find("^api"), sub);
    EXPECT_EQ(sib, "test-the-api-endpoints");
    EXPECT_EQ(child, "endpoints");
    EXPECT_EQ(again, "endpoints-2");
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok()) << ed.text();
    const TaskRef* s = q.by_id(sib);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->task->description, "Test the #api endpoints");  // escaped, not a tag
    EXPECT_TRUE(s->task->tags.empty());
    EXPECT_EQ(s->task->depends_on, (std::vector<std::string>{"api"}));
    EXPECT_EQ(s->task->notes.size(), 2u);
    EXPECT_EQ(s->depth, 0);
    const TaskRef* api = q.by_id("api");
    ASSERT_EQ(api->task->subtasks.size(), 3u);
    EXPECT_EQ(api->task->subtasks[1].id.value_or(""), "endpoints");
    // The sibling comes right after ^api's block, before "Write tests".
    const auto& backend = q.doc().sections[1].tasks;
    ASSERT_EQ(backend.size(), 4u);
    EXPECT_EQ(backend[2].id.value_or(""), sib);
}

TEST(PlanEditor, RefusesBreakingEdits) {
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    NewTask t;
    t.description = "Waits on nothing real";
    t.depends_on = {"no-such-task"};  // an unknown dependency breaks the plan
    ed.add_sibling(*p.find("^api"), t);
    CommitResult r = ed.check();
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.diagnostics.empty());
}

TEST(PlanEditor, ATakenIdGetsTheNextFreeNumber) {
    // A duplicate id does not refuse the whole edit.
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    NewTask t;
    t.description = "Dup";
    t.id = "api";
    EXPECT_EQ(ed.add_sibling(*p.find("^api"), t), "api-2");
    EXPECT_TRUE(ed.check().ok);
}

TEST(PlanEditor, CommitWritesAndDetectsConflicts) {
    TempDir d;
    d.write("PLAN.md", kPlan);
    Plan p = Plan::load(d / "PLAN.md");
    PlanEditor ed(p);
    ed.set_status(*p.find("^api"), TaskStatus::Completed);
    ASSERT_TRUE(ed.commit().ok);
    EXPECT_EQ(Plan::load(d / "PLAN.md").find("^api")->task->status, TaskStatus::Completed);

    Plan stale = Plan::from_text(kPlan, taskml::Dialect::Markdown, d / "PLAN.md");
    PlanEditor ed2(stale);
    ed2.set_status(*stale.find("^api"), TaskStatus::Blocked);
    CommitResult r = ed2.commit();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("changed on disk"), std::string::npos);
}

TEST(PlanEditor, TaskMLDialect) {
    Plan p = Plan::from_text("[ ] Top @builder ^top\n    [ ] Child\n", taskml::Dialect::TaskML);
    PlanEditor ed(p);
    NewTask t;
    t.description = "Next";
    t.depends_on = {"top"};
    ed.add_sibling(*p.find("^top"), t);
    ed.set_status(*p.find("^top"), TaskStatus::Completed);
    Plan q = Plan::from_text(ed.text(), taskml::Dialect::TaskML);
    ASSERT_TRUE(q.ok()) << ed.text();
    EXPECT_EQ(q.tasks().size(), 3u);
    EXPECT_EQ(q.by_id("next")->task->depends_on.size(), 1u);
}

// --- The twin check: every add_* (so every path that makes a task) ---------------------

TEST(Twins, NormalizedTitle) {
    EXPECT_EQ(normalized_title("  Fix: the .pyc  commit! "), "fix the pyc commit");
    EXPECT_EQ(normalized_title("Fix the PYC commit"), "fix the pyc commit");
    EXPECT_EQ(normalized_title("---"), "");
}

TEST(Twins, AnOpenTaskWithTheSameTitleAndAssigneeTakesTheNewOne) {
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    NewTask t;
    t.description = "build api!";  // ^api's title, other case and punctuation
    t.assignee = "@builder";
    t.notes = {"Handed off by @fixer"};
    EXPECT_EQ(ed.add_to_section("Later", t), "api");
    EXPECT_EQ(ed.add_sibling(*p.find("^design"), t), "api");
    EXPECT_EQ(ed.add_subtask(*p.find("^design"), t), "api");
    ASSERT_EQ(ed.twin_merges().size(), 3u);
    EXPECT_EQ(ed.twin_merges()[0].into, "api");
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok()) << ed.text();
    EXPECT_EQ(q.tasks().size(), p.tasks().size()) << ed.text();
    EXPECT_NE(q.by_id("api")->task->notes.back().find("Filed again"), std::string::npos);
    EXPECT_NE(q.by_id("api")->task->notes.back().find("Handed off by @fixer"), std::string::npos);

    // Another assignee, a finished task, a finished new one: not twins.
    PlanEditor ed2(p);
    t.assignee = "fixer";
    EXPECT_NE(ed2.add_to_section("Later", t), "api");
    NewTask d;
    d.description = "Design";  // ^design is done
    d.assignee = "systems";
    EXPECT_NE(ed2.add_to_section("Later", d), "design");
    NewTask done = t;
    done.assignee = "builder";
    done.status = TaskStatus::Completed;
    EXPECT_NE(ed2.add_to_section("Later", done), "api");
    EXPECT_TRUE(ed2.twin_merges().empty());
}

TEST(Twins, TheSameTaskTwiceInOneEditIsAddedOnce) {
    Plan p = Plan::from_text(kPlan);
    PlanEditor ed(p);
    NewTask t;
    t.description = "Write the release notes";
    t.assignee = "fixer";
    const std::string a = ed.add_to_section("Later", t);
    EXPECT_EQ(ed.add_to_section("Backend", t), a);
    Plan q = Plan::from_text(ed.text());
    ASSERT_TRUE(q.ok());
    EXPECT_EQ(q.tasks().size(), p.tasks().size() + 1);
}

TEST(Twins, AnOrgDispatchOfATaskTheUnitHasOpenGoesToIt) {
    // The org planner's dispatch and carry: a task without an assignee, with an id of its own,
    // into the unit's backlog. The unit has it open already (carried earlier as ^from-org-schema).
    Plan p = Plan::from_text("# U\n\n## Backlog\n\n- [ ] Design the schema #org ^from-org-schema\n");
    PlanEditor ed(p);
    NewTask nt;
    nt.description = "Design the schema";
    nt.id = "org-schema";
    nt.tags = {"org"};
    nt.notes = {"From the org, org sprint 2 (^schema in the org plan): the org needs it this sprint"};
    EXPECT_EQ(ed.add_to_section("Backlog", nt), "from-org-schema");
    Plan q = Plan::from_text(ed.text());
    EXPECT_EQ(q.by_id("org-schema"), nullptr) << ed.text();
}

TEST(Twins, TheCleanupMergesOldTwinsOneLineEach) {
    Plan p = Plan::from_text("# P\n\n## Sprint 4\n\n"
                             "- [ ] Dedup on title @maint ^dedup\n"
                             "- [~] dedup on title. @maint ^dedup-2\n"
                             "  - depends: ^dedup\n"
                             "- [ ] Dedup on title @helmi ^other\n"
                             "- [x] Dedup on title @maint ^old\n"
                             "- [ ] Use it @dev ^use\n"
                             "  - blocked-by: ^dedup\n"
                             "- [ ] Dedup on title @maint\n");
    ASSERT_TRUE(p.ok());
    PlanEditor ed(p);
    auto m = merge_open_twins(p, ed);
    ASSERT_EQ(m.size(), 2u);
    // The one in progress stays.
    EXPECT_EQ(twin_cleanup_line(m[0]), "^dedup -> ^dedup-2  Dedup on title");
    EXPECT_EQ(m[1].into, "dedup-2");
    ASSERT_TRUE(ed.check().ok) << ed.text();
    Plan q = Plan::from_text(ed.text());
    EXPECT_EQ(q.by_id("dedup")->task->status, TaskStatus::Cancelled);
    EXPECT_EQ(duplicate_of(*q.by_id("dedup")->task), "dedup-2");
    EXPECT_EQ(q.by_id("other")->task->status, TaskStatus::Pending);  // another assignee
    EXPECT_EQ(q.by_id("old")->task->status, TaskStatus::Completed);
    // The kept one no longer waits for its twin; what waited for the twin waits for the kept one.
    EXPECT_TRUE(q.by_id("dedup-2")->task->depends_on.empty()) << ed.text();
    EXPECT_EQ(q.active_blockers(*q.by_id("use")), std::vector<std::string>{"dedup"});
    EXPECT_EQ(follow_duplicates(q, "dedup"), "dedup-2");
    EXPECT_NE(twin_cleanup_entry(m).find("- ^dedup -> ^dedup-2  Dedup on title"), std::string::npos);
    // Nothing left to merge.
    PlanEditor again(q);
    EXPECT_TRUE(merge_open_twins(q, again).empty());
}
