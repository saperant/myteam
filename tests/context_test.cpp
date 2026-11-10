#include <gtest/gtest.h>

#include "myteam/context.hpp"
#include "myteam/project.hpp"
#include "test_util.hpp"

using namespace myteam;

namespace {

Team team() {
    Team t;
    t.id = "alpha";
    t.name = "Alpha";
    t.product_owner = "chief";
    auto m = [](std::string h, std::string name, std::string role, std::string summary) {
        Member x;
        x.handle = h;
        x.name = name;
        x.role = role;
        x.summary = summary;
        x.models.standard = {"claude", "sonnet"};
        return x;
    };
    t.members = {m("chief", "Chief", "product-owner", "prioritizes"), m("builder", "Builder", "implementer", "writes code"),
                 m("verifier", "Verifier", "qa-tester", "tests"), m("scribe", "Scribe", "docs", "writes docs")};
    t.members[1].prompt = "Write safe code.";
    t.members[1].skills = {"cpp"};
    t.members[1].handoffs = {{"code is done", "verifier", "verify"}};
    return t;
}

const char* kPlan = R"(---
project: Demo
sprint: Sprint 3
goal: Ship the API
---

## Backend

- [x] Design #p0 @systems ^design
- [ ] Build API #p1 ~4h @builder ^api
  - depends: ^design
  - Use REST
  - [x] Schema
  - [ ] Endpoints @builder ^endpoints
  - ✓ Returns JSON
- [ ] Write tests @verifier ^tests
  - depends: ^api
- [ ] Unrelated secret work @scribe ^secret

## Other

- [ ] Something else entirely ^other
)";

}  // namespace

TEST(Context, ContainsOnlyWhatTheTaskNeeds) {
    Team t = team();
    Plan p = Plan::from_text(kPlan);
    AgentContext ctx = build_task_context({t, *t.find("builder"), p, "A small demo project."}, *p.find("^api"));
    std::string s = ctx.render();

    // Role
    EXPECT_NE(s.find("You are Builder (@builder), the implementer of team Alpha. Your skills: cpp."), std::string::npos);
    EXPECT_NE(s.find("Write safe code."), std::string::npos);
    // Project
    EXPECT_NE(s.find("A small demo project."), std::string::npos);
    // Task with its own neighbourhood
    EXPECT_NE(s.find("**Build API** (^api)"), std::string::npos);
    EXPECT_NE(s.find("Sprint goal: Ship the API"), std::string::npos);
    EXPECT_NE(s.find("priority: p1"), std::string::npos);
    EXPECT_NE(s.find("- section: Backend"), std::string::npos);
    EXPECT_NE(s.find("- Use REST"), std::string::npos);
    EXPECT_NE(s.find("- [x] Schema"), std::string::npos);
    EXPECT_NE(s.find("- [ ] Endpoints (^endpoints, @builder)"), std::string::npos);
    EXPECT_NE(s.find("(x) Returns JSON"), std::string::npos);  // ASCII, whatever the plan uses
    EXPECT_NE(s.find("Depends on:\n- Design (^design, @systems, completed)"), std::string::npos);
    EXPECT_NE(s.find("Waiting for this task:\n- Write tests (^tests, @verifier, pending)"), std::string::npos);
    // Handoffs: only the members the rules lead to
    EXPECT_NE(s.find("code is done → Verifier (@verifier): verify"), std::string::npos);
    EXPECT_NE(s.find("@verifier — Verifier, qa-tester: tests"), std::string::npos);
    // Reporting contract
    EXPECT_NE(s.find("<task_status>"), std::string::npos);
    EXPECT_NE(s.find("Do not edit the plan file"), std::string::npos);

    // Filtered out
    EXPECT_EQ(s.find("Unrelated secret work"), std::string::npos);
    EXPECT_EQ(s.find("Something else entirely"), std::string::npos);
    EXPECT_EQ(s.find("Scribe"), std::string::npos);
    // The PO appears only as the customer's contact, in Reporting.
    EXPECT_EQ(ctx.section("teammates")->body.find("Chief"), std::string::npos);
    EXPECT_NE(ctx.section("reporting")->body.find("only to Chief (@chief)"), std::string::npos);

    EXPECT_EQ(ctx.sections.size(), 5u);
    EXPECT_GT(ctx.estimated_tokens(), 100u);
    EXPECT_LT(ctx.estimated_tokens(), 830u);  // (800 until <web_access>)
}

TEST(Context, NoHandoffRulesPointsToFacilitator) {
    Team t = team();
    Plan p = Plan::from_text(kPlan);
    AgentContext ctx = build_task_context({t, *t.find("verifier"), p, ""}, *p.find("^tests"));
    const ContextSection* s = ctx.section("teammates");
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->body.find("hand off to Chief (@chief, product-owner)"), std::string::npos);
    EXPECT_EQ(ctx.render().find("# The project"), std::string::npos);  // empty brief: no section
}

TEST(Context, Repair) {
    Team t = team();
    Plan p = Plan::from_text("- [ ] a ^x\n- [ ] b ^x\n");
    AgentContext ctx = build_repair_context({t, *t.find("builder"), p, ""}, p.errors());
    std::string s = ctx.render();
    EXPECT_NE(s.find("E209"), std::string::npos);
    EXPECT_NE(s.find("This is the one task where you edit the plan file"), std::string::npos);
}

// The brief no longer says another team cannot get a handoff (^the-task-notes-say-for).
TEST(Context, OtherTeamsCanGetAHandoff) {
    Team t = team();
    t.external["web"] = "the web front end";
    Plan p = Plan::from_text(kPlan);
    std::string s = build_task_context({t, *t.find("builder"), p, "A small demo project."}, *p.find("^api")).render();
    EXPECT_EQ(s.find("you cannot hand off to them"), std::string::npos);
    EXPECT_NE(s.find("Other teams of the org (not your teammates):\n- @web — the web front end"), std::string::npos) << s;
    EXPECT_NE(s.find("<handoff to=\"web\"> (or plan_handoff): the org carries it, and your task is not blocked."),
              std::string::npos)
        << s;
}

// `myteam context` and `myteam run` give a task the epics its "epic: ^id" lines link, with their
// "Teams:" line, from the epics view myorg writes; an epic the task does not link stays out.
