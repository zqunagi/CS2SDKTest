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

// Width of the target column in the log. Interface::Method names run long, so
// this is generous; anything past it wraps rather than being cut, because a
// truncated symbol name is worse than a ragged column.
constexpr int kTargetColumn = 52;

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

CallOutcome TestContext::RunCall(const std::string& target, const std::function<void()>& body)
{
	pending_detail_.clear();
	pending_failure_.clear();
	in_call_ = true;

	// The line is written *before* the call in the unguarded case, so that if
	// the call takes the process down the log already names the culprit. It is
	// then completed with the outcome once the call returns.
	if (!IsGuardAvailable())
	{
		LogToFile("  %-*s ... \n", kTargetColumn, target.c_str());
	}

	const CallOutcome guard_result = InvokeGuarded(body);
	in_call_ = false;

	CallOutcome outcome = guard_result;
	std::string detail = pending_detail_;

	if (outcome == CallOutcome::kOk && !pending_failure_.empty())
	{
		outcome = CallOutcome::kFailed;
		detail = pending_failure_ + (detail.empty() ? "" : " (" + detail + ")");
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

	LogRaw("  %-*s %-8s %s\n",
		kTargetColumn,
		target.c_str(),
		CallOutcomeName(outcome),
		detail.c_str());
}

} // namespace cs2st
