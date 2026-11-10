// Agents' commits, through the git_commit tool.
//
// The agent and its MCP server run in the sandbox, where .git is read-only.
// So the tool does not commit: it hands the request to myteam, outside the
// sandbox, through files in the run directory (<run>/commits/N.request.json,
// answered by N.response.json). The CommitBroker, a thread of the loop while
// the agent runs, checks each request against the run's checkpoint and the
// safeguards and commits (commit_run_work), and the tool tells the agent what
// happened. The run record and the changelog list the commits.
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/checkpoint.hpp"
#include "myteam/project.hpp"

namespace myteam {

taskml::Json commit_to_json(const RunCommit& c);
RunCommit commit_from_json(const taskml::Json& j);
taskml::Json commit_request_to_json(const CommitRequest& r);
CommitRequest commit_request_from_json(const taskml::Json& j);

/// The requests directory of a run.
std::filesystem::path commits_dir(const std::filesystem::path& run_dir);

class CommitBroker {
public:
    /// `before` empty: no commits (only the services' requests are answered).
    CommitBroker(const Project& project, std::optional<Snapshot> before, CommitIdentity who,
                 std::filesystem::path run_dir);
    ~CommitBroker();
    CommitBroker(const CommitBroker&) = delete;
    CommitBroker& operator=(const CommitBroker&) = delete;

    void start();
    /// Answer what is waiting, then stop.
    void stop();
    /// Answer the requests waiting now (start() does this in its thread).
    void poll();

    std::vector<RunCommit> commits() const;

private:
    const Project& project_;
    std::optional<Snapshot> before_;
    CommitIdentity who_;
    std::filesystem::path dir_;
    std::vector<RunCommit> commits_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

/// The tool's side: ask, and wait for the answer (an error result on timeout).
RunCommit request_commit(const std::filesystem::path& run_dir, const CommitRequest& req, int timeout_seconds = 900);

/// A service operation (status, logs, restart, reset), answered by myteam
/// outside the sandbox the same way.
struct ServiceAnswer;
ServiceAnswer request_service(const std::filesystem::path& run_dir, const std::string& action, const std::string& name,
                              int lines = 80, int timeout_seconds = 900);

}  // namespace myteam
