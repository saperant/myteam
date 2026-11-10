#include <gtest/gtest.h>

#include "myteam/frontmatter.hpp"
#include "myteam/yaml.hpp"

using namespace myteam;

TEST(Yaml, MappingsSequencesAndScalars) {
    yaml::Node n = yaml::parse(R"(
# comment
name: Builder            # trailing comment
role: "implementation: engineer"
quote: 'it''s'
url: http://example.com/#anchor
skills: [a, "b, c", d]
weights: {priority: 4, skill: 3}
models:
  default: claude:sonnet
  premium: claude:opus
handoffs:
  - when: done
    to: verifier
  - when: bug found
    to: fixer
    note: fix it
list:
- one
- two
empty:
flag: true
)");
    EXPECT_EQ(n.get("name"), "Builder");
    EXPECT_EQ(n.get("role"), "implementation: engineer");
    EXPECT_EQ(n.get("quote"), "it's");
    EXPECT_EQ(n.get("url"), "http://example.com/#anchor");
    ASSERT_EQ(n["skills"].size(), 3u);
    EXPECT_EQ(n["skills"].items()[1].text(), "b, c");
    EXPECT_EQ(n["weights"].get_int("priority", 0), 4);
    EXPECT_EQ(n["models"].get("default"), "claude:sonnet");
    ASSERT_EQ(n["handoffs"].size(), 2u);
    EXPECT_EQ(n["handoffs"].items()[1].get("note"), "fix it");
    EXPECT_EQ(n["list"].as_strings(), (std::vector<std::string>{"one", "two"}));
    EXPECT_TRUE(n["empty"].is_null());
    EXPECT_TRUE(n.has("empty"));
    EXPECT_TRUE(n.get_bool("flag", false));
    EXPECT_TRUE(n["missing"].is_null());
}

TEST(Yaml, BlockScalars) {
    yaml::Node n = yaml::parse("literal: |\n  line one\n  line two\n\nfolded: >-\n  a\n  b\n\n  c\nafter: x\n");
    EXPECT_EQ(n.get("literal"), "line one\nline two\n");
    EXPECT_EQ(n.get("folded"), "a b\nc");
    EXPECT_EQ(n.get("after"), "x");
}

TEST(Yaml, NestedSequencesAndMultilineFlow) {
    yaml::Node n = yaml::parse("matrix:\n  - - 1\n    - 2\n  - [3, 4]\nflow: [a,\n  b]\n");
    ASSERT_EQ(n["matrix"].size(), 2u);
    EXPECT_EQ(n["matrix"].items()[0].as_strings(), (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(n["flow"].as_strings(), (std::vector<std::string>{"a", "b"}));
}

TEST(Yaml, Errors) {
    EXPECT_THROW(yaml::parse("a: 1\na: 2\n"), yaml::Error);
    EXPECT_THROW(yaml::parse("a: [1, 2\n"), yaml::Error);
    EXPECT_THROW(yaml::parse("a: \"open\n"), yaml::Error);
    try {
        yaml::parse("a: 1\n  b: 2\n");
        FAIL();
    } catch (const yaml::Error& e) {
        EXPECT_EQ(e.line(), 2);
    }
    EXPECT_THROW(yaml::parse("n: x").as_int(), yaml::Error);
    EXPECT_THROW(yaml::parse("n: x")["n"].as_int(), yaml::Error);
}

TEST(FrontMatter, SplitsMetaAndBody) {
    MarkdownFile f = parse_markdown_file("---\nname: A\n---\n\nBody line\n");
    EXPECT_EQ(f.meta.get("name"), "A");
    EXPECT_EQ(f.body, "Body line\n");
    EXPECT_EQ(f.body_line, 5);
    MarkdownFile plain = parse_markdown_file("# Just markdown\n");
    EXPECT_TRUE(plain.meta.is_null());
    EXPECT_EQ(plain.body, "# Just markdown\n");
    EXPECT_THROW(parse_markdown_file("---\nname: A\n"), yaml::Error);
}
