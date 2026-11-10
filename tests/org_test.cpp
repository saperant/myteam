#include <gtest/gtest.h>

#include <algorithm>

#include "myorg/org.hpp"
#include "myteam/process.hpp"
#include "test_util.hpp"

using namespace myorg;
using myteam::test::TempDir;

namespace {

// A team for the crew, outside the org: a coach, a Gatekeeper.
std::string crew_team(const TempDir& t) {
    t.write("crew/team.md", "---\nid: crew\nname: Crew\ncoach: coach\ngatekeeper: gatekeeper\nmodels: {default: claude:sonnet}\n---\n");
    t.write("crew/members/coach.md", "---\nname: Coach\nrole: coach\n---\nDecide.\n");
    t.write("crew/members/gatekeeper.md", "---\nname: Gatekeeper\nrole: gatekeeper\n---\nDoubt.\n");
    return (t.path() / "crew").string();
}

const char* kOrg = R"(---
name: suite
rules:
  - "Nothing is installed."
units:
  api:
    purpose: "the HTTP API"
    owns: [interfaces/schema]
    rules: ["The API is built with toolkit."]
  web:
    purpose: "the web front end"
  cli:
    purpose: "the command-line client"
  toolkit:
    purpose: "the build tool api works in"
    type: platform
    reads: [../api]
interactions:
  - between: [api, web]
    mode: collaboration
    provider: web
    until: 1
  - between: [api, toolkit]
    mode: x-as-a-service
    provider: toolkit
---

# suite

The system.
)";

int git(const fs::path& d, std::vector<std::string> args, std::string* out = nullptr) {
    myteam::ProcessOptions o;
    o.argv = {"git", "-C", d.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    std::string text;
    int rc = myteam::run_process(o, [&](std::string_view l) { text += std::string(l) + "\n"; }).exit_code;
    if (out) *out = text;
    return rc;
}

void identity(const fs::path& d) {
    git(d, {"config", "user.name", "Test"});
    git(d, {"config", "user.email", "test@example.invalid"});
}

}  // namespace

TEST(OrgSpec, ParsesUnitsRulesAndInteractions) {
    OrgSpec s = parse_org_spec(kOrg);
    EXPECT_EQ(s.name, "suite");
    ASSERT_EQ(s.units.size(), 4u);
    EXPECT_TRUE(check_org_spec(s).empty());
    EXPECT_EQ(s.interactions[0].mode_in(1), "collaboration");
    EXPECT_EQ(s.interactions[0].mode_in(2), "x-as-a-service");  // until milestone 1
    OrgSpec again = parse_org_spec(render_org_spec(s));
    EXPECT_EQ(again.units.size(), 4u);
    EXPECT_EQ(again.interactions[0].until, 1);
}

TEST(OrgViews, BoundariesFollowTheInteractions) {
    TempDir d;
    for (const char* u : {"api", "web", "cli", "toolkit"}) fs::create_directories(d / u);
    OrgSpec s = parse_org_spec(kOrg);
    UnitView api = unit_view(s, "api", 1, d.path());
    // Collaboration opens web's repository; the others stay hidden.
    EXPECT_NE(std::find(api.readable.begin(), api.readable.end(), "../web"), api.readable.end());
    EXPECT_NE(std::find(api.hide.begin(), api.hide.end(), "../cli"), api.hide.end());
    EXPECT_NE(std::find(api.hide.begin(), api.hide.end(), "../toolkit"), api.hide.end());
    EXPECT_EQ(std::find(api.hide.begin(), api.hide.end(), "../web"), api.hide.end());
    EXPECT_EQ(api.writable, (std::vector<std::string>{"../interfaces/schema"}));
    EXPECT_EQ(api.rules, (std::vector<std::string>{"Nothing is installed.", "The API is built with toolkit."}));
    ASSERT_EQ(api.notes.size(), 2u);
    EXPECT_NE(api.notes[1].find("x-as-a-service. Use what they provide"), std::string::npos) << api.notes[1];
    // After milestone 1 the collaboration ends: web is hidden too.
    UnitView later = unit_view(s, "api", 2, d.path());
    EXPECT_NE(std::find(later.hide.begin(), later.hide.end(), "../web"), later.hide.end());
    // The provider hears that others use it; it reads api's work (reads:), which is not hidden.
    UnitView toolkit = unit_view(s, "toolkit", 1, d.path());
    EXPECT_EQ(std::find(toolkit.hide.begin(), toolkit.hide.end(), "../api"), toolkit.hide.end());
    EXPECT_NE(std::find(toolkit.hide.begin(), toolkit.hide.end(), "../cli"), toolkit.hide.end());
    ASSERT_EQ(toolkit.notes.size(), 1u);
    EXPECT_NE(toolkit.notes[0].find("They use what you provide"), std::string::npos);
}

