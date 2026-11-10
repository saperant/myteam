// What a UI needs to watch the crew: `myteam json WHAT`, one JSON document on stdout.
// The myrcp workbench plugins (ui/) read these; anything else may too.
//
//   overview      the project, the goal, counts, what needs you, the coach's state, who is
//                 running now, the latest runs, the cost so far
//   plan          every task: status, assignee, criteria, notes, blockers
//   team          the crew: each member's role (coach, gatekeeper, expert), unit, models,
//                 memory (coaching, lessons), current work, runs, cost
//   runs          the runs, newest first, with their kind (task, check, coach, review,
//                 council, repair; scrum for the old ones) and the running ones marked
//   run NAME      one run: its record, report, guard, commits, final text
//   events NAME   the run's events as the runner reported them (live); --from N skips N
//   conversation  the customer and the coach, in order, and what the coach has not seen
//   coach         the coach's state: news waiting, a held objection and its decisions,
//                 the coach's notes, counts, cost
//   digest        what the coach reads at its next decision (the whole prompt)
//   council       the councils, newest first: the question and each view
//   skills        what the crew learned to do
//   feedback      the crew's feedback for myteam
//
// Shapes are additive: fields may be added, not removed or renamed.
#pragma once

#include <string>

#include <taskml/json.hpp>

#include "myteam/project.hpp"
#include "myteam/team.hpp"

namespace myteam::inspect {

taskml::Json overview(const Project& project, const Team& team);
taskml::Json plan(const Project& project);
taskml::Json team(const Project& project, const Team& team);
taskml::Json runs(const Project& project, int max = 200);
taskml::Json run(const Project& project, const std::string& name);
taskml::Json events(const Project& project, const std::string& name, std::size_t from = 0);
taskml::Json conversation(const Project& project);
taskml::Json coach(const Project& project, const Team& team);
taskml::Json digest(const Project& project, const Team& team);
taskml::Json council(const Project& project);
taskml::Json skills(const Project& project);
taskml::Json feedback(const Project& project);

}  // namespace myteam::inspect
