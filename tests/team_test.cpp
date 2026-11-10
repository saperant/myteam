#include <gtest/gtest.h>

#include "myteam/team.hpp"
#include "test_util.hpp"

using namespace myteam;
using myteam::test::TempDir;

namespace {

void write_small_team(const TempDir& d, const std::string& dir = "team") {
    d.write(dir + "/team.md",
            "---\nid: small\nname: Small\nproduct_owner: po\nmodels:\n  default: claude:sonnet\n  economy: vibe\n"
            "allocation:\n  roles: {qa: tess}\n  keywords: {test: tess, api: dev}\n---\n\nA small team.\n");
    d.write(dir + "/members/po.md", "---\nname: Owner\nrole: product-owner\n---\nPrioritize.\n");
    d.write(dir + "/members/dev.md",
            "---\nname: Dev\nrole: developer\nsummary: writes code\nskills: [cpp]\n"
            "models: {default: codex:gpt-5, premium: claude:opus}\n"
            "handoffs:\n  - when: code is done\n    to: tess\n---\nWrite code.\n");
    d.write(dir + "/members/tess.md", "---\nname: Tess\nrole: tester\n---\nTest things.\n");
}

}  // namespace

TEST(Team, Loads) {
    TempDir d;
    write_small_team(d);
    Team t = load_team(d / "team");
    EXPECT_EQ(t.id, "small");
    EXPECT_EQ(t.overview, "A small team.");
    ASSERT_EQ(t.members.size(), 3u);
    const Member* dev = t.find("@Dev");
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->prompt, "Write code.");
    EXPECT_EQ(dev->models.standard, (ModelRef{"codex", "gpt-5"}));
    EXPECT_EQ(dev->models.for_priority(0), (ModelRef{"claude", "opus"}));
    EXPECT_EQ(dev->models.for_priority(2), (ModelRef{"codex", "gpt-5"}));  // no economy of its own
    const Member* tess = t.find("tess");
    EXPECT_EQ(tess->models.for_priority(3), (ModelRef{"vibe", ""}));        // the team's
    EXPECT_EQ(t.product_owner_member().name, "Owner");
    ASSERT_EQ(dev->handoffs.size(), 1u);
    EXPECT_EQ(dev->handoffs[0].to, "tess");
}

TEST(Team, OverlayReplacesAndAdds) {
    TempDir d;
    write_small_team(d);
    d.write("overlay/members/dev.md", "---\nname: Dev\nrole: developer\n---\nWrite careful code.\n");
    d.write("overlay/members/sam.md", "---\nname: Sam\nrole: security\n---\nThreat models.\n");
    Team t = load_team(d / "team", d / "overlay");
    EXPECT_EQ(t.find("dev")->prompt, "Write careful code.");
    EXPECT_EQ(t.find("dev")->models.standard, (ModelRef{"claude", "sonnet"}));  // overlay file has none
    ASSERT_NE(t.find("sam"), nullptr);
    EXPECT_EQ(t.members.size(), 4u);
}

TEST(Team, Errors) {
    TempDir d;
    write_small_team(d);
    d.write("team/members/dev.md", "---\nname: Dev\nrole: developer\nhandoffs:\n  - when: x\n    to: nobody\n---\n");
    EXPECT_THROW(load_team(d / "team"), std::runtime_error);
    write_small_team(d);
    d.write("team/members/tess.md", "---\nname: Tess\n---\n");  // no role
    EXPECT_THROW(load_team(d / "team"), std::runtime_error);
    EXPECT_THROW(load_team(d / "missing"), std::runtime_error);
}

TEST(Team, BundledTeamsLoad) {
    const std::filesystem::path bundled = std::filesystem::path(MYTEAM_SOURCE_DIR) / "plugins" / "teams";
    for (const char* name : {"software", "research", "crew"}) {
        Team t = load_team(bundled / name);
        EXPECT_FALSE(t.members.empty()) << name;
        EXPECT_NE(t.find(t.product_owner), nullptr) << name;
    }
    Team software = load_team(bundled / "software");
    EXPECT_EQ(software.members.size(), 4u);
    EXPECT_EQ(software.repairer, "developer");
    Team crew = load_team(bundled / "crew");
    EXPECT_EQ(crew.coach, "coach");
    EXPECT_EQ(crew.gatekeeper, "gatekeeper");
}