TEST(OrgInit, MakesACrewAndSyncMakesTheUnits) {
    TempDir d, teams;
    EXPECT_THROW(init_org(d.path(), {}), std::runtime_error);  // no team
    auto did = init_org(d.path(), {.team = crew_team(teams)});
    EXPECT_FALSE(did.empty());
    identity(d.path());
    const std::string cfg = d.read(".myteam/config.yaml");
    EXPECT_NE(cfg.find("methodology: coach"), std::string::npos) << cfg;
    EXPECT_NE(cfg.find("brief: ORG.md"), std::string::npos) << cfg;
    EXPECT_NE(cfg.find("team: " + crew_team(teams)), std::string::npos) << cfg;
    d.write("ORG.md", kOrg);
    SyncResult r = sync_org(d.path(), 1);
    EXPECT_TRUE(r.problems.empty()) << r.problems.front();
    EXPECT_EQ(r.created.size(), 4u);
    const std::string api = d.read("api/.myteam/config.yaml");
    EXPECT_NE(api.find("sandbox_hide: [\"../cli\""), std::string::npos) << api;
    EXPECT_NE(api.find("The API is built with toolkit."), std::string::npos) << api;
    EXPECT_NE(api.find("notes:"), std::string::npos) << api;
    EXPECT_NE(d.read("api/.myteam/project.md").find("the unit **api**"), std::string::npos);
    // The unit's config is a myteam project's: its rules reach its experts' briefs.
    myteam::Project unit = myteam::Project::open(d / "api");
    EXPECT_NE(unit.brief().find("The API is built with toolkit."), std::string::npos);
    EXPECT_NE(d.read(".myteam/config.yaml").find("readonly: [\"api\""), std::string::npos);
    // Nothing changes on a second sync.
    SyncResult again = sync_org(d.path(), 1);
    EXPECT_TRUE(again.created.empty());
    EXPECT_TRUE(again.updated.empty());
    // At milestone 2 api no longer sees web.
    SyncResult two = sync_org(d.path(), 2);
    EXPECT_FALSE(two.updated.empty());
    EXPECT_NE(d.read("api/.myteam/config.yaml").find("\"../web\""), std::string::npos);
}

namespace {

class Scripted : public myteam::AgentRunner {
public:
    explicit Scripted(std::vector<std::string>* log) : AgentRunner({}), log_(log) {}
    std::string id() const override { return "fake"; }
    std::string start_problem() const override { return ""; }
    std::vector<std::string> command(const myteam::RunRequest&) const override { return {"fake"}; }
    void parse_line(std::string_view, myteam::StreamState&, const myteam::EventSink&) const override {}
    myteam::RunResult run(const myteam::RunRequest& req, const myteam::EventSink&) override {
        log_->push_back(req.what);
        fs::create_directories(req.run_dir);
        myteam::RunResult r;
        r.ok = true;
        r.exit_code = 0;
        r.usage.cost_usd = 0.02;
        if (req.what == "coach") r.final_text = "Done.<milestone>The schema is drawn.</milestone>";
        else if (req.what == "gatekeeper") r.final_text = "<approve>The evidence is in.</approve>";
        else r.final_text = "FAIL";
        return r;
    }

private:
    std::vector<std::string>* log_;
};

}  // namespace

