#include "myteam/report.hpp"

#include "myteam/util.hpp"

namespace myteam {

std::optional<taskml::TaskStatus> report_status_from_string(std::string_view s) {
    std::string v = lower(trim(s));
    for (char& c : v)
        if (c == '-' || c == ' ') c = '_';
    using S = taskml::TaskStatus;
    if (v == "done" || v == "complete" || v == "completed" || v == "x") return S::Completed;
    if (v == "review" || v == "needs_review" || v == "in_review") return S::Review;
    if (v == "in_progress" || v == "progress" || v == "partial" || v == "ongoing") return S::InProgress;
    if (v == "blocked" || v == "stuck") return S::Blocked;
    if (v == "cancelled" || v == "canceled" || v == "wont_do") return S::Cancelled;
    return std::nullopt;
}

using Block = ReportBlock;

std::vector<ReportBlock> report_blocks(std::string_view text, std::string_view tag) {
    std::vector<Block> out;
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t pos = 0;
    while ((pos = text.find(open, pos)) != std::string_view::npos) {
        std::size_t after = pos + open.size();
        if (after >= text.size() || (text[after] != '>' && text[after] != ' ')) {
            pos = after;
            continue;
        }
        std::size_t gt = text.find('>', after);
        if (gt == std::string_view::npos) break;
        std::size_t end = text.find(close, gt + 1);
        if (end == std::string_view::npos) break;
        out.push_back({std::string(trim(text.substr(after, gt - after))),
                       std::string(trim(text.substr(gt + 1, end - gt - 1)))});
        pos = end + close.size();
    }
    return out;
}

std::string block_attr(std::string_view attrs, std::string_view name) {
    const std::string key = std::string(name) + "=";
    std::size_t p = attrs.find(key);
    // A whole attribute name: "to=" is not the end of "into=".
    while (p != std::string_view::npos && p > 0 && attrs[p - 1] != ' ' && attrs[p - 1] != '\t')
        p = attrs.find(key, p + 1);
    if (p == std::string_view::npos) return {};
    p += name.size() + 1;
    if (p >= attrs.size()) return {};
    char q = attrs[p];
    if (q == '"' || q == '\'') {
        std::size_t e = attrs.find(q, p + 1);
        return std::string(attrs.substr(p + 1, e == std::string_view::npos ? std::string_view::npos : e - p - 1));
    }
    std::size_t e = attrs.find(' ', p);
    return std::string(attrs.substr(p, e == std::string_view::npos ? std::string_view::npos : e - p));
}

namespace {
std::vector<Block> blocks(std::string_view text, std::string_view tag) { return report_blocks(text, tag); }
std::string attr(std::string_view attrs, std::string_view name) { return block_attr(attrs, name); }
}  // namespace

BlockerReport parse_blocker(const ReportBlock& b) {
    BlockerReport x;
    x.text = b.body;
    std::string on = block_attr(b.attrs, "on");
    if (!on.empty() && on[0] == '^') on.erase(0, 1);
    std::string to = normalize_handle(block_attr(b.attrs, "to"));
    std::string kind = lower(block_attr(b.attrs, "kind"));
    if (!on.empty()) {
        x.kind = BlockerReport::Kind::Task;
        x.on = on;
    } else if (to == "customer" || kind == "customer") {
        x.kind = BlockerReport::Kind::Customer;
    } else if (to.empty() || to == "operator" || to == "human" || to == "user" || kind == "operator" ||
               kind == "access" || kind == "external" || kind == "environment") {
        x.kind = BlockerReport::Kind::Operator;
    } else {
        x.kind = BlockerReport::Kind::Member;
        x.to = to;
    }
    return x;
}

std::string web_host(std::string_view site) {
    std::string s(trim(site));
    if (auto at = s.find("://"); at != std::string::npos) s = s.substr(at + 3);
    s = s.substr(0, s.find_first_of("/?#:"));
    std::string out;
    for (char c : s) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!(std::isalnum(static_cast<unsigned char>(l)) || l == '.' || l == '-')) return "";
        out += l;
    }
    return out.find('.') == std::string::npos ? "" : out;
}

