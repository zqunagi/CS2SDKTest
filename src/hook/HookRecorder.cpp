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

HookRecorder::Entry* HookRecorder::FindOrCreate(const std::string& id)
{
	for (Entry& entry : entries_)
	{
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

void HookRecorder::Declare(const std::string& id, const std::string& owner)
{
	Entry* entry = FindOrCreate(id);
	entry->owner = owner;
}

void HookRecorder::Fire(const std::string& id)
{
	// Runs in the game's own callback, potentially every tick, so this stays a
	// linear scan and an increment: no allocation on the hot path once the
	// entry exists and the samples are full.
	Entry* entry = FindOrCreate(id);
	++entry->count;
}

void HookRecorder::FireWithArgs(const std::string& id, const char* format, ...)
{
	Entry* entry = FindOrCreate(id);
	++entry->count;

	if (entry->samples.size() >= kMaxSamples)
	{
		return;
	}

	char buffer[512];
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (written >= 0)
	{
		entry->samples.emplace_back(buffer);
	}
}

void HookRecorder::ResetCounts()
{
	for (Entry& entry : entries_)
	{
		entry.count = 0;
		entry.samples.clear();
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
		if (entry.count > 0)
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

	LogRaw("  %-52s %10s  %s\n", "HOOK", "FIRED", "FIRST ARGUMENTS");
	for (const Entry& entry : entries_)
	{
		if (entry.count == 0)
		{
			// Spelled out rather than left as "0": a hook that never fired is
			// the single most informative row in this table after an update.
			LogRaw("  %-52s %10s  %s\n", entry.id.c_str(), "never", "");
			continue;
		}

		LogRaw("  %-52s %10llu  %s\n",
			entry.id.c_str(),
			static_cast<unsigned long long>(entry.count),
			entry.samples.empty() ? "" : entry.samples.front().c_str());

		for (std::size_t i = 1; i < entry.samples.size(); ++i)
		{
			LogRaw("  %-52s %10s  %s\n", "", "", entry.samples[i].c_str());
		}
	}
}

} // namespace cs2st
