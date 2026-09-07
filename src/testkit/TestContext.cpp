#include "testkit/TestContext.h"

#include <cstdarg>
#include <cstdio>

#include "core/Log.h"
#include "data/GameModules.h"

namespace cs2st
{
namespace
{

std::string FormatToString(const char* format, va_list args)
{
	char buffer[2048];
	if (std::vsnprintf(buffer, sizeof(buffer), format, args) < 0)
	{
		return std::string();
	}
	return buffer;
}

// Width of the target column. Wide enough for the longest name in the SDK so
// far ("IVEngineServer2::GetNumSplitScreenUsersAttachedToEdict", 53).
constexpr int kTargetColumn = 56;

/**
 * Forces a name into the column so the outcome column stays vertical.
 *
 * "%-56s" does not truncate, so one long name shifts every column after it on
 * that row - which defeats the purpose of a column you scan down. The tail is
 * kept rather than the head: the interface prefix repeats on every row, the
 * method name is what identifies it. A leading '~' marks what was cut.
 */
std::string FitTarget(const std::string& target)
{
	const std::size_t width = static_cast<std::size_t>(kTargetColumn);
	if (target.size() <= width)
	{
		return target;
	}
	return "~" + target.substr(target.size() - (width - 1));
}

} // namespace

void RunTally::Count(CallOutcome outcome)
{
	switch (outcome)
	{
		case CallOutcome::kOk:          ++ok;          break;
		case CallOutcome::kCrashed:     ++crashed;     break;
		case CallOutcome::kSkipped:     ++skipped;     break;
		case CallOutcome::kUnavailable: ++unavailable; break;
		case CallOutcome::kFailed:      ++failed;      break;
		case CallOutcome::kSuspect:     ++suspect;     break;
	}
}

const Module& TestContext::GetModule(const std::string& stem)
{
	// Goes through the table so the handful of modules with irregular names
	// (steam_api, ...) resolve correctly rather than by plain decoration.
	const std::string file = ResolveModuleFileName(stem);

	auto it = modules_.find(file);
	if (it != modules_.end())
	{
		return it->second;
	}

	// An absent module is cached too: "not loaded" is a stable answer for the
	// length of a run, and re-asking costs a dlopen every time on Linux.
	auto inserted = modules_.emplace(file, Module::FindExact(file));
	return inserted.first->second;
}

void* TestContext::QueryInterface(const std::string& module_stem, const char* version)
{
	const Module& module = GetModule(module_stem);
	if (!module.IsValid())
	{
		return nullptr;
	}
	return module.QueryInterface(version);
}

void* TestContext::QueryInterfaceAnywhere(const char* version, std::string* found_in)
{
	std::size_t count = 0;
	const GameModuleDesc* table = GetGameModules(count);

	for (std::size_t i = 0; i < count; ++i)
	{
		const Module& module = GetModule(table[i].id);
		if (!module.IsValid())
		{
			continue;
		}

		void* iface = module.QueryInterface(version);
		if (iface != nullptr)
		{
			if (found_in != nullptr)
			{
				*found_in = table[i].id;
			}
			return iface;
		}
	}

	if (found_in != nullptr)
	{
		found_in->clear();
	}
	return nullptr;
}

void TestContext::BeginSection(const char* id, const char* display_name)
{
	section_ = id;
	LogRaw("\n--- %s (%s) %s\n", display_name, id,
		"------------------------------------------");
}

void TestContext::EndSection()
{
	section_.clear();
}

void TestContext::Note(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	const std::string text = FormatToString(format, args);
	va_end(args);

	LogRaw("    %s\n", text.c_str());
}

void TestContext::Detail(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	pending_detail_ = FormatToString(format, args);
	va_end(args);
}

void TestContext::Fail(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	pending_failure_ = FormatToString(format, args);
	va_end(args);
}

void TestContext::Suspect(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	pending_suspect_ = FormatToString(format, args);
	va_end(args);
}

CallOutcome TestContext::RunCall(const std::string& target, const std::function<void()>& body)
{
	pending_detail_.clear();
	pending_failure_.clear();
	pending_suspect_.clear();
	in_call_ = true;

	// Without a guard the call can take the process down, so the target is
	// written to the log first: the last line in the file then names the
	// culprit even though the outcome never got written.
	if (!IsGuardAvailable())
	{
		LogToFile("  %-*s ... \n", kTargetColumn, target.c_str());
	}

	FaultInfo fault;
	const CallOutcome guard_result = InvokeGuarded(body, &fault);
	in_call_ = false;

	CallOutcome outcome = guard_result;
	std::string detail = pending_detail_;

	if (outcome == CallOutcome::kCrashed)
	{
		// The fault is the whole finding; anything Detail() managed to set
		// before the call died is at best half-written.
		detail = fault.description;
	}
	else if (outcome == CallOutcome::kOk && !pending_failure_.empty())
	{
		outcome = CallOutcome::kFailed;
		detail = pending_failure_ + (detail.empty() ? "" : " (" + detail + ")");
	}
	else if (outcome == CallOutcome::kOk && !pending_suspect_.empty())
	{
		outcome = CallOutcome::kSuspect;
		detail = pending_suspect_ + (detail.empty() ? "" : " (" + detail + ")");
	}

	Record(target, outcome, detail);
	return outcome;
}

void TestContext::SkipCall(const std::string& target, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	const std::string reason = FormatToString(format, args);
	va_end(args);

	Record(target, CallOutcome::kSkipped, reason);
}

void TestContext::UnavailableCall(const std::string& target, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	const std::string reason = FormatToString(format, args);
	va_end(args);

	Record(target, CallOutcome::kUnavailable, reason);
}

void TestContext::Record(const std::string& target, CallOutcome outcome, const std::string& detail)
{
	tally_.Count(outcome);

	// Only the two failure outcomes are coloured. Colouring every row would
	// make none of them stand out, and the job here is spotting the three bad
	// lines among two hundred good ones.
	LogColor color = LogColor::kNone;
	if (outcome == CallOutcome::kCrashed)
	{
		color = LogColor::kBrightRed;
	}
	else if (outcome == CallOutcome::kFailed)
	{
		color = LogColor::kRed;
	}
	else if (outcome == CallOutcome::kSuspect)
	{
		color = LogColor::kYellow;
	}

	LogRawColored(color, "  %-*s %-8s %s\n",
		kTargetColumn,
		FitTarget(target).c_str(),
		CallOutcomeName(outcome),
		detail.c_str());
}

} // namespace cs2st
