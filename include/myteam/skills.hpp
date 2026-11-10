// Skills: what the team learned to do, as files an agent loads when its task needs them.
//
//   .myteam/skills/<slug>/SKILL.md
//     ---
//     name: Extract text from a vendor PDF
//     description: One line: when to use it.
//     triggers: [pdf, manual]         words in a task that call for it
//     members: [designer]             who it is for; empty: everyone
//     source: "Sprint 4 retrospective"
//     ---
//     The how, short and concrete.
//
// The retrospective and the steward write them (<skill> blocks, a "skill" proposal, applied
// at once by default: a skill adds knowledge and changes nobody's role). myorg writes the
// org's practices into the units as skills too (org-*). A task's context lists every skill
// (name: description) and has the full text of those its words call for; the MCP tool
// skill_read gives any other.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <taskml/taskml.hpp>

#include "myteam/project.hpp"

namespace myteam {

struct Skill {
    std::string slug, name, description, source, body;
    std::vector<std::string> triggers, members;
    std::filesystem::path file;
};

std::filesystem::path skills_dir(const Project& project);
std::vector<Skill> load_skills(const Project& project);
/// Write (or replace) a skill; returns its file.
std::filesystem::path write_skill(const Project& project, const Skill& skill);
/// A <skill name="" triggers="a, b" members="h">description\nbody</skill> block.
std::vector<Skill> parse_skill_blocks(std::string_view text);
/// The block for a skill (what a "skill" proposal holds).
std::string skill_block(const Skill& s);
/// The skills `task` calls for (a trigger in its title, tags or notes), for `member`; at most `max`.
std::vector<const Skill*> skills_for(const std::vector<Skill>& skills, const taskml::Task& task, const std::string& member,
                                     std::size_t max = 2);
/// The task context's section: the list, and the full text of those it calls for; "" none.
std::string skills_section(const std::vector<Skill>& skills, const taskml::Task& task, const std::string& member);

}  // namespace myteam
