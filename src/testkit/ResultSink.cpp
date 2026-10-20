#include "testkit/ResultSink.h"

#include <cstdlib>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "version_gen.h"

namespace cs2st
{
namespace
{

/**
 * Appends `text` to `out` as the inside of a JSON string.
 *
 * Details carry bytes straight from the engine (names, paths, error text), so
 * they are not guaranteed to be valid UTF-8; anything that is not becomes '?'
 * rather than producing a file no parser will open.
 */
void AppendEscaped(std::string& out, const std::string& text)
{
	static const char kHex[] = "0123456789abcdef";

	for (std::size_t i = 0; i < text.size();)
	{
		const unsigned char c = static_cast<unsigned char>(text[i]);

		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += static_cast<char>(c);
			++i;
		}
		else if (c == '\n')
		{
			out += "\\n";
			++i;
		}
		else if (c == '\r')
		{
			out += "\\r";
			++i;
		}
		else if (c == '\t')
		{
			out += "\\t";
			++i;
		}
		else if (c < 0x20)
		{
			out += "\\u00";
			out += kHex[c >> 4];
			out += kHex[c & 0xF];
			++i;
		}
		else if (c < 0x80)
		{
			out += static_cast<char>(c);
			++i;
		}
		else
		{
			// Multi-byte sequence: accept it only if the continuation bytes are there.
			std::size_t length = 0;
			if ((c & 0xE0) == 0xC0)
			{
				length = 2;
			}
			else if ((c & 0xF0) == 0xE0)
			{
				length = 3;
			}
			else if ((c & 0xF8) == 0xF0)
			{
				length = 4;
			}

			bool valid = length != 0 && i + length <= text.size();
			for (std::size_t k = 1; valid && k < length; ++k)
			{
				valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
			}

			if (valid)
			{
				out.append(text, i, length);
				i += length;
			}
			else
			{
				out += '?';
				++i;
			}
		}
	}
}

/** `"key":"value"` with no leading comma. */
void AppendString(std::string& out, const char* key, const std::string& value)
{
	out += '"';
	out += key;
	out += "\":\"";
	AppendEscaped(out, value);
	out += '"';
}

void AppendNumber(std::string& out, const char* key, long long value)
{
	out += '"';
	out += key;
	out += "\":";
	out += std::to_string(value);
}

} // namespace

ResultSink& ResultSink::Get()
{
	static ResultSink instance;
	return instance;
}

std::string ResultSink::MetaJson() const
{
	std::string out;
	AppendString(out, "plugin", PLUGIN_NAME);
	out += ',';
	AppendString(out, "version", PLUGIN_FULL_VERSION);
	out += ',';
	AppendString(out, "git", PLUGIN_GIT_REVISION);
	out += ',';
	AppendString(out, "platform", PLUGIN_BUILD_PLATFORM);
	out += ',';
	AppendString(out, "build_type", PLUGIN_BUILD_TYPE);
	out += ',';
	AppendString(out, "sdk", PLUGIN_SDK_ENGINE);
	out += ',';
	AppendNumber(out, "sdk_engine_code", PLUGIN_SDK_ENGINE_CODE);
	out += ',';
	AppendNumber(out, "metamod_plapi", PLUGIN_MM_PLAPI_VERSION);
	out += ',';
	AppendString(out, "scope", scope_);
	out += ',';
	AppendString(out, "started", started_utc_);

	// The plugin cannot see the game's Steam build id from in here; whoever
	// launched the server can, and passes it through.
	const char* build_id = std::getenv("CST_BUILD_ID");
	if (build_id != nullptr && build_id[0] != '\0')
	{
		out += ',';
		AppendString(out, "game_buildid", build_id);
	}
	return out;
}

bool ResultSink::Open()
{
	Close();
	entries_.clear();

	const char* env_dir = std::getenv("CST_RESULT_DIR");
	std::string directory = directory_override_;
	if (directory.empty() && env_dir != nullptr && env_dir[0] != '\0')
	{
		directory = env_dir;
	}
	if (directory.empty())
	{
		directory = fs::LogDirectory();
	}

	if (!fs::CreateDirectories(directory))
	{
		CS2ST_LOG_WARN("results: cannot create '%s'; no results.json this run\n",
			directory.c_str());
		return false;
	}

	jsonl_path_ = fs::Join(directory, "results.jsonl");
	json_path_ = fs::Join(directory, "results.json");
	started_utc_ = fs::TimestampUtc();

	// A stale summary from a previous run must not outlive the start of this
	// one: if this run dies, "no results.json" has to mean exactly that.
	std::remove(json_path_.c_str());

	file_ = std::fopen(jsonl_path_.c_str(), "wb");
	if (file_ == nullptr)
	{
		CS2ST_LOG_WARN("results: cannot open '%s'\n", jsonl_path_.c_str());
		return false;
	}

	run_start_ = std::chrono::steady_clock::now();
	call_open_ = false;

	WriteLine("{\"ev\":\"start\"," + MetaJson() + "}");
	return true;
}