AgentReport parse_report(std::string_view text) {
    AgentReport r;
    for (const Block& b : blocks(text, "task_status")) {
        r.status_text = b.body;
        r.status = report_status_from_string(b.body);
        r.reviewer = normalize_handle(attr(b.attrs, "reviewer"));
    }
    for (const Block& b : blocks(text, "work_summary")) r.summary = b.body;
    for (const Block& b : blocks(text, "handoff")) {
        HandoffRequest h;
        h.to = normalize_handle(attr(b.attrs, "to"));
        h.instructions = b.body;
        if (!h.to.empty() && !h.instructions.empty()) r.handoffs.push_back(std::move(h));
    }
    for (const Block& b : blocks(text, "blocker")) {
        BlockerReport x = parse_blocker(b);
        if (!x.text.empty() || !x.on.empty()) r.blockers.push_back(std::move(x));
    }
    r.org_issues = parse_org_issues(text);
    for (const Block& b : blocks(text, "finding"))
        if (!b.body.empty()) r.findings.push_back(b.body);
    for (const Block& b : blocks(text, "lesson"))
        if (!b.body.empty()) r.lessons.push_back(b.body);
    for (const Block& b : blocks(text, "plan_edits"))
        if (!b.body.empty()) r.plan_edits += (r.plan_edits.empty() ? "" : "\n") + b.body;
    for (const Block& b : blocks(text, "web_access"))
        if (std::string site = web_host(attr(b.attrs, "site")); !site.empty()) r.web_access.emplace_back(site, b.body);
    for (const Block& b : blocks(text, "criterion")) {
        CriterionReport c;
        try {
            c.number = std::stoi(attr(b.attrs, "n"));
        } catch (...) {
            continue;
        }
        auto st = criterion_status_from_report(attr(b.attrs, "status"));
        if (!st || c.number < 1) continue;
        c.status = *st;
        c.evidence = is_skipped_status(attr(b.attrs, "status")) ? skipped_evidence(b.body) : b.body;
        r.criteria.push_back(std::move(c));
    }
    return r;
}

std::optional<taskml::CriterionStatus> criterion_status_from_report(std::string_view s) {
    std::string v = lower(trim(s));
    using C = taskml::CriterionStatus;
    if (v.empty() || v == "verified" || v == "pass" || v == "passed" || v == "ok" || v == "met" || v == "x" || v == "done")
        return C::Verified;
    if (v == "failed" || v == "fail" || v == "not_met" || v == "!") return C::Failed;
    if (v == "pending" || v == "open" || v == "unknown" || is_skipped_status(v)) return C::Pending;
    return std::nullopt;
}

bool is_skipped_status(std::string_view s) {
    const std::string v = lower(trim(s));
    return v == "skipped" || v == "skip" || v == "not_run" || v == "n/a";
}

std::string skipped_evidence(std::string_view why) {
    std::string w(trim(why));
    if (lower(w).rfind("skipped", 0) == 0) return w;
    return w.empty() ? "skipped" : "skipped: " + w;
}

std::string org_issue_area(std::string_view s) {
    static const char* known[] = {"context", "plan", "handoffs", "tools", "permissions", "process", "team", "other"};
    std::string a = lower(trim(s));
    if (a == "handoff") a = "handoffs";
    if (a == "tool") a = "tools";
    if (a == "permission") a = "permissions";
    if (a == "prompt" || a == "instructions") a = "context";
    for (const char* k : known)
        if (a == k) return a;
    return "other";
}

std::vector<OrgIssue> parse_org_issues(std::string_view text) {
    std::vector<OrgIssue> out;
    for (const Block& b : blocks(text, "org_issue")) {
        if (trim(b.body).empty()) continue;
        out.push_back({org_issue_area(attr(b.attrs, "area")), std::string(trim(b.body))});
    }
    return out;
}

}  // namespace myteam
