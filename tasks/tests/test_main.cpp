// SPDX-License-Identifier: MIT
// C++ test suite for taskml. Self-contained (no external test framework).

#include <taskml/taskml.hpp>

#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------

namespace {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};
int g_failures = 0;
bool g_current_failed = false;

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream os;
        os << file << ":" << line << ": CHECK_EQ(" << ea << ", " << eb << ") failed";
        if constexpr (std::is_convertible_v<A, std::string> || std::is_arithmetic_v<A>) os << "\n    left:  " << a;
        if constexpr (std::is_convertible_v<B, std::string> || std::is_arithmetic_v<B>) os << "\n    right: " << b;
        std::cerr << os.str() << "\n";
        g_current_failed = true;
    }
}

}  // namespace

#define TEST(name)                                         \
    static void test_##name();                             \
    static Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n"; \
            g_current_failed = true;                                                  \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b) check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define REQUIRE(cond)                                                                   \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": REQUIRE(" #cond ") failed\n"; \
            g_current_failed = true;                                                    \
            return;                                                                     \
        }                                                                               \
    } while (0)

using namespace taskml;

static ParseResult ok_parse(const std::string& src, ParseOptions opts = {}) {
    ParseResult r = parse(src, opts);
    for (const auto& e : r.errors) std::cerr << "    unexpected: " << e.format(src) << "\n";
    CHECK(r.errors.empty());
    return r;
}

