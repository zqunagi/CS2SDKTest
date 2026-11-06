#include "suites/SerializedEntitiesSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <engine2/gameshareddefs.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/ivengineserver2.h>
#include <engine2/networkgameserverbase.h>
#include <interfaces/interfaces.h>
#include <networksystem/serializedentity.h>
#include <tier0/memalloc.h>
#include <tier0/utlstring.h>
#include <tier2/fieldpathpacker.h>

#include "core/Log.h"
#include "testkit/BaselineLookup.h"
#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

// Bounds for everything the engine counts.
constexpr int kMaxListed = 8;

// The SDK documents array counts of 100000 and more as invalid.
constexpr uint32_t kArrayCountLimit = 100000;

bool LooksLikePointer(const void* p)
{
	const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
	return v >= 0x10000 && v < 0x0000800000000000ull && (v & 7) == 0;
}

bool SameDesc(const FlattenedSerializerDesc_t& a, const FlattenedSerializerDesc_t& b)
{
	return std::memcmp(&a, &b, sizeof(FlattenedSerializerDesc_t)) == 0;
}

const char* Str(const char* text)
{
	return text != nullptr ? text : "<null>";
}

// One packed path through the packer, formatted for the log.
void DescribePath(TestContext& context, const CFieldPath& path)
{
	char text[96];
	int len = 0;
	text[0] = '\0';
	const int depth = path.GetDepth();
	for (int i = 0; i < depth && i < DEFAULT_MAX_PATH_DEPTH && len < 80; ++i)
	{
		len += std::snprintf(text + len, sizeof(text) - static_cast<std::size_t>(len),
			"%s%d", i == 0 ? "" : ", ", static_cast<int>(path[i]));
	}
	context.Detail("-> depth %d { %s }", depth, text);
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool SerializedEntitiesSuite::Acquire(TestContext& context)
{
	entities_ = static_cast<ISerializedEntities*>(
		context.QueryInterface("networksystem", SERIALIZEDENTITIES_INTERFACE_VERSION));

	if (entities_ == nullptr)
	{
		const Module& network_module = context.GetModule("networksystem");
		if (!network_module.IsValid())
		{
			context.UnavailableCall("ISerializedEntities", "the networksystem module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISerializedEntities",
				"networksystem is loaded but does not answer '%s'",
				SERIALIZEDENTITIES_INTERFACE_VERSION);
		}
		return false;
	}

	service_ = static_cast<INetworkServerService*>(
		context.QueryInterface("engine2", NETWORKSERVERSERVICE_INTERFACE_VERSION));

	context.Note("interface at %p (%s @ networksystem)", static_cast<void*>(entities_),
		SERIALIZEDENTITIES_INTERFACE_VERSION);
	return true;
}

void SerializedEntitiesSuite::ExerciseCalls(TestContext& context)
{
	context.Note("[ISerializedEntities] not an IAppSystem: slot 0 is AllocateSerializedEntity");

	ExercisePacker(context);
	ExerciseCounts(context);

	const CSerializedEntity* baseline = FindInstanceBaseline(context, service_);
	if (baseline == nullptr)
	{
		context.UnavailableCall("ISerializedEntities (entity calls)",
			"no instance baseline to take a serializer from; needs a loaded level");
	}
	else
	{
		InspectEntity(context, baseline);
		ExerciseOnEntity(context, baseline);
	}

	NoteSkippedCalls(context);
}

// -----------------------------------------------------------------------------
//  CFieldPathPacker
// -----------------------------------------------------------------------------

void SerializedEntitiesSuite::ExercisePacker(TestContext& context)
{
	context.Note("[CFieldPathPacker] UnpackPath is inline SDK code; the engine only owns the table");

	const CFieldPathPacker* packer = nullptr;
	CS2ST_CALL(context, "ISerializedEntities::GetFieldPathPacker", {
		packer = entities_->GetFieldPathPacker();
		context.Detail("-> %p", static_cast<const void*>(packer));
		if (packer == nullptr)
		{
			context.Fail("the packer is null; every path-unpacking row below is skipped");
		}
		else if (!LooksLikePointer(packer))
		{
			context.Fail("%p is not an object pointer", static_cast<const void*>(packer));
		}
	});

	if (packer == nullptr || !LooksLikePointer(packer))
	{
		context.UnavailableCall("CFieldPathPacker", "no packer to call");
		return;
	}

	CS2ST_CALL(context, "CFieldPathPacker::UnpackPath (0, the empty path)", {
		const CFieldPath path = packer->UnpackPath(0);
		DescribePath(context, path);
		if (path.GetDepth() != 0)
		{
			context.Fail("0 is documented as the empty path, got depth %d", path.GetDepth());
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackPath (-1)", {
		const CFieldPath path = packer->UnpackPath(static_cast<PackedFieldPath_t>(-1));
		DescribePath(context, path);
		if (path.GetDepth() != 1 || path[0] != 0x7FFF)
		{
			context.Fail("-1 is documented as the path { 0x7FFF }");
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackPath (three components)", {
		// Components are stored plus one: 9 bits from 22, 11 from 11, 11 from 0.
		const PackedFieldPath_t packed = (1u << 22) | (2u << 11) | 3u;
		const CFieldPath path = packer->UnpackPath(packed);
		DescribePath(context, path);
		if (path.GetDepth() != 3 || path[0] != 0 || path[1] != 1 || path[2] != 2)
		{
			context.Fail("expected { 0, 1, 2 } from 0x%08X", packed);
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackPath (largest components)", {
		const CFieldPath path = packer->UnpackPath(0x7FFFFFFFu);
		DescribePath(context, path);
		const bool ok = path.GetDepth() == 3 && path[0] == CFieldPathPacker::s_nDepthMax[0]
			&& path[1] == CFieldPathPacker::s_nDepthMax[1]
			&& path[2] == CFieldPathPacker::s_nDepthMax[2];
		if (!ok)
		{
			context.Fail("0x7FFFFFFF should give the per-depth maxima 510, 2046, 2046");
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackPath (stops at the first zero)", {
		// Depth 2 is zero, so depth 3 is never read even though it is set.
		const PackedFieldPath_t packed = (5u << 22) | 7u;
		const CFieldPath path = packer->UnpackPath(packed);
		DescribePath(context, path);
		if (path.GetDepth() != 1 || path[0] != 4)
		{
			context.Fail("expected { 4 } from 0x%08X", packed);
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackComplex (index the packer cannot have)", {
		// Walks the engine's CUtlStableHashtable through the SDK's layout; an
		// out-of-range handle must come back as the empty path.
		const CFieldPath path = packer->UnpackComplex(0x80000000u | 0x00FFFFF0u);
		DescribePath(context, path);
		if (path.GetDepth() != 0)
		{
			context.Suspect("an index nobody could have registered unpacked to depth %d",
				path.GetDepth());
		}
	});

	CS2ST_CALL(context, "CFieldPathPacker::UnpackComplex (index 0, the first long path)", {
		const CFieldPath path = packer->UnpackComplex(0x80000000u);
		DescribePath(context, path);
		if (path.GetDepth() > DEFAULT_MAX_PATH_DEPTH)
		{
			context.Fail("depth %d is past DEFAULT_MAX_PATH_DEPTH (%d); the SDK says it is cut off",
				path.GetDepth(), DEFAULT_MAX_PATH_DEPTH);
		}
		// Not every build has a path that does not fit in 32 bits yet.
		if (path.GetDepth() == 0)
		{
			context.Detail("(no long path registered yet, or the table layout is off)");
		}
	});

	CS2ST_CALL(context, "CFieldPath::operator== / DefaultHashFunctor<CFieldPath>", {
		const CFieldPath a = packer->UnpackPath((1u << 22) | (2u << 11) | 3u);
		const CFieldPath b = packer->UnpackPath((1u << 22) | (2u << 11) | 3u);
		const CFieldPath c = packer->UnpackPath((1u << 22) | (2u << 11) | 4u);
		const DefaultHashFunctor<CFieldPath> hash;
		context.Detail("-> hash a=%u b=%u c=%u", hash(a), hash(b), hash(c));
		if (!(a == b) || a != b)
		{
			context.Fail("two unpackings of the same value compare different");
		}
		if (a == c || !(a != c))
		{
			context.Fail("paths that differ in the last component compare equal");
		}
		if (hash(a) != hash(b))
		{
			context.Fail("equal paths hash differently");
		}
	});
}

// -----------------------------------------------------------------------------
//  Array count validation
// -----------------------------------------------------------------------------

void SerializedEntitiesSuite::ExerciseCounts(TestContext& context)
{
	context.Note("[ISerializedEntities] array count validation (documented bound: %u)",
		kArrayCountLimit);

	const uint32_t valid_counts[] = { 0, 1, 64, kArrayCountLimit - 1 };
	for (const uint32_t count : valid_counts)
	{
		char label[96];
		std::snprintf(label, sizeof(label), "ISerializedEntities::IsValidFieldArrayCount (%u)", count);
		CS2ST_CALL(context, label, {
			const bool ok = entities_->IsValidFieldArrayCount(count);
			context.Detail("-> %s", ok ? "true" : "false");
			if (!ok)
			{
				context.Fail("%u is below the documented limit but was rejected", count);
			}
		});
	}

	const uint32_t invalid_counts[] = { kArrayCountLimit, kArrayCountLimit + 1, 0xFFFFFFFFu };
	for (const uint32_t count : invalid_counts)
	{
		char label[96];
		std::snprintf(label, sizeof(label), "ISerializedEntities::IsValidFieldArrayCount (%u)", count);
		CS2ST_CALL(context, label, {
			const bool ok = entities_->IsValidFieldArrayCount(count);
			context.Detail("-> %s", ok ? "true" : "false");
			if (ok)
			{
				context.Fail("%u is at or above the documented limit but was accepted", count);
			}
		});
	}

	CS2ST_CALL(context, "ISerializedEntities::ValidateFieldArrayCount (valid count)", {
		// Only counts under the limit: a larger one logs and writes a minidump.
		entities_->ValidateFieldArrayCount(64, "CS2SDKTest");
		context.Detail("-> returned");
	});
}

// -----------------------------------------------------------------------------
//  Finding a real CSerializedEntity
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  Layouts
// -----------------------------------------------------------------------------

void SerializedEntitiesSuite::InspectEntity(TestContext& context, const CSerializedEntity* entity)
{
	context.Note("[CSerializedEntity] layout, read from a live baseline at %p",
		static_cast<const void*>(entity));

	CS2ST_CALL(context, "CSerializedEntity layout", {
		StructInspector inspect(context, "CSerializedEntity", entity, sizeof(CSerializedEntity));
		inspect.Pointer("m_pFieldPaths", offsetof(CSerializedEntity, m_pFieldPaths));
		inspect.Pointer("m_pFieldOffsets", offsetof(CSerializedEntity, m_pFieldOffsets));
		inspect.Pointer("m_pFieldMetadata", offsetof(CSerializedEntity, m_pFieldMetadata));
		inspect.Pointer("m_pData", offsetof(CSerializedEntity, m_pData));
		inspect.Raw("m_AssociatedSerializer", offsetof(CSerializedEntity, m_AssociatedSerializer),
			sizeof(FlattenedSerializerDesc_t));
		inspect.Int32("m_nFieldCount", offsetof(CSerializedEntity, m_nFieldCount), 0, 1 << 20);
		inspect.Int32("m_nDataBits", offsetof(CSerializedEntity, m_nDataBits), 0, 1 << 30);
		inspect.Int32("m_nDataBufferSize", offsetof(CSerializedEntity, m_nDataBufferSize), 0, 1 << 28);
		inspect.Bool("m_bReuseDataBuffer", offsetof(CSerializedEntity, m_bReuseDataBuffer));
		inspect.Summarize();
	});

	// The list headers, each reached through the pointer that owns them.
	CS2ST_CALL(context, "PackedFieldPathList_t layout", {
		const PackedFieldPathList_t* list = entity->m_pFieldPaths;
		if (list == nullptr || !LooksLikePointer(list))
		{
			context.SkipCall("PackedFieldPathList_t layout", "m_pFieldPaths is not a usable pointer");
			return;
		}
		StructInspector inspect(context, "PackedFieldPathList_t", list, sizeof(PackedFieldPathList_t));
		inspect.Int32("m_nRefs", offsetof(PackedFieldPathList_t, m_nRefs), 1, 1 << 24);
		inspect.UInt32("m_nCount", offsetof(PackedFieldPathList_t, m_nCount));
		inspect.Summarize();
		if (list->Count() != entity->m_nFieldCount)
		{
			context.Suspect("path list holds %d, entity says %d field(s)", list->Count(),
				entity->m_nFieldCount);
		}
	});

	CS2ST_CALL(context, "PackedFieldOffsets_t layout", {
		const PackedFieldOffsets_t* list = entity->m_pFieldOffsets;
		if (list == nullptr || !LooksLikePointer(list))
		{
			context.SkipCall("PackedFieldOffsets_t layout", "m_pFieldOffsets is not a usable pointer");
			return;
		}
		StructInspector inspect(context, "PackedFieldOffsets_t", list, sizeof(PackedFieldOffsets_t));
		inspect.Int32("m_nRefs", offsetof(PackedFieldOffsets_t, m_nRefs), 1, 1 << 24);
		inspect.Summarize();
	});

	CS2ST_CALL(context, "FieldMetadataList_t layout", {
		const FieldMetadataList_t* list = entity->m_pFieldMetadata;
		if (list == nullptr)
		{
			context.SkipCall("FieldMetadataList_t layout", "this entity has no metadata list");
			return;
		}
		if (!LooksLikePointer(list))
		{
			context.Fail("m_pFieldMetadata %p is not an object pointer", static_cast<const void*>(list));
			return;
		}
		StructInspector inspect(context, "FieldMetadataList_t", list, sizeof(FieldMetadataList_t));
		inspect.Int32("m_nRefs", offsetof(FieldMetadataList_t, m_nRefs), 1, 1 << 24);
		inspect.Int32("m_nCount", offsetof(FieldMetadataList_t, m_nCount), 0, static_cast<std::int64_t>(kArrayCountLimit));
		inspect.Summarize();
	});

	CS2ST_CALL(context, "CSerializedFieldStorage<PackedFieldPathList_t, PackedFieldPath_t> (elements)", {
		const auto* storage = entity->m_pFieldPaths;
		if (storage == nullptr || !LooksLikePointer(storage))
		{
			context.SkipCall("CSerializedFieldStorage<PackedFieldPathList_t, PackedFieldPath_t> (elements)",
				"no path list");
			return;
		}
		// The elements follow the header in the same allocation; unpack the
		// first few through the engine's packer to see they are real paths.
		const CFieldPathPacker* packer = entities_->GetFieldPathPacker();
		const int count = storage->Count() < kMaxListed ? storage->Count() : kMaxListed;
		int unpacked = 0;
		for (int i = 0; i < count && packer != nullptr; ++i)
		{
			const PackedFieldPath_t packed = storage->Element(i);
			const CFieldPath path = packer->UnpackPath(packed);
			if (path.GetDepth() > 0)
			{
				++unpacked;
			}
			context.Detail("   [%d] 0x%08X -> depth %d", i, packed, path.GetDepth());
		}
		context.Detail("-> %d of %d unpacked to a non-empty path", unpacked, count);
		if (count > 0 && unpacked == 0)
		{
			context.Suspect("none of the first %d packed paths unpacked; the element offset is probably off", count);
		}
	});
}

// -----------------------------------------------------------------------------
//  Calls that need a real entity
// -----------------------------------------------------------------------------

void SerializedEntitiesSuite::ExerciseOnEntity(TestContext& context, const CSerializedEntity* entity)
{
	context.Note("[ISerializedEntities] calls on baseline %p, serializer taken from it",
		static_cast<const void*>(entity));

	const FlattenedSerializerDesc_t desc = entity->m_AssociatedSerializer;

	CS2ST_CALL(context, "ISerializedEntities::GetAssociatedSerializer", {
		const FlattenedSerializerDesc_t got = entities_->GetAssociatedSerializer(entity);
		context.Detail("-> name \"%s\", handle %p", Str(got.m_name.String()),
			static_cast<const void*>(got.m_handle));
		if (!SameDesc(got, desc))
		{
			context.Fail("does not match CSerializedEntity::m_AssociatedSerializer");
		}
	});

	CS2ST_CALL(context, "ISerializedEntities::GetSerializedEntityFieldCount (no output)", {
		const int count = entities_->GetSerializedEntityFieldCount(entity, nullptr);
		context.Detail("-> %d", count);
		if (count != entity->m_nFieldCount)
		{
			context.Fail("returned %d, CSerializedEntity::m_nFieldCount is %d", count,
				entity->m_nFieldCount);
		}
	});

	CS2ST_CALL(context, "ISerializedEntities::AreSerializedEntitiesEqual (entity, itself)", {
		const bool equal = entities_->AreSerializedEntitiesEqual(entity, entity);
		context.Detail("-> %s", equal ? "true" : "false");
		if (!equal)
		{
			context.Fail("an entity is not equal to itself");
		}
	});

	CS2ST_CALL(context, "ISerializedEntities::CompareSerializedEntities (entity, itself)", {
		// Logs each difference; against itself there is none to log.
		const bool result = entities_->CompareSerializedEntities(entity, entity, NET_SERIALIZATION_MODE_DEFAULT);
		context.Detail("-> %s", result ? "true" : "false");
	});

	CS2ST_CALL(context, "ISerializedEntities::DescribeSerializedEntity", {
		const CUtlString text = entities_->DescribeSerializedEntity(entity);
		const char* chars = text.Get();
		const std::size_t length = chars != nullptr ? std::strlen(chars) : 0;
		context.Detail("-> %zu char(s): \"%.80s\"", length, Str(chars));
		if (length == 0)
		{
			context.Suspect("a baseline with %d field(s) described as empty", entity->m_nFieldCount);
		}
	});

	// Copy / Release: the copy is ours and is released before returning.
	CS2ST_CALL(context, "ISerializedEntities::CopySerializedEntity + ReleaseSerializedEntity", {
		CSerializedEntity* copy = entities_->CopySerializedEntity(entity);
		context.Detail("-> %p", static_cast<void*>(copy));
		if (copy == nullptr)
		{
			context.Fail("copying a live baseline returned null");
			return;
		}
		if (copy == entity)
		{
			context.Fail("the copy is the original");
		}
		if (copy->m_nFieldCount != entity->m_nFieldCount || copy->m_nDataBits != entity->m_nDataBits)
		{
			context.Fail("copy has %d field(s) / %d bits, the original %d / %d", copy->m_nFieldCount,
				copy->m_nDataBits, entity->m_nFieldCount, entity->m_nDataBits);
		}
		if (!SameDesc(copy->m_AssociatedSerializer, desc))
		{
			context.Fail("copy lost the associated serializer");
		}
		if (copy->m_pFieldPaths != entity->m_pFieldPaths)
		{
			context.Suspect("copy did not share the field path list, which the SDK says copies do");
		}
		if (!entities_->AreSerializedEntitiesEqual(entity, copy))
		{
			context.Fail("a copy is not equal to its original");
		}
		entities_->ReleaseSerializedEntity(copy);
	});

	CS2ST_CALL(context, "ISerializedEntities::AllocateSerializedEntity + ReplaceSerializedEntity + SetReuseDataBuffer", {
		CSerializedEntity* made = entities_->AllocateSerializedEntity(desc, true, 100);
		context.Detail("-> %p", static_cast<void*>(made));
		if (made == nullptr)
		{
			context.Fail("allocating with a live serializer returned null");
			return;
		}
		if (!SameDesc(made->m_AssociatedSerializer, desc))
		{
			context.Fail("the new entity does not carry the serializer it was given");
		}
		if (made->m_nFieldCount != 0)
		{
			context.Suspect("a fresh entity has %d field(s)", made->m_nFieldCount);
		}
		context.Detail("   buffer %d byte(s), reuse %d (asked for 100, reuse on: documented as 128)",
			made->m_nDataBufferSize, static_cast<int>(*reinterpret_cast<const unsigned char*>(&made->m_bReuseDataBuffer)));
		if (made->m_nDataBufferSize != 128)
		{
			context.Fail("100 bytes with reuse set should round up to 128, got %d", made->m_nDataBufferSize);
		}

		entities_->SetReuseDataBuffer(made, false);
		if (made->m_bReuseDataBuffer)
		{
			context.Fail("SetReuseDataBuffer(false) left the flag set");
		}

		entities_->ReplaceSerializedEntity(made, entity);
		if (made->m_nFieldCount != entity->m_nFieldCount)
		{
			context.Fail("after Replace the entity has %d field(s), the source %d", made->m_nFieldCount,
				entity->m_nFieldCount);
		}
		if (!entities_->AreSerializedEntitiesEqual(entity, made))
		{
			context.Fail("an entity replaced by the source is not equal to it");
		}
		entities_->ReleaseSerializedEntity(made);
	});

	CS2ST_CALL(context, "ISerializedEntities::CreateSerializedEntityBlob + GetSerializedEntityBlobSize + FreeSerializedEntityBlob", {
		// No pool, so the blob is heap allocated and FreeSerializedEntityBlob
		// releases it together with its resources.
		void* blob = entities_->CreateSerializedEntityBlob(entity, nullptr, nullptr);
		context.Detail("-> %p", blob);
		if (blob == nullptr)
		{
			context.Fail("packing a live baseline into a blob returned null");
			return;
		}
		const std::size_t size = entities_->GetSerializedEntityBlobSize(blob);
		context.Detail("   blob size %zu byte(s)", size);
		if (size == 0 || size > (std::size_t(1) << 28))
		{
			context.Suspect("blob size %zu is not plausible", size);
		}
		entities_->FreeSerializedEntityBlob(blob);
	});

	// ---- metadata, against the entries read straight out of the list --------

	CS2ST_CALL(context, "ISerializedEntities::FindFieldMetadata", {
		const FieldMetadataList_t* list = entity->m_pFieldMetadata;
		const int count = list != nullptr ? list->Count() : 0;
		if (count <= 0 || count > static_cast<int>(kArrayCountLimit))
		{
			// Nothing to look up; the miss path still has an answer.
			const PolymorphicMetadataEntry_t* none = entities_->FindFieldMetadata(entity, 0x7FFFFFFEu);
			context.Detail("-> no metadata on this entity; unknown path gives %p", static_cast<const void*>(none));
			if (none != nullptr)
			{
				context.Suspect("a path the entity does not have found an entry");
			}
			return;
		}
		const PolymorphicMetadataEntry_t& first = entity->m_pFieldMetadata->Element(0);
		const PolymorphicMetadataEntry_t* found = entities_->FindFieldMetadata(entity, first.m_nFieldPath);
		context.Detail("-> %p for path 0x%08X (%d entr%s)", static_cast<const void*>(found),
			first.m_nFieldPath, count, count == 1 ? "y" : "ies");
		if (found == nullptr)
		{
			context.Fail("the first entry's own path was not found");
		}
		else if (found->m_nFieldPath != first.m_nFieldPath)
		{
			context.Fail("found an entry for path 0x%08X when asked for 0x%08X", found->m_nFieldPath,
				first.m_nFieldPath);
		}
	});

	for (unsigned type = 0; type < 2; ++type)
	{
		char label[96];
		std::snprintf(label, sizeof(label), "ISerializedEntities::GetFieldMetadata (type %u, %s)", type,
			type == 0 ? "array count" : "polymorphic id");
		CS2ST_CALL(context, label, {
			PolymorphicMetadata_t out;
			out.m_nCount = 0;
			out.m_pEntries = nullptr;
			out.m_bNeedsSort = false;
			entities_->GetFieldMetadata(entity, &out, static_cast<uint8>(type));
			context.Detail("-> %d entr%s at %p", out.m_nCount, out.m_nCount == 1 ? "y" : "ies",
				static_cast<void*>(out.m_pEntries));

			// The same count, counted by hand from the list.
			int expected = 0;
			const FieldMetadataList_t* list = entity->m_pFieldMetadata;
			if (list != nullptr)
			{
				const int total = list->Count();
				const auto* storage = entity->m_pFieldMetadata;
				for (int i = 0; i < total && i < static_cast<int>(kArrayCountLimit); ++i)
				{
					if (storage->Element(i).m_nType == type)
					{
						++expected;
					}
				}
			}
			if (out.m_nCount != expected)
			{
				context.Fail("returned %d entr%s, the list holds %d of this type", out.m_nCount,
					out.m_nCount == 1 ? "y" : "ies", expected);
			}
			// "Expects caller to free it": the shared tier0 allocator.
			if (out.m_pEntries != nullptr && LooksLikePointer(out.m_pEntries))
			{
				g_pMemAlloc->Free(out.m_pEntries);
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  What is not called, and why
// -----------------------------------------------------------------------------

void SerializedEntitiesSuite::NoteSkippedCalls(TestContext& context)
{
	context.SkipCall("ISerializedEntities::AllocSerializedEntityBlob",
		"returns raw memory that FreeSerializedEntityBlob would read as a blob header");
	context.SkipCall("ISerializedEntities::ReleaseSerializedEntityBlobResources",
		"only for blobs allocated from a CUtlScratchMemoryPool the plugin cannot construct");
	context.SkipCall("ISerializedEntities::ReportMemoryUsage",
		"dumps the whole engine report to the console");
	context.SkipCall("ISerializedEntities::GetMemoryUsage",
		"needs the engine's counted-lists set, passed as void*");
	context.SkipCall("ISerializedEntities::GatherFieldMemoryUsage",
		"needs the engine's field stats and counted-lists sets, passed as void*");
	context.SkipCall("ISerializedEntities::ValidateFieldArrayCount (count >= 100000)",
		"logs and writes a minidump, the first four times");
}

} // namespace cs2st
