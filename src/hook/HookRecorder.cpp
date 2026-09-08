#include "hook/HookRecorder.h"

#include <cstdarg>
#include <cstdio>

#include "core/Log.h"

namespace cs2st
{

HookRecorder& HookRecorder::Get()
{
	static HookRecorder instance;
	return instance;
}

HookRecorder::Entry* HookRecorder::FindOrCreate(const char* id)
{
	for (Entry& entry : entries_)
	{
		// std::string == const char* compares without building a temporary.
		if (entry.id == id)
		{
			return &entry;
		}
	}

	Entry entry;
	entry.id = id;
	entries_.push_back(entry);
	return &entries_.back();
}

void HookRecorder::Declare(const char* id, const char* owner)
{
	Entry* entry = FindOrCreate(id);
	entry->owner = owner;
}

void HookRecorder::Enter(const char* id)
{
	// Runs in the game's own callback, potentially every tick, so this stays a
	// linear scan and an increment: no allocation on the hot path.
	Entry* entry = FindOrCreate(id);
	++entry->enters;

	if (entry->traced_enters < kMaxTraced)
	{
		++entry->traced_enters;
		// File only - the console would scroll away everything else. The log
		// is flushed per line, so this survives a fault inside the call.
		LogToFile("  [trace] enter %s\n", id);
	}
}

void HookRecorder::EnterWithArgs(const char* id, const char* format, ...)
{
	Entry* entry = FindOrCreate(id);
	++entry->enters;

	const bool want_sample = entry->samples.size() < kMaxSamples;
	const bool want_trace = entry->traced_enters < kMaxTraced;

	// Bail before formatting, not after: once both quotas are full this runs
	// every tick and must not do any work it does not have to.
	if (!want_sample && !want_trace)
	{
		return;
	}

	char buffer[512];
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (written < 0)
	{
		return;
	}

	if (want_sample)
	{
		entry->samples.emplace_back(buffer);
	}
	if (want_trace)
	{
		++entry->traced_enters;
		LogToFile("  [trace] enter %s(%s)\n", id, buffer);
	}
}

void HookRecorder::Leave(const char* id)
{
	Entry* entry = FindOrCreate(id);
	++entry->leaves;

	if (entry->traced_leaves < kMaxTraced)
	{
		++entry->traced_leaves;
		LogToFile("  [trace] leave %s\n", id);
	}
}

void HookRecorder::ResetCounts()
{
	for (Entry& entry : entries_)
	{
		entry.enters = 0;
		entry.leaves = 0;
		entry.samples.clear();
		entry.traced_enters = 0;
		entry.traced_leaves = 0;
	}
}

void HookRecorder::Clear()
{
	entries_.clear();
}

std::size_t HookRecorder::fired() const
{
	std::size_t count = 0;
	for (const Entry& entry : entries_)
	{
		if (entry.enters > 0)
		{
			++count;
		}
	}
	return count;
}

std::size_t HookRecorder::unbalanced() const
{
	std::size_t count = 0;
	for (const Entry& entry : entries_)
	{
		if (entry.enters != entry.leaves)
		{
			++count;
		}
	}
	return count;
}

void HookRecorder::Dump() const
{
	if (entries_.empty())
	{
		LogRaw("  no hooks installed\n");
		return;
	}

	LogRaw("  %-52s %9s %9s  %s\n", "HOOK", "ENTER", "LEAVE", "FIRST ARGUMENTS");

	for (const Entry& entry : entries_)
	{
		if (entry.enters == 0)
		{
			// Spelled out rather than left as "0": a hook that never fired is
			// the single most informative row in this table after an update.
			LogRawColored(LogColor::kYellow, "  %-52s %9s %9s  %s\n",
				entry.id.c_str(), "never", "-", "");
			continue;
		}

		// Entered more often than it returned: the original function did not
		// come back. Either it faulted, or another plugin superseded the call
		// so the post-hook chain never ran.
		const bool balanced = entry.enters == entry.leaves;

		LogRawColored(balanced ? LogColor::kNone : LogColor::kRed,
			"  %-52s %9llu %9llu  %s\n",
			entry.id.c_str(),
			static_cast<unsigned long long>(entry.enters),
			static_cast<unsigned long long>(entry.leaves),
			entry.samples.empty() ? "" : entry.samples.front().c_str());

		for (std::size_t i = 1; i < entry.samples.size(); ++i)
		{
			LogRaw("  %-52s %9s %9s  %s\n", "", "", "", entry.samples[i].c_str());
		}
	}
}

} // namespace cs2st
