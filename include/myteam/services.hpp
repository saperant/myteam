// The project's services: containers myteam runs for the team (a database,
// a proxy), outside the sandbox.
//
//   services:
//     db:
//       image: docker.io/library/postgres:16-alpine
//       ports: ["127.0.0.1:5432:5432"]
//       env: {POSTGRES_DB: shop, POSTGRES_USER: shop, POSTGRES_PASSWORD: shop}
//       data: /var/lib/postgresql/data          # a named volume, kept across restarts
//       agent_env: {DATABASE_URL: "postgres://shop:shop@127.0.0.1:5432/shop"}
//     proxy:
//       image: docker.io/library/nginx:alpine
//       network: host                           # (or ports)
//       files: {/etc/nginx/conf.d/default.conf: deploy/nginx.conf}   # read-only, from the project
//
// Agents get the addresses (agent_env) and, through the MCP server, tools
// to see, restart, read the logs of and reset a service; they cannot change
// what runs: images, mounts and ports are the operator's (config.yaml is
// protected). Each service is a container named myteam-<project>-<name>,
// so nothing else of the user's podman is touched; it is recreated when its
// definition changes. Ports bind to 127.0.0.1 only; files mount read-only
// and only from inside the project. Podman is the host's, through its socket
// when there is one (a toolbox's own podman often cannot run), else
// `podman`; `services_podman:` overrides.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <taskml/json.hpp>

#include "myteam/project.hpp"

namespace myteam {

/// What is wrong with a service's definition ("" each when nothing).
std::vector<std::string> service_problems(const Project& project, const ServiceSpec& s);

/// The podman command line prefix.
std::vector<std::string> podman_command(const Project& project);

/// The container's name: myteam-<project>-<service>.
std::string service_container(const Project& project, const ServiceSpec& s);

struct ServiceStatus {
    std::string name;
    std::string container;
    std::string state;  ///< running, exited, created, missing, invalid, ...
    std::string error;
};

struct ServicesResult {
    std::vector<std::string> notes;     ///< what was started or recreated
    std::vector<std::string> problems;  ///< what could not be
    std::vector<ServiceStatus> services;
};

/// Every service running, as defined (started, or recreated after a change).
ServicesResult ensure_services(const Project& project);
/// The services as they are now.
std::vector<ServiceStatus> service_status(const Project& project);
/// Stop and remove the containers (and, with `purge`, their data volumes).
ServicesResult stop_services(const Project& project, bool purge);

/// One operation, for the operator or an agent: "status", "restart", "logs",
/// "reset" (data removed, started fresh). The answer is text to show.
struct ServiceAnswer {
    bool ok = false;
    std::string text;
};
ServiceAnswer service_action(const Project& project, const std::string& action, const std::string& name, int lines = 80);

/// What every agent's environment gets (the services' agent_env).
std::map<std::string, std::string> service_agent_env(const Project& project);
/// The services, for the agents' context ("" when there are none).
std::string services_brief(const Project& project, bool tools);

taskml::Json services_json(const Project& project);

}  // namespace myteam
