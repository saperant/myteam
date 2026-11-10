// SPDX-License-Identifier: MIT
// Minimal example of using the taskml library from C++.

#include <taskml/taskml.hpp>

#include <iostream>

int main() {
    const char* source = R"(@project Website Redesign
@sprint Sprint 1

[x] Design homepage #p0 ~2d @alice ^design
[~] Build API #p1 ~8h @bob ^api
    depends: ^design
    [x] Setup database
    [ ] Create endpoints
    ✓ Returns JSON
    ○ Handles errors
[ ] Write tests #p2 #qa

---view:kanban
)";

    taskml::ParseResult result = taskml::parse(source);
    for (const auto& d : result.errors) std::cerr << d.format(source) << "\n";
    if (!result.ok()) return 1;

    const taskml::Document& doc = result.document;
    std::cout << "Project: " << doc.directives.value_or("project", "?") << "\n";
    for (const taskml::Task* t : doc.all_tasks()) {
        std::cout << "  [" << taskml::to_string(t->status) << "] " << t->description;
        if (t->assignee) std::cout << " (" << *t->assignee << ")";
        std::cout << "\n";
    }

    // Modify and write back.
    taskml::Task* api = result.document.find_task("api");
    api->status = taskml::TaskStatus::Completed;
    std::cout << "\n" << taskml::to_taskml(result.document) << "\n";
    std::cout << taskml::to_json(result.document) << "\n";
    return 0;
}
