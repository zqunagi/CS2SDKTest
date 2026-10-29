#include "suites/GameEntitiesSuite.h"

#include <appframework/iappsystem.h>
#include <engine2/isource2server.h>
#include <tier0/bitvec.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "testkit/UtlVectorGuard.h"

// CCheckTransmitInfo is not given a StructInspector here: the engine builds it
// per client and only passes it into CheckTransmit / ShouldClientReceive...,
// so no interface hands one out for us to read. See NoteSideEffectingCalls.

namespace cs2st
{
namespace
{

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

// The transmit sets CheckTransmit writes into. 16384 bits each, so too large
// for comfort on the guarded stack; zero-initialised static storage is also
// what a "nothing transmitted yet" input should look like.
CBitVec<16384> g_union_transmit;
CBitVec<16384> g_scratch_transmit;

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool GameEntitiesSuite::Acquire(TestContext& context)
{
	entities_ = static_cast<ISource2GameEntities*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEENTS));

	if (entities_ == nullptr)
	{
		const Module& server_module = context.GetModule("server");
		if (!server_module.IsValid())
		{
			context.UnavailableCall("ISource2GameEntities", "the server module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISource2GameEntities",
				"server is loaded but does not answer '%s'",
				INTERFACEVERSION_SERVERGAMEENTS);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(entities_),
		INTERFACEVERSION_SERVERGAMEENTS);
	return true;
}

void GameEntitiesSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseEntityQueries(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void GameEntitiesSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = entities_->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		context.Detail("-> %s", entities_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = entities_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = entities_->GetDependencies();
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
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface (unknown name)", {
		void* none = entities_->QueryInterface("CS2SDKTestNoSuchInterface001");
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
//  The interface's own slots that can be called with arguments we can build
// -----------------------------------------------------------------------------

void GameEntitiesSuite::ExerciseEntityQueries(TestContext& context)
{
	context.Note("[ISource2GameEntities] the destructor takes two slots before these; "
	             "GetAlternateBaselineName is the probe (read-only, const)");

	// Entity index 0 is the world. With no map loaded the game may have no
	// entity system to ask, so a CRASHED here on an idle server says "no entity
	// system", which the GameEntitySystemSuite rows beside it will confirm.
	CS2ST_CALL(context, "ISource2GameEntities::GetAlternateBaselineName [entity 0]", {
		const char* name = nullptr;
		const bool found = entities_->GetAlternateBaselineName(CEntityIndex(0), &name);
		context.Detail("-> %s, name \"%s\"", found ? "true" : "false",
			name != nullptr ? name : "(null)");
		if (found && name == nullptr)
		{
			context.Suspect("returned true but wrote no name");
		}
	});

	CS2ST_CALL(context, "ISource2GameEntities::GetAlternateBaselineName [entity 1]", {
		const char* name = nullptr;
		const bool found = entities_->GetAlternateBaselineName(CEntityIndex(1), &name);
		context.Detail("-> %s, name \"%s\"", found ? "true" : "false",
			name != nullptr ? name : "(null)");
		if (found && name == nullptr)
		{
			context.Suspect("returned true but wrote no name");
		}
	});

	// Empty list in: nothing to pack, so nothing for the callee to change.
	// Empty, so the vector never allocates; the guard is for the Windows heap
	// rule if a later edit adds elements.
	CS2ST_CALL(context, "ISource2GameEntities::OnPrePackEntities [empty list]", {
		CUtlVector<Entity2Networkable_t*> ents;
		UtlVectorGuard<CUtlVector<Entity2Networkable_t*>> guard(ents);
		entities_->OnPrePackEntities(ents);
		context.Detail("-> returned (void), list still %d entries", ents.Count());
		if (ents.Count() != 0)
		{
			context.Suspect("a const callee grew the list it was given");
		}
	});

	// Zero clients, zero entities: every loop in the callee has no iterations,
	// so what is left is the slot and the argument layout (the bit-vector
	// references and the three pointers). The bit vectors are ours.
	CS2ST_CALL(context, "ISource2GameEntities::CheckTransmit [0 clients, 0 entities]", {
		g_union_transmit.ClearAll();
		g_scratch_transmit.ClearAll();
		entities_->CheckTransmit(nullptr, 0, g_union_transmit, g_scratch_transmit,
			nullptr, nullptr, 0);
		const bool touched = g_union_transmit.FindNextSetBit(0) >= 0;
		context.Detail("-> returned (void), union set %s",
			touched ? "has bits" : "still empty");
		if (touched)
		{
			context.Suspect("an empty entity list marked entities for transmission");
		}
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void GameEntitiesSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISource2GameEntities] slots not called");

	context.SkipCall("ISource2GameEntities::~ISource2GameEntities",
		"destroys the game-owned interface; the destructor may take two vtable slots");
	context.SkipCall("ISource2GameEntities::PrepareForFullUpdate",
		"drops a client slot's PVS state, so that client's next update is a full one");
	context.SkipCall("ISource2GameEntities::ShouldClientReceiveStringTableUserData",
		"reads a CCheckTransmitInfo only the engine can fill in (its bit-vector "
		"pointers and visibility info); a fake would make any crash ours");
	context.SkipCall("ISource2GameEntities::ResetChangeAccessorsSerialNumbersToZero",
		"resets the change-tracking serial numbers of live entities, which "
		"forces every client to be resent them");
	context.SkipCall("CCheckTransmitInfo layout",
		"no interface hands one out; the engine builds it per client and only "
		"passes it into CheckTransmit");
}

} // namespace cs2st
