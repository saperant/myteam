#include <gtest/gtest.h>

#include "myteam/feedback.hpp"
#include "myteam/plan.hpp"
#include "myteam/util.hpp"
#include "test_util.hpp"

using namespace myteam;
using myteam::test::TempDir;

TEST(OrgIssue, Parse) {
    AgentReport r = parse_report(R"(Done.
<task_status>done</task_status>
<org_issue area="Context">The context does not say how to run the tests
I had to read CMakeLists.txt to find the target.</org_issue>
<org_issue area="handoff">Handoffs cannot say which file to deliver</org_issue>
<org_issue area="weather">It rained</org_issue>
<org_issue> </org_issue>)");
    ASSERT_EQ(r.org_issues.size(), 3u);
    EXPECT_EQ(r.org_issues[0].area, "context");
    EXPECT_EQ(first_line(r.org_issues[0].text), "The context does not say how to run the tests");
    EXPECT_EQ(r.org_issues[1].area, "handoffs");
    EXPECT_EQ(r.org_issues[2].area, "other");
    EXPECT_TRUE(r.status.has_value());
}

TEST(OrgIssue, Similar) {
    EXPECT_TRUE(similar_issue("The context does not say how to run the tests",
                              "context doesn't say how to run tests"));
    EXPECT_TRUE(similar_issue("Handoffs cannot name the file to deliver", "Handoff cannot name the file to deliver"));
    EXPECT_FALSE(similar_issue("The context does not say how to run the tests",
                               "The plan format has no place for estimates"));
    EXPECT_FALSE(similar_issue("", "anything"));
}

TEST(Feedback, RecordMergeExport) {
    TempDir dir;
    dir.write("p/PLAN.md", "# P\n\n- [ ] Work ^w\n");
    Project p = Project::open(dir / "p");

    FeedbackResult a = record_feedback(
        p, {{"context", "The context does not say how to run the tests\nHad to read CMakeLists.txt."},
            {"tools", "No permission to run the linter"}},
        {"builder", "w", ""});
    ASSERT_TRUE(a.error.empty()) << a.error;
    ASSERT_EQ(a.added.size(), 2u);
    EXPECT_TRUE(a.merged.empty());

    FeedbackResult b = record_feedback(p, {{"context", "context doesn't say how to run tests"}},
                                       {"fixer", "", "retrospective"});
    ASSERT_EQ(b.merged.size(), 1u);
    EXPECT_EQ(b.merged[0], a.added[0]);
    EXPECT_TRUE(b.added.empty());

    // The file is a valid plan with tags and notes.
    Plan fb = Plan::load(p.feedback_path());
    ASSERT_TRUE(fb.ok());
    const TaskRef* t = fb.by_id(a.added[0]);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->task->tags, std::vector<std::string>{"context"});
    std::string text = read_file(p.feedback_path());
    EXPECT_NE(text.find("Reported " + iso_timestamp().substr(0, 10) + " by @builder on ^w"), std::string::npos);
    EXPECT_NE(text.find("by @fixer (retrospective)"), std::string::npos);

    auto items = load_feedback(p);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].reports, 2);
    EXPECT_EQ(items[0].reporters, (std::vector<std::string>{"builder", "fixer"}));
    EXPECT_EQ(items[0].notes, std::vector<std::string>{"Had to read CMakeLists.txt."});
    EXPECT_EQ(items[1].area, "tools");

    std::string md = export_feedback(p, false);
    EXPECT_NE(md.find("## context"), std::string::npos);
    EXPECT_LT(md.find("## context"), md.find("## tools"));  // most reported first
    EXPECT_NE(md.find("(reported 2 times by @builder, @fixer)"), std::string::npos);

    // Fixed items leave the export; a new report goes to the fixed item (is the
    // fix holding?), which stays fixed.
    std::string fixed = text;
    fixed.replace(fixed.find("- [ ] The context"), 5, "- [x]");
    write_file_atomic(p.feedback_path(), fixed);
    EXPECT_EQ(export_feedback(p, false).find("## context"), std::string::npos);
    EXPECT_NE(export_feedback(p, true).find("fixed"), std::string::npos);
    FeedbackResult c = record_feedback(p, {{"context", "The context does not say how to run the tests"}}, {"builder", "", ""});
    EXPECT_TRUE(c.added.empty());
    ASSERT_EQ(c.merged.size(), 1u);
    auto after = load_feedback(p);
    EXPECT_EQ(after[0].status, "fixed");
    EXPECT_EQ(after[0].since_fix, 1);
    // Something else is a new item.
    EXPECT_EQ(record_feedback(p, {{"plan", "Handoffs arrive without their list"}}, {"builder", "", ""}).added.size(), 1u);
}

TEST(Feedback, TheSameSubjectInOtherWordsIsOneItem) {
    // From a real retrospective: the same issues, reported shorter.
    EXPECT_TRUE(similar_issue("The guard protects `.github/workflows/`, so a CI-owning role can't deliver CI",
                              "The guard protects `.github/workflows/`"));
    EXPECT_TRUE(similar_issue("The sandbox blocks podman (read-only `/run/user/1000/libpod`), so PostgreSQL verification",
                              "The sandbox blocks podman, so nobody can run PostgreSQL"));
    EXPECT_TRUE(similar_issue("Four of this round's tasks had no acceptance criteria, which produced the problems",
                              "Several tasks had no acceptance criteria"));
    EXPECT_FALSE(similar_issue("The guard protects `.github/workflows/`", "The sandbox blocks podman"));
    EXPECT_FALSE(similar_issue("No linter", "No tests"));
}