static bool has_code(const std::vector<Diagnostic>& ds, ErrorCode code) {
    for (const auto& d : ds)
        if (d.code == code) return true;
    return false;
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Directives
// ---------------------------------------------------------------------------

TEST(directives_basic) {
    auto r = ok_parse("@project My Project\n@sprint Sprint 1\n@version 1.1\n@author Alice");
    const auto& d = r.document.directives;
    CHECK_EQ(*d.get("project"), "My Project");
    CHECK_EQ(*d.get("sprint"), "Sprint 1");
    CHECK_EQ(*d.get("version"), "1.1");
    CHECK_EQ(*d.get("author"), "Alice");
    CHECK_EQ(r.document.version, "1.1");
    CHECK_EQ(d.size(), 4u);
    CHECK(r.warnings.empty());
}

TEST(directives_custom_and_order) {
    auto r = ok_parse("@x-team backend\n@x-milestone Q1-2026\n");
    CHECK_EQ(*r.document.directives.get("x-team"), "backend");
    CHECK_EQ(*r.document.directives.get("x-milestone"), "Q1-2026");
    CHECK_EQ(r.document.directives.items()[0].first, "x-team");
}

TEST(directives_after_comment) {
    auto r = ok_parse("// header\n@project Test\n[ ] Task");
    CHECK_EQ(r.document.directives.value_or("project", ""), "Test");
    CHECK_EQ(r.document.comments.size(), 1u);
}

TEST(directives_unknown_warns_when_disallowed) {
    ParseOptions o;
    o.allow_unknown_directives = false;
    auto r = ok_parse("@team core\n@x-ok fine\n@project P", o);
    CHECK_EQ(r.warnings.size(), 1u);
    CHECK(has_code(r.warnings, ErrorCode::UnknownDirective));
}

TEST(directive_without_value_warns) {
    auto r = ok_parse("@project\n");
    CHECK(has_code(r.warnings, ErrorCode::ExpectedDirectiveValue));
    CHECK_EQ(*r.document.directives.get("project"), "");
}

TEST(agent_directives) {
    auto r = ok_parse("@agent claude-3\n@agent-id session-abc123\n");
    REQUIRE(r.document.agent_context.has_value());
    CHECK_EQ(*r.document.agent_context->model, "claude-3");
    CHECK_EQ(*r.document.agent_context->agent_id, "session-abc123");
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

TEST(all_statuses) {
    auto r = ok_parse("[ ] a\n[~] b\n[x] c\n[!] d\n[-] e\n[?] f\n[X] g");
    const auto& t = r.document.tasks;
    REQUIRE(t.size() == 7);
    CHECK(t[0].status == TaskStatus::Pending);
    CHECK(t[1].status == TaskStatus::InProgress);
    CHECK(t[2].status == TaskStatus::Completed);
    CHECK(t[3].status == TaskStatus::Blocked);
    CHECK(t[4].status == TaskStatus::Cancelled);
    CHECK(t[5].status == TaskStatus::Review);
    CHECK(t[6].status == TaskStatus::Completed);
    CHECK_EQ(t[0].description, "a");
    CHECK_EQ(t[0].line, 1);
}

TEST(unknown_status_warns) {
    auto r = ok_parse("[y] nope\n[ ] yes");
    CHECK_EQ(r.document.tasks.size(), 1u);
    CHECK(has_code(r.warnings, ErrorCode::UnknownStatus));
}

TEST(all_tokens) {
    auto r = ok_parse("[ ] Implement auth #p0 ~8h @bob #backend !2026-01-20 ^auth-1");
    const Task& t = r.document.tasks.at(0);
    CHECK_EQ(t.description, "Implement auth");
    CHECK_EQ(*t.priority, 0);
    CHECK_EQ(*t.estimate, "8h");
    CHECK_EQ(*t.assignee, "bob");
    REQUIRE(t.tags.size() == 1);
    CHECK_EQ(t.tags[0], "backend");
    CHECK_EQ(*t.due, "2026-01-20");
    CHECK_EQ(*t.id, "auth-1");
}

TEST(priorities_and_estimates) {
    auto r = ok_parse("[ ] a #p0\n[ ] b #p1\n[ ] c #p2\n[ ] d #p3 ~1d\n[ ] e ~5d");
    for (int i = 0; i < 4; ++i) CHECK_EQ(*r.document.tasks[i].priority, i);
    CHECK_EQ(*r.document.tasks[3].estimate, "1d");
    CHECK_EQ(*r.document.tasks[4].estimate, "5d");
}

TEST(tokens_in_middle_of_description) {
    auto r = ok_parse("[!] Deploy to production #p0 - waiting for security review");
    const Task& t = r.document.tasks[0];
    CHECK_EQ(t.description, "Deploy to production - waiting for security review");
    CHECK_EQ(*t.priority, 0);
}

TEST(tokens_require_word_boundary) {
    auto r = ok_parse("[ ] Email bob@example.com about issue#42 and C# ~ maybe x^2");
    const Task& t = r.document.tasks[0];
    CHECK_EQ(t.description, "Email bob@example.com about issue#42 and C# ~ maybe x^2");
    CHECK(!t.assignee);
    CHECK(t.tags.empty());
    CHECK(!t.id);
}

TEST(token_lookalikes_are_text) {
    auto r = ok_parse("[ ] Ship ~1y and #project and !important ~4hours");
    const Task& t = r.document.tasks[0];
    CHECK(!t.estimate);
    CHECK(!t.priority);
    REQUIRE(t.tags.size() == 1);
    CHECK_EQ(t.tags[0], "project");
    CHECK_EQ(t.description, "Ship ~1y and and !important ~4hours");
}

TEST(week_estimates_accepted) {
    auto r = ok_parse("[ ] Big job ~2w");
    CHECK_EQ(*r.document.tasks[0].estimate, "2w");
    CHECK(r.warnings.empty());
    ParseOptions strict;
    strict.strict = true;
    CHECK(has_code(parse("[ ] Big job ~2w", strict).warnings, ErrorCode::InvalidEstimateFormat));
}

TEST(nested_notes_and_continuations) {
    auto r = ok_parse(
        "[ ] Task\n"
        "  - Contract map:\n"
        "    - Buffer API -> ^core (@auditor)\n"
        "    - File I/O\n"
        "      wrapped continuation\n"
        "  - Back at top\n"
        "  [ ] Sub\n"
        "    - sub note\n"
        "      continues here\n"
        "  - parent note again\n");
    CHECK(r.warnings.empty());
    const Task& t = r.document.tasks.at(0);
    REQUIRE(t.notes.size() == 5);
    CHECK_EQ(t.notes[1], "Buffer API -> ^core (@auditor)");
    CHECK_EQ(t.notes[2], "File I/O wrapped continuation");
    CHECK_EQ(t.notes[3], "Back at top");
    CHECK_EQ(t.notes[4], "parent note again");
    CHECK_EQ(t.subtasks.at(0).notes.at(0), "sub note continues here");
    // Without a preceding note, deeper text is still unrecognized.
    auto r2 = ok_parse("[ ] a\n    stray text");
    CHECK(has_code(r2.warnings, ErrorCode::UnexpectedToken));
}

TEST(invalid_priority_is_tag_with_warning) {
    auto r = ok_parse("[ ] a #p7");
    CHECK(!r.document.tasks[0].priority);
    CHECK_EQ(r.document.tasks[0].tags.at(0), "p7");
    CHECK(has_code(r.warnings, ErrorCode::InvalidPriority));
}

TEST(invalid_date_warns) {
    auto r = ok_parse("[ ] a !2026-02-30");
    CHECK_EQ(*r.document.tasks[0].due, "2026-02-30");
    CHECK(has_code(r.warnings, ErrorCode::InvalidDateFormat));
    auto r2 = ok_parse("[ ] a !2024-02-29");
    CHECK(r2.warnings.empty());
}

TEST(multiple_assignees_warns) {
    auto r = ok_parse("[ ] a @alice @bob");
    CHECK_EQ(*r.document.tasks[0].assignee, "bob");
    CHECK(has_code(r.warnings, ErrorCode::MultipleAssignees));
}

TEST(escapes) {
    auto r = ok_parse("[ ] Use \\#hashtag and \\@mention, path C:\\temp \\\\#x");
    const Task& t = r.document.tasks[0];
    CHECK_EQ(t.description, "Use #hashtag and @mention, path C:\\temp \\#x");
    CHECK(t.tags.empty());
    CHECK(!t.assignee);
}

TEST(empty_description_warns) {
    auto r = ok_parse("[ ] #p1");
    CHECK_EQ(r.document.tasks[0].description, "");
    CHECK(has_code(r.warnings, ErrorCode::ExpectedDescription));
}

TEST(strict_token_order) {
    ParseOptions strict;
    strict.strict = true;
    auto good = ok_parse("[ ] Task description #p0 ~4h @alice #backend !2026-01-15 ^task-1", strict);
    CHECK(good.warnings.empty());

    auto bad = ok_parse("[ ] Task ~4h #p0", strict);
    REQUIRE(bad.warnings.size() == 1);
    CHECK_EQ(bad.warnings[0].message, "Token order: priority should appear before estimate");
    CHECK(bad.warnings[0].code == ErrorCode::TokenOrderViolation);

    auto bad2 = ok_parse("[ ] Task ^task-1 !2026-01-15", strict);
    REQUIRE(bad2.warnings.size() == 1);
    CHECK(bad2.warnings[0].message.find("due date") != std::string::npos);

    auto lax = ok_parse("[ ] Task ~4h #p0");
    CHECK(lax.warnings.empty());
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

TEST(subtasks_and_deep_nesting) {
    auto r = ok_parse("[ ] Level 0\n    [ ] Level 1a\n        [ ] Level 2\n    [ ] Level 1b\n[ ] Root 2");
    REQUIRE(r.document.tasks.size() == 2);
    const Task& t = r.document.tasks[0];
    REQUIRE(t.subtasks.size() == 2);
    CHECK_EQ(t.subtasks[0].subtasks.size(), 1u);
    CHECK_EQ(t.subtasks[0].subtasks[0].description, "Level 2");
    CHECK_EQ(t.subtasks[1].description, "Level 1b");
}

TEST(two_space_and_tab_indentation) {
    auto r = ok_parse("[ ] a\n  [ ] b\n    [ ] c\n[ ] d\n\t[ ] e");
    REQUIRE(r.document.tasks.size() == 2);
    CHECK_EQ(r.document.tasks[0].subtasks[0].subtasks[0].description, "c");
    CHECK_EQ(r.document.tasks[1].subtasks[0].description, "e");
}

TEST(inconsistent_indentation_is_error) {
    auto r = parse("[ ] a\n    [ ] b\n  [ ] c");
    CHECK(has_code(r.errors, ErrorCode::InconsistentIndentation));
    // Recovery: c is still attached to a.
    CHECK_EQ(r.document.tasks[0].subtasks.size(), 2u);
    try {
        parse_or_throw("[ ] a\n    [ ] b\n  [ ] c");
        CHECK(false);
    } catch (const Error& e) {
        CHECK(std::string(e.what()).find("Line 3") != std::string::npos);
        CHECK_EQ(e.diagnostics().size(), 1u);
    }
}

TEST(notes_dependencies_criteria) {
    auto r = ok_parse(
        "[ ] Deploy ^deploy-1\n"
        "    depends: ^setup-ci, ^security-review\n"
        "    blocked-by: ^ops\n"
        "    - Must support full-text search\n"
        "    - Consider \\#hashtags\n"
        "    ✓ Status 200 for valid request\n"
        "      evidence: \"curl output shows 200 OK\"\n"
        "    [✗] Session persists\n"
        "    ○ Pending one\n"
        "    ✔ heavy check\n");
    const Task& t = r.document.tasks.at(0);
    REQUIRE(t.depends_on.size() == 2);
    CHECK_EQ(t.depends_on[0], "setup-ci");
    CHECK_EQ(t.depends_on[1], "security-review");
    CHECK_EQ(t.blocked_by.at(0), "ops");
    REQUIRE(t.notes.size() == 2);
    CHECK_EQ(t.notes[0], "Must support full-text search");
    CHECK_EQ(t.notes[1], "Consider #hashtags");
    REQUIRE(t.criteria.size() == 4);
    CHECK(t.criteria[0].status == CriterionStatus::Verified);
    CHECK_EQ(t.criteria[0].description, "Status 200 for valid request");
    CHECK_EQ(*t.criteria[0].evidence, "curl output shows 200 OK");
    CHECK(t.criteria[1].status == CriterionStatus::Failed);
    CHECK_EQ(t.criteria[1].description, "Session persists");
    CHECK(t.criteria[2].status == CriterionStatus::Pending);
    CHECK(t.criteria[3].status == CriterionStatus::Verified);
    CHECK(r.warnings.empty());
}

TEST(depends_on_alias) {
    auto r = ok_parse("[ ] a\n    depends-on: ^x ^y\n    depends: ^z");
    CHECK_EQ(r.document.tasks[0].depends_on.size(), 3u);
}

TEST(criteria_after_subtasks_belong_to_parent) {
    auto r = ok_parse("[ ] Parent\n    [ ] Child\n    ○ Parent criterion");
    CHECK_EQ(r.document.tasks[0].criteria.size(), 1u);
    CHECK(r.document.tasks[0].subtasks[0].criteria.empty());
}

TEST(root_level_note_warns) {
    auto r = ok_parse("- Note: Using Tailwind for styling\n[ ] a");
    CHECK(has_code(r.warnings, ErrorCode::UnexpectedToken));
    CHECK_EQ(r.document.tasks.size(), 1u);
}

TEST(max_depth) {
    std::string src;
    for (int i = 0; i < 5; ++i) src += std::string(static_cast<std::size_t>(i * 2), ' ') + "[ ] t\n";
    ParseOptions o;
    o.max_depth = 3;
    auto r = parse(src, o);
    CHECK(has_code(r.errors, ErrorCode::InvalidNesting));
}

// ---------------------------------------------------------------------------
// Sections, includes, comments
// ---------------------------------------------------------------------------

TEST(sections) {
    auto r = ok_parse("[ ] Root task\n\n== Sprint 1 ==\n[ ] In 1\n\n=== Sub\n[ ] In 2\n== Empty ==\n");
    CHECK_EQ(r.document.tasks.size(), 1u);
    REQUIRE(r.document.sections.size() == 3);
    CHECK_EQ(r.document.sections[0].title, "Sprint 1");
    CHECK_EQ(r.document.sections[0].level, 2);
    CHECK_EQ(r.document.sections[0].tasks.size(), 1u);
    CHECK_EQ(r.document.sections[1].title, "Sub");
    CHECK_EQ(r.document.sections[1].level, 3);
    CHECK(r.document.sections[2].tasks.empty());
}

TEST(includes) {
    auto r = ok_parse("< ./tasks/sprint1.taskml\n[ ] Task\n< ./path/with spaces/file.taskml");
    REQUIRE(r.document.includes.size() == 2);
    CHECK_EQ(r.document.includes[0].path, "./tasks/sprint1.taskml");
    CHECK_EQ(r.document.includes[0].line, 1);
    CHECK_EQ(r.document.includes[1].path, "./path/with spaces/file.taskml");
    CHECK_EQ(r.document.includes[1].line, 3);
}

TEST(comments) {
    auto r = ok_parse("// Header comment\n@project Test\n\n// Before task\n[ ] Do http://x.y // not a comment\n// After task\n");
    REQUIRE(r.document.comments.size() == 3);
    CHECK_EQ(r.document.comments[0].text, "Header comment");
    CHECK_EQ(r.document.comments[0].line, 1);
    CHECK_EQ(r.document.comments[2].line, 6);
    CHECK_EQ(r.document.tasks[0].description, "Do http://x.y // not a comment");
}

TEST(crlf_and_bom) {
    auto r = ok_parse("\xEF\xBB\xBF@project P\r\n[ ] a\r\n    [ ] b\r[ ] c");
    CHECK_EQ(*r.document.directives.get("project"), "P");
    CHECK_EQ(r.document.tasks.size(), 2u);
    CHECK_EQ(r.document.tasks[0].subtasks.size(), 1u);
}

// ---------------------------------------------------------------------------
// Views and blocks
// ---------------------------------------------------------------------------

TEST(views) {
    auto r = ok_parse("[ ] a\n---view:table columns=status,task,priority sort=due");
    REQUIRE(r.document.view.has_value());
    CHECK_EQ(r.document.view->type, "table");
    CHECK_EQ(*r.document.view->options.get("columns"), "status,task,priority");
    CHECK_EQ(*r.document.view->options.get("sort"), "due");

    auto bad = ok_parse("---view:pie");
    CHECK(has_code(bad.warnings, ErrorCode::InvalidViewType));
}

TEST(context_block) {
    auto r = ok_parse(
        "@agent claude-3\n"
        "---context\n"
        "{\n"
        "  \"session_id\": \"abc123\",\n"
        "  \"started_at\": \"2026-01-15T10:00:00Z\",\n"
        "  \"tokens\": 42\n"
        "}\n"
        "---\n"
        "[ ] after");
    const auto& ctx = r.document.agent_context;
    REQUIRE(ctx.has_value());
    CHECK_EQ(*ctx->model, "claude-3");
    CHECK_EQ(*ctx->session_id, "abc123");
    CHECK_EQ(*ctx->started_at, "2026-01-15T10:00:00Z");
    REQUIRE(ctx->metadata.has_value());
    CHECK_EQ(ctx->metadata->find("tokens")->as_number(), 42.0);
    CHECK_EQ(r.document.tasks.size(), 1u);
}

TEST(context_block_invalid_json_and_unterminated) {
    auto r = parse("---context\nnot json\n");
    CHECK(has_code(r.errors, ErrorCode::ExpectedFenceEnd));
    CHECK(has_code(r.warnings, ErrorCode::InvalidJson));
    CHECK_EQ(r.document.agent_context->metadata->find("raw")->as_string(), "not json");
}

TEST(handoff_block) {
    auto r = ok_parse(
        "---handoff\n"
        "from: claude-agent-1\n"
        "to: claude-agent-2\n"
        "reason: \"Specialized knowledge needed\"\n"
        "context: {\"files\": [\"a.cpp\"]}\n"
        "---\n");
    REQUIRE(r.document.handoff.has_value());
    CHECK_EQ(r.document.handoff->from, "claude-agent-1");
    CHECK_EQ(r.document.handoff->to, "claude-agent-2");
    CHECK_EQ(*r.document.handoff->reason, "Specialized knowledge needed");
    CHECK_EQ(r.document.handoff->context->find("files")->as_array().at(0).as_string(), "a.cpp");

    auto r2 = ok_parse("---handoff\nfrom: a\nto: b\ncontext: {...}\n---");
    CHECK_EQ(r2.document.handoff->context->find("raw")->as_string(), "{...}");
}

// ---------------------------------------------------------------------------
// Complete spec example
// ---------------------------------------------------------------------------

TEST(spec_complete_example) {
    std::string src = read_file(std::string(TASKML_TEST_DATA_DIR) + "/spec_example.taskml");
    REQUIRE(!src.empty());
    auto r = ok_parse(src);
    CHECK(r.warnings.empty());
    const Document& d = r.document;
    CHECK_EQ(*d.directives.get("sprint"), "Sprint 1: Foundation");
    REQUIRE(d.tasks.size() == 4);
    CHECK_EQ(d.tasks[0].subtasks.size(), 3u);
    CHECK_EQ(d.tasks[1].depends_on.at(0), "setup");
    CHECK_EQ(d.tasks[1].subtasks.at(1).notes.size(), 2u);
    CHECK_EQ(d.tasks[2].criteria.size(), 2u);
    CHECK_EQ(d.tasks[2].subtasks.size(), 4u);
    CHECK_EQ(d.tasks[3].blocked_by.at(0), "parser");
    CHECK(d.tasks[3].status == TaskStatus::Blocked);
    CHECK_EQ(d.view->type, "kanban");
    CHECK_EQ(*d.view->options.get("columns"), "pending,in_progress,blocked,completed");
    CHECK_EQ(d.comments.at(0).text, "Core Parser Implementation");
    CHECK(validate(src).empty());
    CHECK_EQ(d.find_task("lexer")->description, "Implement lexer");
    CHECK_EQ(d.all_tasks().size(), 4u + 3 + 3 + 4);
}

// ---------------------------------------------------------------------------
// Semantic validation
// ---------------------------------------------------------------------------

TEST(validate_semantics) {
    auto diags = validate(
        "[ ] a ^a\n    depends: ^b, ^missing\n"
        "[ ] b ^b\n    blocked-by: ^a\n"
        "[ ] c ^a\n    blocked-by: ^nobody\n");
    CHECK(has_code(diags, ErrorCode::DuplicateTaskId));
    CHECK(has_code(diags, ErrorCode::UnknownDependency));
    CHECK(has_code(diags, ErrorCode::UnknownBlocker));
    CHECK(has_code(diags, ErrorCode::CircularDependency));
    for (const auto& d : diags)
        if (d.code == ErrorCode::CircularDependency) CHECK_EQ(d.message, "Circular dependency detected: ^a -> ^b -> ^a");
}

TEST(validate_self_cycle_and_clean) {
    CHECK(has_code(validate("[ ] a ^a\n    depends: ^a"), ErrorCode::CircularDependency));
    CHECK(validate("@project P\n[ ] a ^a\n[ ] b ^b\n    depends: ^a").empty());
}

// ---------------------------------------------------------------------------
// Serialization round trips
// ---------------------------------------------------------------------------

TEST(to_taskml_canonical) {
    auto r = ok_parse("@project P\n[ ] Ship  it ^s1 !2026-01-15 #be @bob ~4h #p1\n  - note\n  ✓ done\n");
    std::string out = to_taskml(r.document);
    CHECK_EQ(out,
             "@project P\n"
             "\n"
             "[ ] Ship it #p1 ~4h @bob #be !2026-01-15 ^s1\n"
             "    - note\n"
             "    \xE2\x9C\x93 done\n");
}

TEST(round_trip_spec_example) {
    std::string src = read_file(std::string(TASKML_TEST_DATA_DIR) + "/spec_example.taskml");
    Document d = parse_or_throw(src);
    d.comments.clear();
    std::string text = to_taskml(d);
    Document d2 = parse_or_throw(text);
    CHECK(d == d2);
    CHECK_EQ(to_taskml(d2), text);  // idempotent
}

TEST(round_trip_tricky_text) {
    Document d;
    d.directives.set("project", "Tricky");
    Task t;
    t.description = "Use #hash @at ~1h !2026-01-01 ^id and back\\#slash";
    t.notes = {"note with \\@ and #tag", "- dashed"};
    Criterion c;
    c.description = "\xE2\x9C\x93 looks checked";
    c.status = CriterionStatus::Failed;
    c.evidence = "\"quoted\"";
    t.criteria.push_back(c);
    t.tags = {"x"};
    Task sub;
    sub.status = TaskStatus::Review;
    sub.description = "sub";
    t.subtasks.push_back(sub);
    d.tasks.push_back(t);
    Section s;
    s.title = "Later";
    s.level = 3;
    s.tasks.push_back(sub);
    d.sections.push_back(s);
    d.includes.push_back({"other.taskml", 0});
    d.view = ViewConfig{"timeline", {{"start", "2026-01-01"}}};
    AgentContext ctx;
    ctx.session_id = "abc";
    Json meta = Json::object();
    meta.set("session_id", "abc");
    ctx.metadata = meta;
    d.agent_context = ctx;
    Handoff h;
    h.from = "a1";
    h.to = "a2";
    h.reason = "because";
    Json hc = Json::object();
    hc.set("k", Json::array({1, "two", nullptr, true}));
    h.context = hc;
    d.handoff = h;

    std::string text = to_taskml(d);
    auto r = parse(text);
    for (const auto& e : r.errors) std::cerr << e.format(text) << "\n";
    CHECK(r.errors.empty());
    if (!(r.document == d)) {
        std::cerr << "--- serialized:\n" << text << "--- reparsed json:\n" << to_json(r.document) << "\n";
        CHECK(false);
    }

    // Tab indentation also round trips.
    SerializeOptions tabs;
    tabs.use_tabs = true;
    CHECK(parse_or_throw(to_taskml(d, tabs)) == d);
}

TEST(json_round_trip) {
    std::string src = read_file(std::string(TASKML_TEST_DATA_DIR) + "/spec_example.taskml");
    Document d = parse_or_throw(src);
    std::string j = to_json(d);
    Document d2 = from_json(j);
    CHECK(d == d2);
    CHECK_EQ(to_json(d2), j);
    Json v = Json::parse(j);
    CHECK_EQ(v.find("tasks")->as_array().at(0).find("status")->as_string(), "completed");
    CHECK_EQ(v.find("tasks")->as_array().at(1).find("dependsOn")->as_array().at(0).as_string(), "setup");
}

TEST(json_reference_shape) {
    auto r = ok_parse("[~] Build API #p1 ~8h @bob");
    CHECK_EQ(to_json(r.document, -1),
             "{\"version\":\"1.1\",\"tasks\":[{\"status\":\"in_progress\",\"description\":\"Build API\","
             "\"priority\":1,\"estimate\":\"8h\",\"assignee\":\"bob\"}]}");
}

TEST(from_json_errors) {
    auto expect_code = [](const char* json, ErrorCode code) {
        try {
            from_json(json);
            std::cerr << "    no error for " << json << "\n";
            CHECK(false);
        } catch (const Error& e) {
            REQUIRE(e.diagnostics().size() == 1);
            if (e.diagnostics()[0].code != code) std::cerr << "    got " << e.what() << "\n";
            CHECK(e.diagnostics()[0].code == code);
        }
    };
    expect_code("{", ErrorCode::InvalidJson);
    expect_code("{\"tasks\":[{\"status\":\"pending\"}]}", ErrorCode::MissingRequiredField);
    expect_code("{\"tasks\":[{\"status\":\"done\",\"description\":\"x\"}]}", ErrorCode::InvalidFieldType);
    expect_code("{\"tasks\":{}}", ErrorCode::InvalidFieldType);
    expect_code("{\"tasks\":[{\"status\":\"pending\",\"description\":\"x\",\"priority\":9}]}",
                ErrorCode::InvalidFieldType);
    // Minimal valid document.
    Document d = from_json("{\"tasks\":[]}");
    CHECK_EQ(d.version, "1.1");
}

// ---------------------------------------------------------------------------
// JSON value
// ---------------------------------------------------------------------------

TEST(json_value) {
    Json j = Json::parse(R"({"a":[1,2.5,-3e2,true,false,null],"s":"\u00e9\ud83d\ude00\n\"","o":{}})");
    CHECK_EQ(j.find("a")->as_array().size(), 6u);
    CHECK_EQ(j.find("a")->as_array()[2].as_number(), -300.0);
    CHECK_EQ(j.find("s")->as_string(), "\xC3\xA9\xF0\x9F\x98\x80\n\"");
    CHECK_EQ(j.dump(), R"({"a":[1,2.5,-300,true,false,null],"s":"é😀\n\"","o":{}})");
    CHECK(Json::parse(j.dump(2)) == j);
    for (const char* bad : {"", "[1,]", "{\"a\" 1}", "tru", "\"abc", "01", "[1] x", "\"\\ud800\""}) {
        bool threw = false;
        try {
            Json::parse(bad);
        } catch (const JsonParseError&) {
            threw = true;
        }
        if (!threw) std::cerr << "    accepted invalid JSON: " << bad << "\n";
        CHECK(threw);
    }
    std::string deep(1000, '[');
    bool threw = false;
    try {
        Json::parse(deep);
    } catch (const JsonParseError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// Robustness: parse must never throw or crash on arbitrary input.
// ---------------------------------------------------------------------------

TEST(fuzz_smoke) {
    const std::string alphabet = "[]x~!?- #p0123@^:=,\\/\n\t abc\xE2\x9C\x93\xE2\x97\x8B-<view:context";
    unsigned state = 12345;
    auto rnd = [&]() {
        state = state * 1103515245u + 12345u;
        return (state >> 16) & 0x7fff;
    };
    for (int iter = 0; iter < 3000; ++iter) {
        std::string s;
        int len = static_cast<int>(rnd() % 200);
        for (int i = 0; i < len; ++i) s += alphabet[rnd() % alphabet.size()];
        auto r = parse(s);
        (void)validate(r.document);
        std::string text = to_taskml(r.document);
        auto r2 = parse(text);
        (void)to_json(r2.document);
    }
    CHECK(true);
}

TEST(diagnostic_format) {
    auto r = parse("[ ] a\n    [ ] b\n  [ ] c");
    REQUIRE(!r.errors.empty());
    std::string f = r.errors[0].format("[ ] a\n    [ ] b\n  [ ] c");
    CHECK_EQ(f,
             "error[E101]: Inconsistent indentation: expected 4 spaces, got 2 (3:3)\n"
             "  suggestion: Use consistent indentation (4 spaces per level recommended)\n"
             "  3 |   [ ] c\n"
             "    |   ^");
}

TEST(versions) {
    CHECK_EQ(std::string(library_version()), TASKML_VERSION_STRING);
    CHECK_EQ(std::string(spec_version()), "1.1");
    auto r = ok_parse("@version 1.0\n");
    CHECK_EQ(r.document.version, "1.0");
}


// ---------------------------------------------------------------------------
// Markdown dialect
// ---------------------------------------------------------------------------

static ParseOptions md_opts() {
    ParseOptions o;
    o.dialect = Dialect::Markdown;
    return o;
}

TEST(markdown_basic) {
    const std::string src =
        "---\n"
        "project: Website Redesign\n"
        "sprint: \"Sprint #1\"\n"
        "goal: 'It''s live'\n"
        "tags:\n"
        "  - ignored\n"
        "---\n"
        "\n"
        "# Website Redesign\n"
        "\n"
        "Some prose about the project, with #hashtags and @mentions.\n"
        "\n"
        "- a plain list\n"
        "- outside of any task\n"
        "\n"
        "## Backend\n"
        "\n"
        "- [x] Design homepage #p0 ~2d @alice ^design\n"
        "- [~] Build API #p1 ~8h @bob ^api\n"
        "  - depends: ^design\n"
        "  - A note about **the** task\n"
        "    continued on the next line\n"
        "    - nested note\n"
        "  - [x] Setup database\n"
        "  * [ ] Create endpoints\n"
        "  1. ✓ Returns JSON\n"
        "     - evidence: \"curl output\"\n"
        "  - ○ Handles errors\n"
        "\n"
        "```\n"
        "- [ ] not a task, code\n"
        "```\n"
        "<!-- - [ ] not a task, comment\n"
        "-->\n"
        "| table | - [ ] cell |\n"
        "\n"
        "## Later ##\n"
        "\n"
        "- [ ] Write tests #p2 #qa\n"
        "  - blocked-by: ^api\n";
    auto r = ok_parse(src, md_opts());
    CHECK(r.warnings.empty());
    for (const auto& w : r.warnings) std::cerr << "    warning: " << w.format(src) << "\n";
    const Document& d = r.document;
    CHECK_EQ(d.directives.value_or("project", ""), "Website Redesign");
    CHECK_EQ(d.directives.value_or("sprint", ""), "Sprint #1");
    CHECK_EQ(d.directives.value_or("goal", ""), "It's live");
    CHECK(!d.directives.contains("tags"));
    REQUIRE(d.sections.size() == 3);
    CHECK_EQ(d.sections[0].title, "Website Redesign");
    CHECK_EQ(d.sections[0].level, 1);
    CHECK(d.sections[0].tasks.empty());
    CHECK_EQ(d.sections[1].title, "Backend");
    CHECK_EQ(d.sections[2].title, "Later");
    REQUIRE(d.sections[1].tasks.size() == 2);
    const Task& api = d.sections[1].tasks[1];
    CHECK(api.status == TaskStatus::InProgress);
    CHECK_EQ(api.description, "Build API");
    CHECK_EQ(*api.assignee, "bob");
    CHECK_EQ(*api.id, "api");
    CHECK_EQ(api.line, 19);
    REQUIRE(api.depends_on.size() == 1);
    REQUIRE(api.notes.size() == 2);
    CHECK_EQ(api.notes[0], "A note about **the** task continued on the next line");
    CHECK_EQ(api.notes[1], "nested note");
    REQUIRE(api.subtasks.size() == 2);
    CHECK(api.subtasks[0].status == TaskStatus::Completed);
    REQUIRE(api.criteria.size() == 2);
    CHECK_EQ(*api.criteria[0].evidence, "curl output");
    REQUIRE(d.sections[2].tasks.size() == 1);
    CHECK_EQ(d.sections[2].tasks[0].blocked_by[0], "api");
    CHECK_EQ(d.all_tasks().size(), 5u);
}

TEST(markdown_prose_ends_lists) {
    auto r = ok_parse("- [ ] A\n\nParagraph.\n\n  - not a note of A\n", md_opts());
    REQUIRE(r.document.tasks.size() == 1);
    CHECK(r.document.tasks[0].notes.empty());
}

TEST(markdown_diagnostics) {
    auto r = parse("- [ ] a\n    - [ ] b\n  - [ ] c\n- [y] what\n", md_opts());
    CHECK(has_code(r.errors, ErrorCode::InconsistentIndentation));
    CHECK(has_code(r.warnings, ErrorCode::UnknownStatus));
    auto u = parse("---\nproject: x\n", md_opts());
    CHECK(has_code(u.errors, ErrorCode::ExpectedFenceEnd));
    auto v = validate("- [ ] a ^x\n- [ ] b ^x\n", Dialect::Markdown);
    CHECK(has_code(v, ErrorCode::DuplicateTaskId));
    CHECK(validate("---\nteam: alpha\n---\n- [ ] a\n", Dialect::Markdown).empty());
}

TEST(markdown_round_trip) {
    const std::string src =
        "@project Demo\n@sprint Sprint: \"one\"\n\n"
        "[ ] Top #p1 @ann ^top\n"
        "    depends: ^b\n"
        "    - a note with \\#hash\n"
        "    [x] Sub\n"
        "    ✓ Works\n"
        "        evidence: \"log\"\n"
        "== Section ==\n"
        "[!] Blocked thing ^b\n";
    Document d = ok_parse(src).document;
    std::string md = to_markdown(d);
    CHECK_EQ(md,
             "---\n"
             "project: Demo\n"
             "sprint: \"Sprint: \\\"one\\\"\"\n"
             "---\n"
             "\n"
             "- [ ] Top #p1 @ann ^top\n"
             "  - depends: ^b\n"
             "  - a note with #hash\n"
             "  - [x] Sub\n"
             "  - (x) Works\n"
             "    - evidence: \"log\"\n"
             "\n"
             "## Section\n"
             "\n"
             "- [!] Blocked thing ^b\n");
    Document back = ok_parse(md, md_opts()).document;
    // Line numbers differ; compare through the canonical TaskML form.
    CHECK_EQ(to_taskml(back), to_taskml(d));
}

TEST(markdown_ascii_criteria) {
    auto r = ok_parse("- [ ] T\n  - ( ) pending one\n  - (x) verified\n  - (X) also\n  - (!) failed\n"
                      "  - (y) a note\n  - (x)no space is a note\n  - \\(x) escaped note\n", md_opts());
    CHECK(r.warnings.empty());
    REQUIRE(r.document.tasks.size() == 1);
    const Task& t = r.document.tasks[0];
    REQUIRE(t.criteria.size() == 4);
    CHECK(t.criteria[0].status == CriterionStatus::Pending);
    CHECK_EQ(t.criteria[0].description, "pending one");
    CHECK(t.criteria[1].status == CriterionStatus::Verified);
    CHECK(t.criteria[2].status == CriterionStatus::Verified);
    CHECK(t.criteria[3].status == CriterionStatus::Failed);
    REQUIRE(t.notes.size() == 3);
    CHECK_EQ(t.notes[2], "(x) escaped note");
    // TaskML accepts the ASCII forms too, and writes the spec's markers.
    auto tm = ok_parse("[ ] T\n    (x) done\n");
    REQUIRE(tm.document.tasks[0].criteria.size() == 1);
    CHECK(to_taskml(tm.document).find("\xE2\x9C\x93 done") != std::string::npos);
    // Notes that look like items survive a Markdown round trip.
    Document d;
    Task n;
    n.description = "N";
    n.notes = {"(x) not a criterion", "[x] not a task"};
    d.tasks.push_back(n);
    std::string md = to_markdown(d);
    auto back = ok_parse(md, md_opts());
    REQUIRE(back.document.tasks.size() == 1);
    CHECK_EQ(back.document.tasks[0].notes.size(), 2u);
    CHECK(back.document.tasks[0].criteria.empty());
    CHECK(back.document.tasks[0].subtasks.empty());
}

TEST(dialect_for_path) {
    CHECK(dialect_for_path("PLAN.md") == Dialect::Markdown);
    CHECK(dialect_for_path("x/Notes.MARKDOWN") == Dialect::Markdown);
    CHECK(dialect_for_path("plan.taskml") == Dialect::TaskML);
    CHECK(dialect_for_path("md") == Dialect::TaskML);
}

int main(int argc, char** argv) {
    int run = 0;
    for (const auto& t : registry()) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        g_current_failed = false;
        t.fn();
        ++run;
        if (g_current_failed) {
            ++g_failures;
            std::cerr << "FAIL " << t.name << "\n";
        }
    }
    std::cout << run - g_failures << "/" << run << " tests passed\n";
    return g_failures ? 1 : 0;
}
