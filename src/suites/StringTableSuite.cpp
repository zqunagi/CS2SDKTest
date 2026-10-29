#include "suites/StringTableSuite.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <appframework/iappsystem.h>
#include <networkstringtabledefs.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

// Bounds for everything that walks something the engine counts: a garbage
// count must end the loop, not the process.
constexpr int kMaxTablesListed = MAX_TABLES;
constexpr int kMaxStringsSampled = 4;

const char* TierName(AppSystemTier_t tier)
{
	switch (tier)
	{
		case APP_SYSTEM_TIER0:      return "TIER0";
		case APP_SYSTEM_TIER1:      return "TIER1";
		case APP_SYSTEM_TIER2:      return "TIER2";
		case APP_SYSTEM_TIER3:      return "TIER3";
		case APP_SYSTEM_TIER4:      return "TIER4";
		case APP_SYSTEM_TIER_OTHER: return "TIER_OTHER";
		default:                    return "<out of range>";
	}
}

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

// One row label per table / string. Built outside CS2ST_CALL: the macro takes
// two arguments and a format call with commas in its target would split.
template <std::size_t N>
void TableLabel(char (&label)[N], const char* method, int table)
{
	std::snprintf(label, N, "INetworkStringTable::%s [table %d]", method, table);
}

template <std::size_t N>
void StringLabel(char (&label)[N], const char* method, int table, int string)
{
	std::snprintf(label, N, "INetworkStringTable::%s [table %d, string %d]", method, table, string);
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool StringTableSuite::Acquire(TestContext& context)
{
	container_ = static_cast<INetworkStringTableContainer*>(
		context.QueryInterface("engine2", INTERFACENAME_NETWORKSTRINGTABLESERVER));

	if (container_ == nullptr)
	{
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("INetworkStringTableContainer",
				"the engine2 module is not loaded");
		}
		else
		{
			context.UnavailableCall("INetworkStringTableContainer",
				"engine2 is loaded but does not answer '%s'",
				INTERFACENAME_NETWORKSTRINGTABLESERVER);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(container_),
		INTERFACENAME_NETWORKSTRINGTABLESERVER);
	return true;
}

void StringTableSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseContainer(context);
	NoteSideEffectingCalls(context);
	InspectUserData(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void StringTableSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = container_->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		context.Detail("-> %s", container_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = container_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = container_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}

		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
		for (int i = 0; i < count && i < 16; ++i)
		{
			context.Detail("   [%d] module \"%s\", interface \"%s\"", i,
				deps[i].m_pModuleName, Str(deps[i].m_pInterfaceName));
		}
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface (unknown name)", {
		void* none = container_->QueryInterface("CS2SDKTestNoSuchInterface001");
		context.Detail("-> %p", none);
		if (none != nullptr)
		{
			context.Suspect("an interface name nobody registered resolved to a pointer");
		}
	});

	context.SkipCall("IAppSystem::Connect", "would re-run engine startup");
	context.SkipCall("IAppSystem::Init", "would re-run engine startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the engine down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  The container and its tables
// -----------------------------------------------------------------------------

void StringTableSuite::ExerciseContainer(TestContext& context)
{
	context.Note("[INetworkStringTableContainer] table lookups");

	CS2ST_CALL(context, "INetworkStringTableContainer::GetName", {
		context.Detail("-> \"%s\"", Str(container_->GetName()));
	});

	int table_count = 0;
	CS2ST_CALL(context, "INetworkStringTableContainer::GetNumTables", {
		table_count = container_->GetNumTables();
		context.Detail("-> %d", table_count);
		if (table_count < 0)
		{
			context.Fail("a table count cannot be negative");
		}
		else if (table_count > MAX_TABLES)
		{
			context.Fail("%d tables, but the table id is sent in a field that holds %d",
				table_count, MAX_TABLES);
		}
	});

	CS2ST_CALL(context, "INetworkStringTableContainer::FindTable (unknown name)", {
		INetworkStringTable* none = container_->FindTable("cs2sdktest_no_such_table");
		context.Detail("-> %p", static_cast<void*>(none));
		if (none != nullptr)
		{
			context.Suspect("a table name nobody created resolved to a table");
		}
	});

	if (table_count <= 0)
	{
		context.Note("no tables exist (no game server running); per-table rows have nothing to read");
		return;
	}

	const int listed = table_count < kMaxTablesListed ? table_count : kMaxTablesListed;
	for (int i = 0; i < listed; ++i)
	{
		INetworkStringTable* table = nullptr;
		char label[160];
		std::snprintf(label, sizeof(label),
			"INetworkStringTableContainer::GetTable [table %d]", i);
		CS2ST_CALL(context, label, {
			table = container_->GetTable(i);
			context.Detail("-> %p", static_cast<void*>(table));
			if (table == nullptr)
			{
				context.Suspect("GetNumTables is %d but GetTable(%d) is null", table_count, i);
			}
		});

		if (table != nullptr)
		{
			ExerciseTable(context, table, i);
		}
	}
}

void StringTableSuite::ExerciseTable(TestContext& context, INetworkStringTable* table, int index)
{
	char label[160];

	const char* name = nullptr;
	TableLabel(label, "GetTableName", index);
	CS2ST_CALL(context, label, {
		name = table->GetTableName();
		context.Detail("-> \"%s\"", Str(name));
		if (name == nullptr)
		{
			context.Fail("a table the container returned reports no name");
		}
	});

	TableLabel(label, "GetTableId", index);
	CS2ST_CALL(context, label, {
		const TABLEID id = table->GetTableId();
		context.Detail("-> %d", static_cast<int>(id));
		if (id != index)
		{
			context.Fail("GetTable(%d) returned a table whose id is %d", index, static_cast<int>(id));
		}
	});

	if (name != nullptr)
	{
		TableLabel(label, "FindTable round trip (container)", index);
		CS2ST_CALL(context, label, {
			INetworkStringTable* found = container_->FindTable(name);
			context.Detail("FindTable(\"%s\") -> %p", name, static_cast<void*>(found));
			if (found != table)
			{
				context.Fail("GetTable(%d) is %p but FindTable(\"%s\") is %p",
					index, static_cast<void*>(table), name, static_cast<void*>(found));
			}
		});
	}

	int num_strings = 0;
	TableLabel(label, "GetNumStrings", index);
	CS2ST_CALL(context, label, {
		num_strings = table->GetNumStrings();
		context.Detail("-> %d", num_strings);
		if (num_strings < 0)
		{
			context.Fail("a string count cannot be negative");
		}
	});

	int tick = 0;
	TableLabel(label, "GetTick", index);
	CS2ST_CALL(context, label, {
		tick = table->GetTick();
		context.Detail("-> %d", tick);
	});

	TableLabel(label, "SetDebugLogging (false)", index);
	CS2ST_CALL(context, label, {
		// Logging is off unless something turned it on, so this leaves the
		// table as it was. There is no getter to read the effect back.
		table->SetDebugLogging(false);
		context.Detail("called");
	});

	TableLabel(label, "ChangedBetweenTicks", index);
	CS2ST_CALL(context, label, {
		// Read-only. An empty range cannot contain a change, the whole
		// range up to the table's own tick can.
		const bool same = table->ChangedBetweenTicks(tick, tick);
		const bool whole = table->ChangedBetweenTicks(0, tick);
		context.Detail("(%d, %d) -> %s, (0, %d) -> %s", tick, tick, same ? "true" : "false",
			tick, whole ? "true" : "false");
	});

	TableLabel(label, "FindStringIndex (unknown string)", index);
	CS2ST_CALL(context, label, {
		const int found = table->FindStringIndex("cs2sdktest_no_such_string");
		context.Detail("-> %d", found);
		if (static_cast<unsigned int>(found) != INVALID_STRING_INDEX)
		{
			context.Suspect("a string nobody added was found at index %d", found);
		}
	});

	if (num_strings <= 0)
	{
		return;
	}

	TableLabel(label, "GetString (past the end, fail silent)", index);
	CS2ST_CALL(context, label, {
		const char* none = table->GetString(num_strings, true);
		context.Detail("[%d] -> %s", num_strings, none != nullptr ? "non-null" : "null");
	});

	unsigned char copy[1024];
	const int sampled = num_strings < kMaxStringsSampled ? num_strings : kMaxStringsSampled;
	for (int i = 0; i < sampled; ++i)
	{
		const char* text = nullptr;
		StringLabel(label, "GetString", index, i);
		CS2ST_CALL(context, label, {
			text = table->GetString(i, true);
			context.Detail("-> \"%.63s\"", Str(text));
			if (text == nullptr)
			{
				context.Suspect("string %d of %d is null", i, num_strings);
			}
		});

		if (text != nullptr)
		{
			StringLabel(label, "FindStringIndex round trip", index, i);
			CS2ST_CALL(context, label, {
				const int found = table->FindStringIndex(text);
				context.Detail("-> %d", found);
				if (found != i)
				{
					context.Suspect("string %d found again at index %d (duplicate strings?)", i, found);
				}
			});
		}

		StringLabel(label, "GetStringUserData", index, i);
		CS2ST_CALL(context, label, {
			const StringUserData_t* data = table->GetStringUserData(i);
			if (data == nullptr)
			{
				context.Detail("-> none");
				return;
			}
			context.Detail("-> %p, size %u, data %p", static_cast<const void*>(data),
				data->m_cbDataSize, data->m_pRawData);
			if (data->m_cbDataSize > 0 && data->m_pRawData == nullptr)
			{
				context.Suspect("user data of %u bytes has no data pointer", data->m_cbDataSize);
			}
			if (user_data_ == nullptr)
			{
				user_data_ = data;
			}
		});

		// Writes back the bytes the string already has, so the table ends up
		// as it was. The first string with user data is left alone: its
		// StringUserData_t is read again by InspectUserData, and a write may
		// move it.
		StringLabel(label, "SetStringUserData (same bytes)", index, i);
		CS2ST_CALL(context, label, {
			const StringUserData_t* before = table->GetStringUserData(i);
			if (before == nullptr || before->m_cbDataSize == 0 || before->m_pRawData == nullptr)
			{
				context.SkipCall(label, "the string has no user data to write back");
				return;
			}
			if (before == user_data_)
			{
				context.SkipCall(label, "its StringUserData_t is read again by the layout inspection");
				return;
			}
			if (before->m_cbDataSize > sizeof(copy))
			{
				context.SkipCall(label, "more user data than the scratch buffer holds");
				return;
			}

			const unsigned int size = before->m_cbDataSize;
			std::memcpy(copy, before->m_pRawData, size);
			SetStringUserDataRequest_t request;
			request.m_pRawData = copy;
			request.m_cbDataSize = size;
			const bool set = table->SetStringUserData(i, &request, false);
			context.Detail("%u bytes -> %s", size, set ? "true" : "false");

			const StringUserData_t* after = table->GetStringUserData(i);
			if (after == nullptr || after->m_cbDataSize != size
				|| (after->m_pRawData != nullptr && std::memcmp(after->m_pRawData, copy, size) != 0))
			{
				context.Suspect("user data differs after writing back the bytes it already had");
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  Struct layout
// -----------------------------------------------------------------------------

void StringTableSuite::InspectUserData(TestContext& context)
{
	context.Note("[StringUserData_t] struct layout");

	if (user_data_ == nullptr)
	{
		context.SkipCall("StringUserData_t",
			"no reachable instance: no table had a string with user data");
		return;
	}

	StructInspector inspect(context, "StringUserData_t", user_data_, sizeof(StringUserData_t));
	inspect.UInt32 ("m_cbDataSize", offsetof(StringUserData_t, m_cbDataSize));
	inspect.Pointer("m_pRawData",   offsetof(StringUserData_t, m_pRawData));
	inspect.Summarize();
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  Calls that change what the clients are replicated
// -----------------------------------------------------------------------------

void StringTableSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[INetworkStringTableContainer / INetworkStringTable] mutating slots");

	context.SkipCall("INetworkStringTableContainer::CreateStringTable",
		"creates a table every client would be sent");
	context.SkipCall("INetworkStringTableContainer::~INetworkStringTableContainer",
		"destroys the engine-owned container; the destructor may take two vtable slots");
	context.SkipCall("INetworkStringTableContainer::RemoveAllTables",
		"destroys every table of the running server");

	context.SkipCall("INetworkStringTable::~INetworkStringTable",
		"destroys an engine-owned table; the destructor may take two vtable slots");
	context.SkipCall("INetworkStringTable::SetStringChangeTick",
		"changes the tick the table is replicated at; the second argument is unknown");
	context.SkipCall("INetworkStringTable::AddString",
		"adds a string that is replicated to clients");
	context.SkipCall("INetworkStringTable::SetStringChangedCallback",
		"needs a CUtlDelegate the engine can call; a bad one crashes the server on the next change");
	context.SkipCall("INetworkStringTable::SetAllowClientSideAddString",
		"changes whether clients may add strings");
	context.SkipCall("INetworkStringTable::SetDebugLoggingPrefix",
		"changes the engine's log prefix for the table");
	context.SkipCall("INetworkStringTable::SetUserDataFormatter",
		"needs a CUtlDelegate the engine can call; replaces the table's formatter");
}

} // namespace cs2st
