#include <gtest/gtest.h>

#include "myteam/contracts.hpp"
#include "myteam/plan.hpp"
#include "myteam/util.hpp"
#include "test_util.hpp"

using namespace myteam;
using myteam::test::TempDir;
namespace fs = std::filesystem;

namespace {
void contract(const TempDir& d, const std::string& name, const std::string& version) {
    d.write("interfaces/" + name + "/README.md", "---\ncontract: " + name + "\nversion: " + version + "\n---\n\n# C\n");
}
bool has(const std::string& s, std::string_view x) { return s.find(x) != std::string::npos; }
}  // namespace

TEST(Contracts, VersionsFromTheReadmes) {
    TempDir d;
    contract(d, "events", "3");
    contract(d, "schema", "1.1");
    fs::create_directories(d / "interfaces/schema");  // no README: not a contract
    auto v = contract_versions(d / "interfaces");
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v["events"], "3");
    EXPECT_EQ(v["schema"], "1.1");
    fs::create_directories(d / "unit");
    EXPECT_EQ(interfaces_dir(d / "unit"), d / "interfaces");  // a unit's ../interfaces
    EXPECT_EQ(interfaces_dir(d.path()), d / "interfaces");    // the org's own
}

TEST(Contracts, ABumpNotesTheOpenTasksThatNameItOnce) {
    TempDir d;
    contract(d, "events", "3");
    contract(d, "regs2", "1");
    d.write("PLAN.md",
            "# P\n\n- [ ] Client ^drv\n  - Fields from ../interfaces/events/events.json\n"
            "- [ ] UI ^ui\n  - ( ) matches interfaces/events\n- [ ] Check interfaces/events. ^chk\n"
            "- [x] Old client ^old\n  - uses interfaces/events\n- [ ] Other ^other\n  - uses interfaces/events2\n");
    const fs::path state = d / ".myteam";
    // The first look records the versions only.
    EXPECT_TRUE(note_contract_changes(d / "interfaces", state, d / "PLAN.md").empty());
    EXPECT_FALSE(has(d.read("PLAN.md"), "Contract interfaces/"));
    EXPECT_EQ(contract_changes_for_standup(state), "");
    contract(d, "events", "4");
    auto ch = note_contract_changes(d / "interfaces", state, d / "PLAN.md");
    ASSERT_EQ(ch.size(), 1u);
    EXPECT_EQ(ch[0].contract, "events");
    EXPECT_EQ(ch[0].from, "3");
    EXPECT_EQ(ch[0].to, "4");
    EXPECT_EQ(ch[0].tasks, (std::vector<std::string>{"^drv", "^ui", "^chk"}));  // not the finished one, not regs2
    Plan p = Plan::load(d / "PLAN.md");
    EXPECT_TRUE(has(p.by_id("drv")->task->notes.back(), "Contract interfaces/events is version 4 now (was 3"));
    EXPECT_TRUE(p.by_id("other")->task->notes.size() == 1);
    EXPECT_TRUE(has(contract_changes_for_standup(state), "interfaces/events: version 3 -> 4; tasks that read it: ^drv ^ui ^chk"));
    // Nothing changed since: no second note.
    EXPECT_TRUE(note_contract_changes(d / "interfaces", state, d / "PLAN.md").empty());
    EXPECT_EQ(Plan::load(d / "PLAN.md").by_id("drv")->task->notes.size(), 2u);
    // A new contract after the first look is a change too; the list grows until the stand-up.
    contract(d, "audio-noc", "1");
    ch = note_contract_changes(d / "interfaces", state, d / "PLAN.md");
    ASSERT_EQ(ch.size(), 1u);
    EXPECT_EQ(ch[0].from, "new");
    EXPECT_TRUE(has(contract_changes_for_standup(state), "interfaces/audio-noc: version new -> 1; no open task names it"));
    EXPECT_TRUE(has(contract_changes_for_standup(state), "interfaces/events: version 3 -> 4"));
    clear_contract_changes(state);
    EXPECT_EQ(contract_changes_for_standup(state), "");
}