TEST(OrgEngine, AMilestoneIsCommittedAndTagged) {
    TempDir d, teams;
    init_org(d.path(), {.team = crew_team(teams)});
    identity(d.path());
    d.write("ORG.md", kOrg);
    myteam::append_file(d / ".myteam/customer/conversation.md", "# The customer and the coach\n\n### Customer · now\n\nDraw the schema.\n");
    std::vector<std::string> log;
    EngineOptions o;
    o.runners = [&](const std::string&) -> std::unique_ptr<myteam::AgentRunner> { return std::make_unique<Scripted>(&log); };
    Engine e(d.path(), o);
    Tick t = e.tick();
    EXPECT_TRUE(t.problems.empty()) << t.problems.front();
    EXPECT_EQ(log, (std::vector<std::string>{"coach", "gatekeeper"}));
    EXPECT_TRUE(t.milestone);
    EXPECT_TRUE(t.waiting);
    EXPECT_NE(t.waiting_for.find("milestone 1 reached"), std::string::npos) << t.waiting_for;
    EXPECT_EQ(load_org_state(d.path()).milestone, 2);
    std::string tags;
    git(d.path(), {"tag"}, &tags);
    EXPECT_NE(tags.find("milestone-1"), std::string::npos) << tags;
    auto conv = myteam::load_conversation(myteam::Project::open(d.path()));
    EXPECT_EQ(conv.back().text, "Milestone: The schema is drawn.");
    EXPECT_NE(d.read(".myorg/log/ticks.jsonl").find("\"reached_milestone\":true"), std::string::npos);
}

TEST(OrgWatch, WhatNeedsYouIsSaidOnceAndReleasesAreRecorded) {
    TempDir d, teams;
    init_org(d.path(), {.team = crew_team(teams)});
    identity(d.path());
    std::string org = kOrg;
    org.replace(org.find("    type: platform\n"), 18, "    type: platform\n    writes: [/tmp/toolkit-wt]\n");
    d.write("ORG.md", org);
    d.write(".myteam/team/members/tooldev.md", "---\nname: Tooldev\nrole: developer\nunit: toolkit\n---\nFix.\n");
    d.write("PLAN.md", "---\nproject: suite\n---\n\n## Work\n\n"
                       "- [ ] Lend a SSH cable @operator ^ssh\n"
                       "- [x] The queue model @tooldev ^queue\n  - Accepted by the Gatekeeper @coach 2026-11-10: checked\n"
                       "- [x] Not checked yet @tooldev ^other\n");
    d.write(".myteam/customer/conversation.md", "# c\n\n### Coach (Coach) · 2026-11-10T01:47:56Z\n\nRelease linter, please.\n");
    auto kinds = [](const std::vector<Need>& v) {
        std::vector<std::string> k;
        for (const auto& n : v) k.push_back(n.kind + " " + n.key);
        return k;
    };
    auto now = org_needs(d.path());
    EXPECT_EQ(kinds(now), (std::vector<std::string>{"operator operator:^ssh", "release release:toolkit:^queue",
                                                    "customer customer:2026-11-10T01:47:56Z"}));
    std::set<std::string> seen;
    EXPECT_EQ(new_needs(now, seen).size(), 3u);
    EXPECT_TRUE(new_needs(org_needs(d.path()), seen).empty());  // said once
    EXPECT_EQ(mark_released(d.path(), "toolkit"), (std::vector<std::string>{"queue"}));
    EXPECT_EQ(kinds(org_needs(d.path())).size(), 2u);  // the release is recorded
    // A tick that waits, and one long ago: the org waits, and nothing runs it.
    d.write(".myorg/log/ticks.jsonl", "{\"time\":\"2026-01-01T00:00:00Z\",\"waiting\":true,\"waiting_for\":\"the crew is idle\"}\n");
    auto later = kinds(org_needs(d.path(), 60));
    EXPECT_NE(std::find(later.begin(), later.end(), "waiting waiting:2026-01-01T00:00:00Z"), later.end());
    EXPECT_NE(std::find(later.begin(), later.end(), "stopped stopped:2026-01-01T00:00:00Z"), later.end());
    // The notify command gets each need's text.
    d.write(".myorg/config.yaml", "notify: \"echo \\\"$MYORG_NEED_KIND $1\\\" >> notified.txt\"\n");
    EXPECT_TRUE(notify_needs(d.path(), {{"operator", "operator:^ssh", "an operator task waits"}}).empty());
    EXPECT_EQ(d.read("notified.txt"), "operator an operator task waits\n");
}
