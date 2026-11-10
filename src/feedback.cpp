#include "myteam/feedback.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

#include "myteam/plan.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Task;
using taskml::TaskStatus;

namespace {

const char* kTemplate =
    "# Feedback for myteam\n"
    "\n"
    "What in the way the team is organised got in the agents' way: the context\n"
    "they were given, the plan, handoffs, tools and permissions, the process, the\n"
    "team. Collected from reports and retrospectives, for myteam's developers\n"
    "(`myteam feedback --export`).\n"
    "\n"
    "`[ ]` new · `[~]` filed for myteam · `[x]` fixed in myteam · `[-]` will not be done\n"
    "\n"
    "## Issues\n";

std::string tag_area(const Task& t) {
    for (const auto& tag : t.tags)
        if (org_issue_area(tag) == tag) return tag;
    return "other";
}

std::string status_word(TaskStatus s) {
    switch (s) {
        case TaskStatus::InProgress: return "filed";
        case TaskStatus::Completed: return "fixed";
        case TaskStatus::Cancelled: return "declined";
        default: return "new";
    }
}

std::string reporter(std::string_view note) {
    auto p = note.find("by @");
    if (p == std::string_view::npos) return {};
    p += 4;
    auto e = p;
    while (e < note.size() && (std::isalnum(static_cast<unsigned char>(note[e])) || note[e] == '-' || note[e] == '_')) ++e;
    return std::string(note.substr(p, e - p));
}

std::string source_line(const char* what, const FeedbackSource& s) {
    std::string l = std::string(what) + " " + iso_timestamp().substr(0, 10);
    if (!s.member.empty()) l += " by @" + s.member;
    if (!s.task.empty()) l += " on ^" + s.task;
    if (!s.occasion.empty()) l += " (" + s.occasion + ")";
    return l;
}

}  // namespace

bool similar_issue(std::string_view a, std::string_view b) { return same_subject(a, b); }

void ensure_feedback_file(const Project& project) {
    if (!fs::exists(project.feedback_path())) {
        fs::create_directories(project.feedback_path().parent_path());
        write_file_atomic(project.feedback_path(), kTemplate);
    }
}

FeedbackResult record_feedback(const Project& project, const std::vector<OrgIssue>& issues,
                               const FeedbackSource& source) {
    FeedbackResult out;
    if (issues.empty()) return out;
    ensure_feedback_file(project);
    Plan fb = Plan::load(project.feedback_path());
    if (!fb.ok()) {
        out.error = project.feedback_path().string() + " does not validate";
        return out;
    }
    PlanEditor ed(fb);
    std::vector<std::pair<std::string, std::string>> added;  // area, text of items added now
    for (const OrgIssue& is : issues) {
        auto [title, more] = title_and_rest(first_line(is.text, 1000));
        // An item about the same thing: count it again. An open one first; a
        // fixed one gets the report as a note (is the fix holding?), not reopened.
        const TaskRef* same = nullptr;
        for (const TaskRef& r : fb.tasks())
            if (!is_finished(r.task->status) && similar_issue(r.task->description, title)) {
                same = &r;
                break;
            }
        bool after_fix = false;
        if (!same)
            for (const TaskRef& r : fb.tasks())
                if (r.task->status == taskml::TaskStatus::Completed && similar_issue(r.task->description, title)) {
                    same = &r;
                    after_fix = true;
                    break;
                }
        if (same) {
            ed.add_note(*same, source_line(after_fix ? "Reported again after the fix" : "Also reported", source) +
                                   (after_fix && !more.empty() ? ": " + first_line(more, 200) : ""));
            out.merged.push_back(same->task->id.value_or(same->key()));
            continue;
        }
        if (std::any_of(added.begin(), added.end(), [&](const auto& a) { return similar_issue(a.second, title); }))
            continue;  // the same report twice
        NewTask t;
        t.description = title;
        t.tags.push_back(is.area);
        t.notes.push_back(source_line("Reported", source));
        if (!more.empty()) t.notes.push_back(more);
        auto lines = split_lines(is.text);
        for (std::size_t i = 1; i < lines.size() && t.notes.size() < 12; ++i)
            if (!trim(lines[i]).empty()) t.notes.emplace_back(trim(lines[i]));
        out.added.push_back(ed.add_to_section("Issues", t));
        added.emplace_back(is.area, title);
    }
    if (!ed.changed()) return out;
    CommitResult c = ed.commit();
    if (!c.ok) {
        out.error = "feedback not recorded: " + c.error;
        out.added.clear();
        out.merged.clear();
    }
    return out;
}

std::vector<FeedbackItem> load_feedback(const Project& project) {
    std::vector<FeedbackItem> out;
    if (!fs::exists(project.feedback_path())) return out;
    Plan fb = Plan::load(project.feedback_path());
    for (const TaskRef& r : fb.tasks()) {
        const Task& t = *r.task;
        FeedbackItem it;
        it.id = t.id.value_or(r.key());
        it.area = tag_area(t);
        it.title = t.description;
        it.status = status_word(t.status);
        it.reports = 0;
        for (const auto& n : t.notes) {
            if (starts_with(n, "Reported") || starts_with(n, "Also reported")) {
                ++it.reports;
                if (starts_with(n, "Reported again after the fix")) ++it.since_fix;
                std::string who = reporter(n);
                if (!who.empty() && std::find(it.reporters.begin(), it.reporters.end(), who) == it.reporters.end())
                    it.reporters.push_back(who);
            } else {
                it.notes.push_back(n);
            }
        }
        it.reports = std::max(it.reports, 1);
        out.push_back(std::move(it));
    }
    return out;
}

std::string export_feedback(const Project& project, bool all) {
    std::vector<FeedbackItem> items = load_feedback(project);
    std::map<std::string, std::vector<const FeedbackItem*>> by_area;
    for (const auto& it : items)
        if (all || it.status == "new" || it.status == "filed") by_area[it.area].push_back(&it);

    std::string out = "# Feedback for myteam from " + project.dir().filename().string() + "\n\n";
    out += "Exported " + iso_timestamp().substr(0, 10) + ".\n";
    if (by_area.empty()) return out + "\nNo open issues.\n";
    // Areas with the most reports first; items likewise.
    std::vector<std::pair<std::string, int>> areas;
    for (auto& [area, v] : by_area) {
        std::stable_sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->reports > b->reports; });
        int n = 0;
        for (auto* i : v) n += i->reports;
        areas.emplace_back(area, n);
    }
    std::stable_sort(areas.begin(), areas.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (const auto& [area, n] : areas) {
        out += "\n## " + area + "\n";
        for (const FeedbackItem* i : by_area[area]) {
            out += "\n- **" + i->title + "**";
            out += " (reported " + std::to_string(i->reports) + (i->reports == 1 ? " time" : " times");
            if (!i->reporters.empty()) {
                out += " by";
                for (std::size_t k = 0; k < i->reporters.size(); ++k) out += (k ? ", @" : " @") + i->reporters[k];
            }
            if (i->status != "new") out += "; " + i->status;
            out += ")\n";
            for (const auto& n : i->notes) out += "  " + n + "\n";
        }
    }
    return out;
}

}  // namespace myteam
