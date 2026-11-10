// Organisational feedback: what in the way the team is organised got in an
// agent's way. It is about myteam (the context it was given, the plan format,
// handoffs, tools and permissions, the process, the team's make-up), not about
// the product and not about the agent's own prompt (retrospectives change
// those). It is collected here and exported to drive myteam's development.
//
// .myteam/feedback.md is a plan file:
//
//   - [ ] The context does not say how to run the tests #context ^context-does-not-say
//     - Reported 2026-11-06 by @developer on ^count
//     - I had to read the CMake files to find the test target.
//     - Also reported 2026-11-08 by @developer on ^usage-docs (retrospective)
//
//   [ ] new · [~] filed for myteam · [x] fixed in myteam · [-] will not be done
//
// Agents report it with <org_issue area="..."> in any report; the
// retrospective asks for it explicitly. A report that resembles an open one
// adds an "Also reported" note instead of a new item, so the count shows
// what hurts most.
#pragma once

#include <string>
#include <vector>

#include "myteam/project.hpp"
#include "myteam/report.hpp"

namespace myteam {

struct FeedbackSource {
    std::string member;    ///< handle
    std::string task;      ///< task id the agent worked on, may be empty
    std::string occasion;  ///< "", "retrospective", "stand-up", ...
};

struct FeedbackResult {
    std::vector<std::string> added;   ///< ids of new items
    std::vector<std::string> merged;  ///< ids of items reported again
    std::string error;                ///< the file could not be written
};

/// Create .myteam/feedback.md when missing.
void ensure_feedback_file(const Project& project);

/// Record the issues: a new item, or a note on a similar open one.
FeedbackResult record_feedback(const Project& project, const std::vector<OrgIssue>& issues,
                               const FeedbackSource& source);

/// Similar enough to be the same issue (the words of the first lines).
bool similar_issue(std::string_view a, std::string_view b);

struct FeedbackItem {
    std::string id;
    std::string area;
    std::string title;
    std::string status;  ///< new, filed, fixed, declined
    int reports = 1;
    int since_fix = 0;                   ///< reports after it was marked fixed
    std::vector<std::string> reporters;  ///< handles
    std::vector<std::string> notes;      ///< the detail, without the "Reported" lines
};

std::vector<FeedbackItem> load_feedback(const Project& project);

/// A Markdown digest for myteam's developers, grouped by area, most reported
/// first. `all` includes fixed and declined items.
std::string export_feedback(const Project& project, bool all);

}  // namespace myteam
