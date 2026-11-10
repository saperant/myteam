// Contracts between an org's units (interfaces/<name>/README.md, version: in its front
// matter): when one's version changes, the open tasks that name it hear of it.
//
// Otherwise the members working from a contract find out it changed by diffing it. A version bump puts a note on every open task whose description,
// notes or criteria name "interfaces/<name>", and the next stand-up lists them.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace myteam {

class Project;

/// The org's interfaces/ for a project: <dir>/interfaces (the org) or <dir>/../interfaces (a
/// unit); empty when neither is a directory.
std::filesystem::path interfaces_dir(const std::filesystem::path& project_dir);

/// Each contract's version: directory name -> the version: in its README.md front matter
/// ("" when it names none). Directories without a README.md are not contracts.
std::map<std::string, std::string> contract_versions(const std::filesystem::path& interfaces);

struct ContractChange {
    std::string contract;            ///< "events"
    std::string from, to;            ///< versions
    std::vector<std::string> tasks;  ///< keys of the open tasks noted ("^id")
};

/// Compares the versions with those last seen (<state_dir>/contracts-seen) and records the new
/// ones. The first look records only: no versions were seen before it. For each change, every
/// open task in the plan that names "interfaces/<name>" gets a note "Contract interfaces/<name>
/// is version <to> now (was <from>, DATE): ..." and the change goes on the list for the next
/// stand-up (<state_dir>/contracts-changed.md). A removed contract is no change.
std::vector<ContractChange> note_contract_changes(const std::filesystem::path& interfaces,
                                                  const std::filesystem::path& state_dir,
                                                  const std::filesystem::path& plan_path);
std::vector<ContractChange> note_contract_changes(const Project& project);

/// The stand-up's list of contracts changed since the last stand-up ("" when none);
/// clear_contract_changes empties it once the stand-up is held.
std::string contract_changes_for_standup(const std::filesystem::path& state_dir);
void clear_contract_changes(const std::filesystem::path& state_dir);

}  // namespace myteam
