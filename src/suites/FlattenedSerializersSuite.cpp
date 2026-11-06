#include "suites/FlattenedSerializersSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <engine2/gameshareddefs.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/ivengineserver2.h>
#include <engine2/networkgameserverbase.h>
#include <interfaces/interfaces.h>
#include <networksystem/iflattenedserializers.h>
#include <networksystem/serializedentity.h>
#include <tier0/bitbuf.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>
#include <tier2/fieldpathpacker.h>

#include "core/Log.h"
#include "core/Platform.h"
#include "testkit/BaselineLookup.h"
#include "testkit/UtlVectorGuard.h"

namespace cs2st
{
namespace
{

constexpr int kMaxListed = 6;
constexpr int kMaxPaths = 1 << 16;
constexpr NetworkSerializationMode_t kMode = NET_SERIALIZATION_MODE_DEFAULT;
constexpr int kEntityIndex = 0;

typedef CUtlLeanVectorFixedGrowable<uint32> PathVec_t;
typedef CUtlVector<CUtlString> StringVec_t;

// A path vector that outgrows its fixed buffer is heap allocated through the
// tier0 allocator; on Windows its destructor would free that with the plugin's
// CRT, so the holder lets go of it instead. A few KB leaked per call, as
// UtlVectorGuard does for CUtlVector.
class PathVecHolder
{
public:
	PathVecHolder() : vec_(new PathVec_t()) {}
	~PathVecHolder()
	{
#if CS2ST_WINDOWS
		(void)vec_.release();
#endif
	}
	PathVecHolder(const PathVecHolder&) = delete;
	PathVecHolder& operator=(const PathVecHolder&) = delete;

