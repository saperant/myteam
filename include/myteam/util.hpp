// Small helpers shared by the library.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace myteam {

namespace fs = std::filesystem;

/// Read a whole file; throws std::runtime_error when it cannot be read.
std::string read_file(const fs::path& path);
/// Read a whole file; nullopt when it does not exist or cannot be read.
std::optional<std::string> try_read_file(const fs::path& path);
/// Write through a temporary file and rename, so readers never see half a file.
void write_file_atomic(const fs::path& path, std::string_view content);
/// Append to a file, creating it (and its directory) when missing.
void append_file(const fs::path& path, std::string_view content);

std::string_view trim(std::string_view s);
std::string lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool contains_ci(std::string_view haystack, std::string_view needle);
std::vector<std::string> split_lines(std::string_view text);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
/// "@Developer" / "developer" -> "developer".
std::string normalize_handle(std::string_view handle);
/// "Build the API!" -> "build-the-api" (at most `max` characters).
std::string slug(std::string_view text, std::size_t max = 40);
/// The first line of `text`, trimmed, cut at a word boundary to `max` characters.
std::string first_line(std::string_view text, std::size_t max = 200);
/// A task title from a line: the whole line when short, else its first
/// sentence (or the line cut at a word), with the rest returned for a note.
/// The #tags (#epic, \\#sprint-5) and #pN in a new task's title: taken out of it, into `tags`
/// and `priority` (agents write them into titles, where they would be kept as escaped text).
/// Returns the title without them.
std::string take_title_tags(const std::string& title, std::vector<std::string>& tags, std::optional<int>& priority);
std::pair<std::string, std::string> title_and_rest(std::string_view line, std::size_t max = 100);
/// An estimate as TaskML writes it (whole hours, days or weeks): "1h30m" and
/// "90m" -> "2h", "1.5h" -> "2h", "3 hours" -> "3h", "2d" -> "2d"; "" when it is
/// not an estimate.
std::string normalize_estimate(std::string_view e);
/// A line of a new task's text that is an acceptance criterion ("accept: ...",
/// or "( ) ...", either after "- "): its text; else nullopt.
std::optional<std::string> criterion_line(std::string_view line);
/// About the same thing: similar_text, or most of the shorter title's content
/// words in the longer one ("The guard protects .github/workflows/" and "The
/// guard protects .github/workflows/, so a CI role can't deliver CI").
bool same_subject(std::string_view a, std::string_view b);
/// One shell word: 'it''s' quoted for sh.
std::string shell_quote(std::string_view s);
/// Most of the content words of the first lines are the same (Jaccard, with
/// stop words dropped and a crude plural stem).
bool similar_text(std::string_view a, std::string_view b, double threshold = 0.6);
/// Rough token count for budgeting prompts (about 4 bytes per token).
std::size_t estimate_tokens(std::string_view text);

using Clock = std::chrono::system_clock;
/// 2026-11-06T14:03:22Z
std::string iso_timestamp(Clock::time_point t = Clock::now());
/// 20261106-140322 (for file names)
std::string file_timestamp(Clock::time_point t = Clock::now());
/// Days since 1970-01-01 for "YYYY-MM-DD", nullopt when malformed.
std::optional<long> parse_date_days(std::string_view date);
long days_since_epoch(Clock::time_point t = Clock::now());

/// Find an executable on $PATH (or check an explicit path).
std::optional<fs::path> find_executable(std::string_view name);

}  // namespace myteam