void ResultSink::WriteLine(const std::string& line)
{
	if (file_ == nullptr)
	{
		return;
	}
	std::fwrite(line.data(), 1, line.size(), file_);
	std::fputc('\n', file_);
	// Flushed per line on purpose: the whole point is that the last line
	// survives the process dying right after it.
	std::fflush(file_);
}

void ResultSink::Begin(const std::string& suite, const std::string& call)
{
	if (file_ == nullptr)
	{
		return;
	}

	call_start_ = std::chrono::steady_clock::now();
	call_open_ = true;

	std::string line = "{\"ev\":\"begin\",";
	AppendString(line, "suite", suite);
	line += ',';
	AppendString(line, "call", call);
	line += '}';
	WriteLine(line);
}

void ResultSink::Result(const std::string& suite, const std::string& call,
	CallOutcome outcome, const std::string& detail)
{
	if (file_ == nullptr)
	{
		return;
	}

	Entry entry;
	entry.suite = suite;
	entry.call = call;
	entry.outcome = CallOutcomeName(outcome);
	entry.detail = detail;
	if (call_open_)
	{
		entry.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - call_start_).count();
		call_open_ = false;
	}

	std::string line = "{\"ev\":\"result\",";
	AppendString(line, "suite", entry.suite);
	line += ',';
	AppendString(line, "call", entry.call);
	line += ',';
	AppendString(line, "outcome", entry.outcome);
	line += ',';
	AppendNumber(line, "ms", entry.ms);
	line += ',';
	AppendString(line, "detail", entry.detail);
	line += '}';
	WriteLine(line);

	entries_.push_back(std::move(entry));
}

void ResultSink::Finish(const RunTally& tally)
{
	if (file_ == nullptr)
	{
		return;
	}

	const long long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - run_start_).count();

	std::string json = "{\n  ";
	json += MetaJson();
	json += ",\n  \"completed\":true,\n  ";
	AppendNumber(json, "duration_ms", total_ms);
	json += ",\n  \"summary\":{";
	AppendNumber(json, "total", static_cast<long long>(tally.Total()));
	json += ',';
	AppendNumber(json, "ok", static_cast<long long>(tally.ok));
	json += ',';
	AppendNumber(json, "failed", static_cast<long long>(tally.failed));
	json += ',';
	AppendNumber(json, "crashed", static_cast<long long>(tally.crashed));
	json += ',';
	AppendNumber(json, "suspect", static_cast<long long>(tally.suspect));
	json += ',';
	AppendNumber(json, "unavailable", static_cast<long long>(tally.unavailable));
	json += ',';
	AppendNumber(json, "skipped", static_cast<long long>(tally.skipped));
	json += "},\n  \"results\":[";

	for (std::size_t i = 0; i < entries_.size(); ++i)
	{
		const Entry& e = entries_[i];
		json += (i == 0) ? "\n    {" : ",\n    {";
		AppendString(json, "suite", e.suite);
		json += ',';
		AppendString(json, "call", e.call);
		json += ',';
		AppendString(json, "outcome", e.outcome);
		json += ',';
		AppendNumber(json, "ms", e.ms);
		json += ',';
		AppendString(json, "detail", e.detail);
		json += '}';
	}
	json += "\n  ]\n}\n";

	WriteLine("{\"ev\":\"finish\",\"completed\":true}");
	Close();

	// Write-then-rename: a reader polling for results.json never sees half of it.
	const std::string tmp = json_path_ + ".tmp";
	std::FILE* out = std::fopen(tmp.c_str(), "wb");
	if (out == nullptr)
	{
		CS2ST_LOG_WARN("results: cannot write '%s'\n", tmp.c_str());
		return;
	}
	const std::size_t written = std::fwrite(json.data(), 1, json.size(), out);
	std::fclose(out);
	if (written != json.size())
	{
		CS2ST_LOG_WARN("results: short write to '%s'\n", tmp.c_str());
		std::remove(tmp.c_str());
		return;
	}

	std::remove(json_path_.c_str()); // rename() does not replace on Windows
	if (std::rename(tmp.c_str(), json_path_.c_str()) != 0)
	{
		CS2ST_LOG_WARN("results: cannot move '%s' into place\n", tmp.c_str());
		return;
	}
	CS2ST_LOG_INFO("results written to %s\n", json_path_.c_str());
}

void ResultSink::Abort(const std::string& reason)
{
	if (file_ == nullptr)
	{
		return;
	}

	std::string line = "{\"ev\":\"abort\",";
	AppendString(line, "reason", reason);
	line += '}';
	WriteLine(line);
	Close();
}

void ResultSink::Close()
{
	if (file_ != nullptr)
	{
		std::fclose(file_);
		file_ = nullptr;
	}
}

} // namespace cs2st
