// The AgentRunner backends and helpers they share.
#pragma once

#include <memory>
#include <string>

#include <taskml/json.hpp>

#include "myteam/runner.hpp"

namespace myteam {

std::unique_ptr<AgentRunner> make_claude_runner(RunnerSettings settings);
std::unique_ptr<AgentRunner> make_codex_runner(RunnerSettings settings);
std::unique_ptr<AgentRunner> make_vibe_runner(RunnerSettings settings);

namespace runner_detail {

using taskml::Json;

/// Parse one JSON line; nullopt for anything else (banners, progress).
std::optional<Json> parse_json(std::string_view line);
std::string str(const Json* j);
long long num(const Json* j);
double dbl(const Json* j);
/// A path into nested objects: get(j, {"message", "content"}).
const Json* get(const Json& j, std::initializer_list<const char*> path);
/// A short description of tool input: its command, path or pattern, else
/// compact JSON, cut to `max` characters.
std::string tool_detail(const Json* input, std::size_t max = 300);

void emit(const EventSink& sink, AgentEvent::Kind kind, std::string text, std::string tool = {});

}  // namespace runner_detail

}  // namespace myteam