	PathVec_t& Get() { return *vec_; }

private:
	std::unique_ptr<PathVec_t> vec_;
};

const char* Str(const char* text)
{
	return text != nullptr ? text : "<null>";
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool FlattenedSerializersSuite::Acquire(TestContext& context)
{
	serializers_ = static_cast<IFlattenedSerializers*>(
		context.QueryInterface("networksystem", FLATTENEDSERIALIZERS_INTERFACE_VERSION));

	if (serializers_ == nullptr)
	{
		const Module& network_module = context.GetModule("networksystem");
		if (!network_module.IsValid())
		{
			context.UnavailableCall("IFlattenedSerializers", "the networksystem module is not loaded");
		}
		else
		{
			context.UnavailableCall("IFlattenedSerializers",
				"networksystem is loaded but does not answer '%s'",
				FLATTENEDSERIALIZERS_INTERFACE_VERSION);
		}
		return false;
	}

	entities_ = static_cast<ISerializedEntities*>(
		context.QueryInterface("networksystem", SERIALIZEDENTITIES_INTERFACE_VERSION));
	service_ = static_cast<INetworkServerService*>(
		context.QueryInterface("engine2", NETWORKSERVERSERVICE_INTERFACE_VERSION));

	context.Note("interface at %p (%s @ networksystem)", static_cast<void*>(serializers_),
		FLATTENEDSERIALIZERS_INTERFACE_VERSION);
	return true;
}

void FlattenedSerializersSuite::ExerciseCalls(TestContext& context)
{
	context.Note("[IFlattenedSerializers] not an IAppSystem: slot 0 is FindOrCreateCreateFlattenedSerializers");

	const CSerializedEntity* baseline = FindInstanceBaseline(context, service_);
	if (baseline == nullptr)
	{
		context.UnavailableCall("IFlattenedSerializers (serializer calls)",
			"no instance baseline to take a serializer from; needs a loaded level");
	}
	else
	{
		ExerciseSerializer(context, baseline);
		ExerciseEntityWork(context, baseline);
	}

	NoteSkippedCalls(context);
}

// -----------------------------------------------------------------------------
//  Calls that only need the serializer
// -----------------------------------------------------------------------------

void FlattenedSerializersSuite::ExerciseSerializer(TestContext& context, const CSerializedEntity* entity)
{
	const FlattenedSerializerDesc_t desc = entity->m_AssociatedSerializer;
	context.Note("[IFlattenedSerializers] serializer \"%s\" taken from the baseline at %p",
		Str(desc.m_name.String()), static_cast<const void*>(entity));

	// The first field the baseline carries, as a packed and as an unpacked path.
	PackedFieldPath_t first_packed = 0;
	bool have_first = false;
	if (entity->m_pFieldPaths != nullptr && entity->m_pFieldPaths->Count() > 0)
	{
		first_packed = entity->m_pFieldPaths->Element(0);
		have_first = true;
	}
	const CFieldPathPacker* packer = entities_ != nullptr ? entities_->GetFieldPathPacker() : nullptr;
	CFieldPath first_path;
	first_path.m_nCount = 0;
	first_path.m_bUsingExternalPaths = false;
	if (have_first && packer != nullptr)
	{
		first_path = packer->UnpackPath(first_packed);
	}

	// ---- names --------------------------------------------------------------

	CUtlString leaf_name;
	if (!have_first || packer == nullptr || first_path.GetDepth() <= 0)
	{
		context.UnavailableCall("IFlattenedSerializers::GetFieldName",
			"no packed path to name (the baseline has no fields, or the packer is missing)");
	}
	else
	{
		CS2ST_CALL(context, "IFlattenedSerializers::GetFieldName (CFieldPath)", {
			CUtlString out;
			const char* name = serializers_->GetFieldName(desc, first_path, nullptr, kMode, &out);
			context.Detail("-> \"%s\" for packed 0x%08X", Str(name), first_packed);
			if (name == nullptr || name[0] == '\0')
			{
				context.Suspect("the first field of a baseline has no name");
			}
			else
			{
				leaf_name = name;
			}
		});

		// ABI note: the two overloads are adjacent in the SDK. Under the Itanium
		// ABI that is declaration order; MSVC lays overloads out in reverse, so
		// on Windows these two rows swap and the oracle below shows it.
		CS2ST_CALL(context, "IFlattenedSerializers::GetFieldName (packed int)", {
			CUtlString out;
			const char* name = serializers_->GetFieldName(desc, static_cast<int>(first_packed), nullptr, kMode, &out);
			context.Detail("-> \"%s\"", Str(name));
			if (!leaf_name.IsEmpty() && (name == nullptr || std::strcmp(name, leaf_name.Get()) != 0))
			{
				context.Fail("the int overload names 0x%08X \"%s\", the CFieldPath one \"%s\"", first_packed,
					Str(name), leaf_name.Get());
			}
		});

		CS2ST_CALL(context, "IFlattenedSerializers::GetFullFieldName", {
			// pObject is only needed to resolve polymorphic fields; null is the
			// non-polymorphic case.
			const CUtlString full = serializers_->GetFullFieldName(desc, first_path, nullptr, kEntityIndex, kMode, true);
			context.Detail("-> \"%s\"", Str(full.Get()));
			if (full.IsEmpty())
			{
				context.Suspect("the full name of an existing field is empty");
			}
		});

		CS2ST_CALL(context, "IFlattenedSerializers::GetFieldPathChildIndices", {
			CUtlVector<int> children;
			UtlVectorGuard<CUtlVector<int>> guard(children);
			const bool ok = serializers_->GetFieldPathChildIndices(desc, nullptr, kMode, first_path, &children);
			context.Detail("-> %s, %d index(es)", ok ? "true" : "false", children.Count());
			if (ok && children.Count() < first_path.GetDepth())
			{
				context.Suspect("%d index(es) for a path of depth %d", children.Count(), first_path.GetDepth());
			}
		});

		CS2ST_CALL(context, "IFlattenedSerializers::LookupOffsetsToIgnoreForPath", {
			const OffsetIgnore_t* ignore = serializers_->LookupOffsetsToIgnoreForPath(desc, first_path, nullptr, kMode);
			context.Detail("-> %p", static_cast<const void*>(ignore));
		});
	}

	// ---- field queries by name ----------------------------------------------

	if (leaf_name.IsEmpty())
	{
		context.UnavailableCall("IFlattenedSerializers::IsFieldNetworked (known field)",
			"no field name to look up");
	}
	else
	{
		CS2ST_CALL(context, "IFlattenedSerializers::IsFieldNetworked (a field the baseline carries)", {
			const bool networked = serializers_->IsFieldNetworked(desc, leaf_name.Get());
			context.Detail("-> %s for \"%s\"", networked ? "true" : "false", leaf_name.Get());
			if (!networked)
			{
				context.Suspect("\"%s\" is in the baseline but not reported as networked", leaf_name.Get());
			}
		});

		CS2ST_CALL(context, "IFlattenedSerializers::IsUnflattenedField (a field the baseline carries)", {
			const bool unflattened = serializers_->IsUnflattenedField(desc, leaf_name.Get());
			context.Detail("-> %s for \"%s\"", unflattened ? "true" : "false", leaf_name.Get());
		});
	}

	CS2ST_CALL(context, "IFlattenedSerializers::IsFieldNetworked (no such field)", {
		const bool networked = serializers_->IsFieldNetworked(desc, "CS2SDKTestNoSuchField");
		context.Detail("-> %s", networked ? "true" : "false");
		if (networked)
		{
			context.Suspect("a field nobody declared is reported as networked");
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::IsUnflattenedField (no such field)", {
		const bool unflattened = serializers_->IsUnflattenedField(desc, "CS2SDKTestNoSuchField");
		context.Detail("-> %s", unflattened ? "true" : "false");
		if (unflattened)
		{
			context.Suspect("a field nobody declared is reported as unflattened");
		}
	});

	// ---- serializer properties ----------------------------------------------

	CS2ST_CALL(context, "IFlattenedSerializers::HasPolymorphicFields", {
		context.Detail("-> %s", serializers_->HasPolymorphicFields(desc) ? "true" : "false");
	});

	CS2ST_CALL(context, "IFlattenedSerializers::UserGroupHasFields (no such group)", {
		const bool has = serializers_->UserGroupHasFields(desc, "CS2SDKTestNoSuchUserGroup");
		context.Detail("-> %s", has ? "true" : "false");
		if (has)
		{
			context.Suspect("a user group nobody declared has fields");
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::GetOutOfPVSUpdateFields", {
		CSendProxyDataList* opt_in = nullptr;
		CSendProxyDataList* opt_out = nullptr;
		const bool ok = serializers_->GetOutOfPVSUpdateFields(desc, &opt_in, &opt_out);
		context.Detail("-> %s, opt in %p (%d), opt out %p (%d)", ok ? "true" : "false",
			static_cast<void*>(opt_in), opt_in != nullptr ? opt_in->m_Data.Count() : -1,
			static_cast<void*>(opt_out), opt_out != nullptr ? opt_out->m_Data.Count() : -1);
	});

	CS2ST_CALL(context, "IFlattenedSerializers::GetPolymorphicMetadataDescription (empty metadata)", {
		PolymorphicMetadata_t none;
		none.m_nCount = 0;
		none.m_pEntries = nullptr;
		none.m_bNeedsSort = false;
		StringVec_t lines;
		UtlVectorGuard<StringVec_t> guard(lines);
		serializers_->GetPolymorphicMetadataDescription(desc, &none, nullptr, &lines);
		context.Detail("-> %d line(s)", lines.Count());
		if (lines.Count() != 0)
		{
			context.Suspect("%d line(s) described for no metadata at all", lines.Count());
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::PurgeTemporaryData", {
		// Drops caches the serializers rebuild on use; no state the game keeps.
		serializers_->PurgeTemporaryData();
		context.Detail("-> returned");
	});
}

// -----------------------------------------------------------------------------
//  Calls that work on the baseline entity
// -----------------------------------------------------------------------------

void FlattenedSerializersSuite::ExerciseEntityWork(TestContext& context, const CSerializedEntity* entity)
{
	const FlattenedSerializerDesc_t desc = entity->m_AssociatedSerializer;
	context.Note("[IFlattenedSerializers] work on the baseline entity (read-only on it)");

	CS2ST_CALL(context, "IFlattenedSerializers::ValidateSerializedEntity", {
		const bool ok = serializers_->ValidateSerializedEntity(desc, entity, kEntityIndex, kMode);
		context.Detail("-> %s", ok ? "true" : "false");
		if (!ok)
		{
			context.Suspect("the engine's own baseline does not validate against its serializer");
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::DumpSerializedEntityToLines", {
		StringVec_t lines;
		UtlVectorGuard<StringVec_t> guard(lines);
		serializers_->DumpSerializedEntityToLines(desc, "CS2SDKTest", entity, kEntityIndex, kMode, &lines);
		context.Detail("-> %d line(s) for %d field(s)", lines.Count(), entity->m_nFieldCount);
		for (int i = 0; i < lines.Count() && i < kMaxListed; ++i)
		{
			context.Detail("   %.100s", Str(lines[i].Get()));
		}
		if (lines.Count() == 0)
		{
			context.Suspect("a baseline with %d field(s) dumped no lines", entity->m_nFieldCount);
		}
	});

	// ---- field path vectors ---------------------------------------------------

	if (entities_ == nullptr)
	{
		context.UnavailableCall("IFlattenedSerializers (delta calls)",
			"ISerializedEntities is not available to fill the path vector");
		return;
	}

	PathVecHolder all_paths_holder;
	PathVec_t& all_paths = all_paths_holder.Get();
	CS2ST_CALL(context, "ISerializedEntities::GetSerializedEntityFieldCount (with output vector)", {
		const int reported = entities_->GetSerializedEntityFieldCount(entity, &all_paths);
		context.Detail("-> %d, vector holds %d", reported, all_paths.Count());
		if (all_paths.Count() < 0 || all_paths.Count() > kMaxPaths)
		{
			context.Suspect("vector count %d is not plausible", all_paths.Count());
		}
		else if (all_paths.Count() != reported)
		{
			context.Fail("returned %d, but appended %d path(s)", reported, all_paths.Count());
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::CalcDelta (entity against itself)", {
		PathVecHolder changed_holder;
		PathVec_t& changed = changed_holder.Get();
		const int result = serializers_->CalcDelta(desc, entity, entity, &changed, kEntityIndex, kMode, nullptr);
		context.Detail("-> %d, vector holds %d", result, changed.Count());
		if (changed.Count() != 0)
		{
			context.Suspect("an entity compared with itself changed %d field(s)", changed.Count());
		}
		else if (result != 0)
		{
			context.Suspect("returned %d with no changed field", result);
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::RemoveArrayElementsOutsideOfArrayMetadataBounds", {
		if (all_paths.Count() <= 0 || all_paths.Count() > kMaxPaths)
		{
			context.SkipCall("IFlattenedSerializers::RemoveArrayElementsOutsideOfArrayMetadataBounds",
				"no field path vector from the call above");
			return;
		}
		const int before = all_paths.Count();
		const int removed = serializers_->RemoveArrayElementsOutsideOfArrayMetadataBounds(
			desc, entity, &all_paths, kEntityIndex, kMode);
		context.Detail("-> %d removed, vector %d -> %d", removed, before, all_paths.Count());
		if (removed != before - all_paths.Count())
		{
			context.Suspect("returned %d but the vector shrank by %d", removed, before - all_paths.Count());
		}
		if (removed != 0)
		{
			context.Suspect("the engine's own baseline has %d element(s) outside its array bounds", removed);
		}
	});

	CS2ST_CALL(context, "IFlattenedSerializers::MergeDeltas (entity + entity into a new one)", {
		CSerializedEntity* merged = entities_->AllocateSerializedEntity(desc, false, 0);
		if (merged == nullptr)
		{
			context.SkipCall("IFlattenedSerializers::MergeDeltas (entity + entity into a new one)",
				"could not allocate the output entity");
			return;
		}
		PathVecHolder merged_paths_holder;
		PathVec_t& merged_paths = merged_paths_holder.Get();
		const bool ok = serializers_->MergeDeltas(desc, entity, entity, merged, kEntityIndex, kMode, &merged_paths);
		context.Detail("-> %s, %d field(s), %d changed path(s)", ok ? "true" : "false", merged->m_nFieldCount,
			merged_paths.Count());
		if (ok && !entities_->AreSerializedEntitiesEqual(entity, merged))
		{
			context.Suspect("merging an entity with itself gave an entity that is not equal to it");
		}
		entities_->ReleaseSerializedEntity(merged);
	});

#if defined(CS2SDKTEST_WITH_BITBUF)
	CS2ST_CALL(context, "IFlattenedSerializers::WriteFieldList + ReadFields (round trip)", {
		std::vector<unsigned char> buffer(1 << 16);
		bf_write writer("CS2SDKTest", buffer.data(), static_cast<int>(buffer.size()));
		const bool wrote = serializers_->WriteFieldList(desc, &writer, entity, kEntityIndex, kMode, nullptr, CPlayerSlot(0));
		context.Detail("-> wrote %s, %d bit(s)%s", wrote ? "true" : "false", writer.GetNumBitsWritten(),
			writer.IsOverflowed() ? ", overflowed" : "");
		if (!wrote || writer.IsOverflowed() || writer.GetNumBitsWritten() <= 0)
		{
			context.Suspect("the baseline did not write out with no field list");
			return;
		}

		CSerializedEntity* back = entities_->AllocateSerializedEntity(desc, false, 0);
		if (back == nullptr)
		{
			context.SkipCall("IFlattenedSerializers::WriteFieldList + ReadFields (round trip)",
				"could not allocate the entity to read into");
			return;
		}
		bf_read reader("CS2SDKTest", buffer.data(), static_cast<int>(buffer.size()), writer.GetNumBitsWritten());
		const bool read = serializers_->ReadFields(desc, &reader, back, kEntityIndex, kMode, 0);
		context.Detail("   read %s, %d field(s) back (the baseline has %d), %d bit(s) left", read ? "true" : "false",
			back->m_nFieldCount, entity->m_nFieldCount, reader.GetNumBitsLeft());
		if (read && !entities_->AreSerializedEntitiesEqual(entity, back))
		{
			context.Suspect("what was written did not read back into an equal entity; the SDK does not "
			                "specify the wire format, so this may be the field list or mode argument");
		}
		entities_->ReleaseSerializedEntity(back);
	});
#else
	context.SkipCall("IFlattenedSerializers::WriteFieldList",
		"needs bf_write; tier1/bitbuf.cpp is not in this build (CS2SDKTEST_TIER1_BITBUF is off)");
	context.SkipCall("IFlattenedSerializers::ReadFields",
		"needs bf_read; tier1/bitbuf.cpp is not in this build (CS2SDKTEST_TIER1_BITBUF is off)");
#endif
}

// -----------------------------------------------------------------------------
//  What is not called, and why
// -----------------------------------------------------------------------------

void FlattenedSerializersSuite::NoteSkippedCalls(TestContext& context)
{
	context.SkipCall("IFlattenedSerializers::FindOrCreateCreateFlattenedSerializers",
		"builds and registers serializers from class introspection; changes engine state");
	context.SkipCall("IFlattenedSerializers::Encode",
		"needs the CEntityInstance being packed as pObject and a send proxy list");
	context.SkipCall("IFlattenedSerializers::GatherSendProxyResults",
		"needs the CEntityInstance being packed as pObject");
	context.SkipCall("IFlattenedSerializers::Decode",
		"writes the decoded fields into a live entity object");
	context.SkipCall("IFlattenedSerializers::BuildDeltaProperties",
		"needs the CEntityInstance being packed as pObject");
	context.SkipCall("IFlattenedSerializers::BuildMergedSerializedEntity",
		"the SDK does not say whether pBase and the packed delta are consumed");
	context.SkipCall("IFlattenedSerializers::CullFields",
		"needs a CSendProxyDataList from GatherSendProxyResults, which needs a live entity");
	context.SkipCall("IFlattenedSerializers::SpewSerializer",
		"needs an IFlattenedSerializerSpewFunc; the SDK only forward-declares it");
	context.SkipCall("IFlattenedSerializers::MakeSerializersMatchByMeta",
		"takes engine-internal void* meta from a demo or replay");
	context.SkipCall("IFlattenedSerializers::CreateReplayCompatSerializerFromMeta",
		"takes engine-internal void* meta from a demo or replay");
	context.SkipCall("IFlattenedSerializers::AssignChangeAccessorPathIds",
		"assigns path ids into a live object's change accessor");
	context.SkipCall("IFlattenedSerializers::ResolveChanges",
		"needs a VarChangeList_t from a live edict's change info");
	context.SkipCall("IFlattenedSerializers::FindFieldPathByAddress",
		"needs a live CEntityInstance and a field address inside it");
	context.SkipCall("IFlattenedSerializers::GatherSendProxyResultsForPath",
		"needs a live CEntityInstance");
	context.SkipCall("IFlattenedSerializers::DumpSerializedEntityToConsole",
		"prints the whole entity to the console; DumpSerializedEntityToLines covers the same path");
	context.SkipCall("IFlattenedSerializers::AddSpewListener",
		"needs an IFlattenedSerializerSpewListener; the SDK only forward-declares it");
	context.SkipCall("IFlattenedSerializers::RemoveSpewListener",
		"needs an IFlattenedSerializerSpewListener; the SDK only forward-declares it");
	context.SkipCall("IFlattenedSerializers::WriteSerializerInfos",
		"fills a CSVCMsg_FlattenedSerializer protobuf for a connecting client");
	context.SkipCall("IFlattenedSerializers::Report",
		"prints the engine's serializer report to the console");
	context.SkipCall("IFlattenedSerializers::AddArrayElementNameFormatter",
		"installs a global naming delegate that changes every later field path spew");
	context.SkipCall("IFlattenedSerializers::RemoveArrayElementNameFormatter",
		"pairs with AddArrayElementNameFormatter, which is skipped");
	context.SkipCall("IFlattenedSerializers::CreateNetworkFieldChangedEventQueue",
		"needs an INetworkFieldScratchData and an IFieldChangeLimitSpew");
	context.SkipCall("IFlattenedSerializers::BeginSerializerMetaMatching",
		"resets every serializer's matched meta variant");
	context.SkipCall("IFlattenedSerializers::EndSerializerMetaMatching",
		"pairs with BeginSerializerMetaMatching, which is skipped");
	context.SkipCall("IFlattenedSerializers::SetFieldEnabled",
		"turns a field off for every later delta and send proxy gather");
	context.SkipCall("IFlattenedSerializers::GatherPolymorphicMetadata",
		"needs the CEntityInstance being packed as pObject");
}

} // namespace cs2st
