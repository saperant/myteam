#include <gtest/gtest.h>

#include "myteam/report.hpp"

using namespace myteam;
using taskml::TaskStatus;

TEST(Report, ParsesBlocks) {
    AgentReport r = parse_report(R"(I did the work.

<task_status>review</task_status>
<work_summary>
Built the endpoints.
Files: api.cpp
</work_summary>
<handoff to="@Verifier">
Test the endpoints.
Write the report to test-report.md.
</handoff>
<handoff to='scribe'>Document the API.</handoff>
<handoff to="">ignored</handoff>
)");
    ASSERT_TRUE(r.status);
    EXPECT_EQ(*r.status, TaskStatus::Review);
    EXPECT_EQ(r.summary, "Built the endpoints.\nFiles: api.cpp");
    EXPECT_EQ(r.reviewer, "");
    EXPECT_EQ(parse_report("<task_status reviewer=\"@Qa\">review</task_status>").reviewer, "qa");
    ASSERT_EQ(r.handoffs.size(), 2u);
    EXPECT_EQ(r.handoffs[0].to, "verifier");
    EXPECT_EQ(r.handoffs[0].instructions, "Test the endpoints.\nWrite the report to test-report.md.");
    EXPECT_EQ(r.handoffs[1].to, "scribe");
}

TEST(Report, LastStatusWinsAndAliases) {
    EXPECT_EQ(*parse_report("<task_status>blocked</task_status> <task_status>done</task_status>").status,
              TaskStatus::Completed);
    EXPECT_EQ(*report_status_from_string("In Progress"), TaskStatus::InProgress);
    EXPECT_EQ(*report_status_from_string("needs-review"), TaskStatus::Review);
    EXPECT_FALSE(report_status_from_string("maybe"));
    AgentReport unknown = parse_report("<task_status>maybe</task_status>");
    EXPECT_FALSE(unknown.status);
    EXPECT_EQ(unknown.status_text, "maybe");
    EXPECT_FALSE(parse_report("no blocks here").has_blocks());
    EXPECT_FALSE(parse_report("<task_statusx>done</task_statusx>").status);
}

TEST(Report, ASkippedCriterionStaysOpenWithItsReason) {
    AgentReport r = parse_report(
        "<criterion n=\"1\" status=\"verified\">pytest</criterion>"
        "<criterion n=\"2\" status=\"skipped\">not yet delivered: ^events-gen</criterion>"
        "<criterion n=\"3\" status=\"N/A\"></criterion>");
    ASSERT_EQ(r.criteria.size(), 3u);
    EXPECT_EQ(r.criteria[1].status, taskml::CriterionStatus::Pending);  // never a pass
    EXPECT_EQ(r.criteria[1].evidence, "skipped: not yet delivered: ^events-gen");
    EXPECT_EQ(r.criteria[2].status, taskml::CriterionStatus::Pending);
    EXPECT_EQ(r.criteria[2].evidence, "skipped");
    EXPECT_TRUE(is_skipped_status(" Skip "));
    EXPECT_FALSE(is_skipped_status("pending"));
    EXPECT_EQ(skipped_evidence("skipped: already"), "skipped: already");
}

TEST(Report, FindingsAreForTheOperatorAndDoNotBlock) {
    AgentReport r = parse_report(
        "<task_status>in_progress</task_status>"
        "<finding to=\"operator\">toolkit has no YAML interface type\nUsed JSON for now.</finding><finding></finding>");
    EXPECT_EQ(*r.status, TaskStatus::InProgress);
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_EQ(r.findings[0], "toolkit has no YAML interface type\nUsed JSON for now.");
    EXPECT_TRUE(r.blockers.empty());
    EXPECT_TRUE(parse_report("<finding>x</finding>").has_blocks());
}
