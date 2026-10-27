#include "suites/Tier0ContainerSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>
#include <tier0/utlbuffer.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "testkit/PrivateField.h"
#include "testkit/StructInspector.h"
#include "testkit/UtlVectorGuard.h"

// Everything below is reached through the private/protected members the SDK
// declares. Offsets come from the declaration (see testkit/PrivateField.h),
// never from numbers written here.

// CUtlString has one member, the pointer. (ServerConfigSuite reads it too, under
// its own tag; tags are per translation unit.)
CS2ST_PRIVATE_FIELD(kT0UtlStringPtr, CUtlString, char*, m_pString);

// CUtlVector<T>: m_Size and m_Memory live in CUtlVectorBase, the allocator's
// three fields in CUtlVectorMemory_Growable. Two element types, because the
// engine-owned vector holds void* and the local one holds int.
typedef CUtlVectorMemory_Growable<int, int> T0IntMemory;
typedef CUtlVectorBase<int, int, T0IntMemory> T0IntVectorBase;
CS2ST_PRIVATE_FIELD(kT0IntVecSize,  T0IntVectorBase, int, m_Size);
CS2ST_PRIVATE_FIELD(kT0IntVecMem,   T0IntVectorBase, T0IntMemory, m_Memory);
CS2ST_PRIVATE_FIELD(kT0IntMemPtr,   T0IntMemory, int*, m_pMemory);
CS2ST_PRIVATE_FIELD(kT0IntMemAlloc, T0IntMemory, int, m_nAllocationCount);
CS2ST_PRIVATE_FIELD(kT0IntMemGrow,  T0IntMemory, int, m_nGrowSize);

typedef CUtlVectorMemory_Growable<void*, int> T0PtrMemory;
typedef CUtlVectorBase<void*, int, T0PtrMemory> T0PtrVectorBase;
CS2ST_PRIVATE_FIELD(kT0PtrVecSize,  T0PtrVectorBase, int, m_Size);
CS2ST_PRIVATE_FIELD(kT0PtrVecMem,   T0PtrVectorBase, T0PtrMemory, m_Memory);
CS2ST_PRIVATE_FIELD(kT0PtrMemPtr,   T0PtrMemory, void**, m_pMemory);
CS2ST_PRIVATE_FIELD(kT0PtrMemAlloc, T0PtrMemory, int, m_nAllocationCount);
CS2ST_PRIVATE_FIELD(kT0PtrMemGrow,  T0PtrMemory, int, m_nGrowSize);

// CUtlBuffer's state is all protected.
typedef CUtlLeanVector<unsigned char> T0BufferMemory;
typedef bool (CUtlBuffer::*T0BufferOverflowFn)(int);
CS2ST_PRIVATE_FIELD(kT0BufMemory,  CUtlBuffer, T0BufferMemory, m_Memory);
CS2ST_PRIVATE_FIELD(kT0BufGet,     CUtlBuffer, int, m_Get);
CS2ST_PRIVATE_FIELD(kT0BufPut,     CUtlBuffer, int, m_Put);
CS2ST_PRIVATE_FIELD(kT0BufError,   CUtlBuffer, unsigned char, m_Error);
CS2ST_PRIVATE_FIELD(kT0BufFlags,   CUtlBuffer, unsigned char, m_Flags);
CS2ST_PRIVATE_FIELD(kT0BufTab,     CUtlBuffer, int, m_nTab);
CS2ST_PRIVATE_FIELD(kT0BufMaxPut,  CUtlBuffer, int, m_nMaxPut);
CS2ST_PRIVATE_FIELD(kT0BufOffset,  CUtlBuffer, int, m_nOffset);
CS2ST_PRIVATE_FIELD(kT0BufGetFn,   CUtlBuffer, T0BufferOverflowFn, m_GetOverflowFunc);
CS2ST_PRIVATE_FIELD(kT0BufPutFn,   CUtlBuffer, T0BufferOverflowFn, m_PutOverflowFunc);

namespace cs2st
{
namespace
{

typedef CUtlVector<int> T0IntVector;

/** Allocation counts carry the "external buffer" marker bits above bit 29. */
constexpr int kAllocationCountMask = 0x3FFFFFFF;

template <typename T>
T ReadAt(const void* base, std::size_t offset)
{
	T value;
	std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
	return value;
}

/**
 * Reads a CUtlVector's raw fields and compares them with what its own
 * accessors say. The two views are the same bytes seen through different
 * declarations, so any disagreement means the field offsets the SDK computed
 * are not where the accessors actually read.
 *
 * Fails only on facts the container's own type makes impossible: a size past
 * the allocation, a Base() that is not the pointer field.
 */
template <typename SizeTag, typename MemTag, typename PtrTag, typename AllocTag, typename GrowTag,
          typename Vector>
void CheckVectorRaw(TestContext& context, Vector& vector)
{
	const typename SizeTag::Owner* base = &vector;
	const std::size_t off_size = PrivateFieldOffset<SizeTag>(base);
	const std::size_t off_mem = PrivateFieldOffset<MemTag>(base);
	if (off_size == static_cast<std::size_t>(-1) || off_mem == static_cast<std::size_t>(-1))
	{
		context.Fail("a private field offset could not be resolved");
		return;
	}

	const typename PtrTag::Owner* memory = reinterpret_cast<const typename PtrTag::Owner*>(
		reinterpret_cast<const unsigned char*>(base) + off_mem);
	const std::size_t off_ptr = PrivateFieldOffset<PtrTag>(memory);
	const std::size_t off_alloc = PrivateFieldOffset<AllocTag>(memory);
	const std::size_t off_grow = PrivateFieldOffset<GrowTag>(memory);

	const int raw_size = ReadAt<int>(base, off_size);
	const void* raw_ptr = ReadAt<const void*>(memory, off_ptr);
	const int raw_alloc = ReadAt<int>(memory, off_alloc);
	const int raw_grow = ReadAt<int>(memory, off_grow);

	const int count = vector.Count();
	const int allocated = raw_alloc & kAllocationCountMask;

	context.Detail("m_Size@0x%zx=%d (Count()=%d), m_Memory@0x%zx{ptr@+0x%zx=%p alloc@+0x%zx=0x%x grow@+0x%zx=%d}, sizeof=%zu",
		off_size, raw_size, count, off_mem, off_ptr, raw_ptr, off_alloc,
		static_cast<unsigned>(raw_alloc), off_grow, raw_grow, sizeof(Vector));

	if (raw_size != count)
	{
		context.Fail("m_Size and Count() disagree: the field is not where Count() reads");
	}
	if (static_cast<const void*>(vector.Base()) != raw_ptr)
	{
		context.Fail("m_pMemory and Base() disagree");
	}
	if (raw_size < 0 || raw_size > allocated)
	{
		context.Fail("m_Size is %d but only %d slots are allocated", raw_size, allocated);
	}
	if (raw_size > 0 && raw_ptr == nullptr)
	{
		context.Fail("a vector with %d elements has no storage", raw_size);
	}
	if (raw_grow < 0)
	{
		context.Suspect("m_nGrowSize is negative (%d)", raw_grow);
	}
}

// Wrappers with no commas in the call, so CS2ST_CALL bodies can use them.
void CheckLocalIntVector(TestContext& context, T0IntVector& vector)
{
	CheckVectorRaw<kT0IntVecSize, kT0IntVecMem, kT0IntMemPtr, kT0IntMemAlloc, kT0IntMemGrow>(
		context, vector);
}

void CheckEngineVector(TestContext& context, CUtlVector<void*>& vector)
{
	CheckVectorRaw<kT0PtrVecSize, kT0PtrVecMem, kT0PtrMemPtr, kT0PtrMemAlloc, kT0PtrMemGrow>(
		context, vector);
}

/** Appends until the vector holds `count` elements; element i is always i * 10. */
void FillLocalIntVector(T0IntVector& vector, int count)
{
	for (int i = vector.Count(); i < count; ++i)
	{
		vector.AddToTail(i * 10);
	}
}

bool LocalIntVectorHolds(const T0IntVector& vector, int count)
{
	if (vector.Count() != count)
	{
		return false;
	}
	for (int i = 0; i < count; ++i)
	{
		if (vector.Base()[i] != i * 10)
		{
			return false;
		}
	}
	return true;
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool Tier0ContainerSuite::Acquire(TestContext& context)
{
	// Nothing here needs an interface except the engine-owned vector row, so a
	// missing schemasystem degrades that one row and never the suite.
	schema_ = static_cast<ISchemaSystem*>(
		context.QueryInterface("schemasystem", SCHEMASYSTEM_INTERFACE_VERSION));
	context.Note("schemasystem %s (only the engine-owned CUtlVector row uses it)",
		schema_ != nullptr ? "acquired" : "not available");
	return true;
}

void Tier0ContainerSuite::ExerciseCalls(TestContext& context)
{
	ExerciseUtlString(context);
	ExerciseUtlBuffer(context);
	ExerciseUtlVector(context);
	NoteUncoveredContainers(context);
}

// -----------------------------------------------------------------------------
//  CUtlString - built by tier0, read by the SDK header
// -----------------------------------------------------------------------------

void Tier0ContainerSuite::ExerciseUtlString(TestContext& context)
{
	context.Note("[CUtlString] sizeof %zu; Set/Append/Purge run inside tier0, Get/Length/IsEmpty are inline",
		sizeof(CUtlString));

	CUtlString text;

	CS2ST_CALL(context, "CUtlString::m_pString offset and size", {
		const std::size_t offset = PrivateFieldOffset<kT0UtlStringPtr>(&text);
		context.Detail("m_pString@0x%zx, sizeof(CUtlString)=%zu, sizeof(char*)=%zu",
			offset, sizeof(CUtlString), sizeof(char*));
		if (offset != 0 || sizeof(CUtlString) != sizeof(char*))
		{
			context.Fail("CUtlString is declared as a single pointer; this is not");
		}
	});

	CS2ST_CALL(context, "CUtlString::Set + Append (tier0) vs Get/Length (inline)", {
		text.Set("cs2sdktest");
		text.Append("-tail");

		const std::size_t offset = PrivateFieldOffset<kT0UtlStringPtr>(&text);
		const char* raw = ReadAt<const char*>(&text, offset);
		const char* got = text.Get();
		const int length = text.Length();

		context.Detail("raw=%p Get()=%p \"%.40s\" Length()=%d", static_cast<const void*>(raw),
			static_cast<const void*>(got), got, length);

		if (raw == nullptr)
		{
			context.Fail("tier0 wrote a string and the field the header reads is still null");
		}
		else if (raw != got)
		{
			context.Fail("Get() does not return the pointer stored in m_pString");
		}
		if (std::strcmp(got, "cs2sdktest-tail") != 0)
		{
			context.Fail("the engine wrote \"cs2sdktest-tail\" and the header reads something else");
		}
		if (length != 15)
		{
			context.Fail("Length() is %d for a 15 character string", length);
		}
		if (text.IsEmpty())
		{
			context.Fail("IsEmpty() on a string tier0 just filled");
		}
	});

	CS2ST_CALL(context, "CUtlString::Purge (tier0) vs IsEmpty/Get (inline)", {
		text.Purge();

		const std::size_t offset = PrivateFieldOffset<kT0UtlStringPtr>(&text);
		const char* raw = ReadAt<const char*>(&text, offset);
		context.Detail("raw=%p IsEmpty()=%d Get()=\"%s\"", static_cast<const void*>(raw),
			text.IsEmpty() ? 1 : 0, text.Get());

		if (!text.IsEmpty())
		{
			context.Fail("IsEmpty() is false after Purge()");
		}
		if (raw != nullptr)
		{
			// Suspect, not Fail: nothing in the SDK says Purge nulls the pointer
			// rather than pointing it at a shared empty string.
			context.Suspect("Purge() left m_pString non-null");
		}
	});
}

// -----------------------------------------------------------------------------
//  CUtlBuffer - constructed by tier0, state read through the SDK's accessors
// -----------------------------------------------------------------------------

void Tier0ContainerSuite::ExerciseUtlBuffer(TestContext& context)
{
	context.Note("[CUtlBuffer] sizeof %zu; the external-memory constructor runs inside tier0, the "
	             "accessors below are inline in the header", sizeof(CUtlBuffer));

	// External, read-only memory: the engine builds the object but allocates
	// nothing, so the destructor has no heap to free (the Windows allocator
	// split described in UtlVectorGuard.h cannot apply).
	alignas(8) unsigned char storage[64];
	std::memset(storage, 0, sizeof(storage));

	alignas(CUtlBuffer) unsigned char object[sizeof(CUtlBuffer)];
	std::memset(object, 0, sizeof(object));
	CUtlBuffer* buffer = nullptr;

	CS2ST_CALL(context, "CUtlBuffer::CUtlBuffer(external, READ_ONLY) (tier0 constructor)", {
		buffer = new (object) CUtlBuffer(storage, static_cast<int>(sizeof(storage)),
			CUtlBuffer::READ_ONLY);
		context.Detail("constructed at %p over %p (%zu bytes)", static_cast<void*>(buffer),
			static_cast<void*>(storage), sizeof(storage));
	});

	if (buffer == nullptr)
	{
		context.SkipCall("CUtlBuffer state", "the constructor did not complete");
		return;
	}

	CS2ST_CALL(context, "CUtlBuffer inline accessors vs the engine-built object", {
		const unsigned char raw_flags = ReadAt<unsigned char>(buffer, PrivateFieldOffset<kT0BufFlags>(buffer));
		const int raw_get = ReadAt<int>(buffer, PrivateFieldOffset<kT0BufGet>(buffer));
		const int raw_put = ReadAt<int>(buffer, PrivateFieldOffset<kT0BufPut>(buffer));
		const int raw_max = ReadAt<int>(buffer, PrivateFieldOffset<kT0BufMaxPut>(buffer));

		context.Detail("flags raw=0x%x GetFlags()=0x%x; get raw=%d TellGet()=%d; put raw=%d TellPut()=%d; "
		               "maxput raw=%d TellMaxPut()=%d; Size()=%d Base()=%p external=%d valid=%d readonly=%d",
			raw_flags, buffer->GetFlags(), raw_get, buffer->TellGet(), raw_put, buffer->TellPut(),
			raw_max, buffer->TellMaxPut(), buffer->Size(), buffer->Base(),
			buffer->IsExternallyAllocated() ? 1 : 0, buffer->IsValid() ? 1 : 0,
			buffer->IsReadOnly() ? 1 : 0);

		if (raw_flags != buffer->GetFlags() || raw_get != buffer->TellGet()
			|| raw_put != buffer->TellPut() || raw_max != buffer->TellMaxPut())
		{
			context.Fail("an accessor does not read the field the SDK says it reads");
		}
		if (buffer->Base() != static_cast<void*>(storage))
		{
			context.Fail("the engine attached the buffer to memory other than the one it was given");
		}
		if ((buffer->GetFlags() & CUtlBuffer::READ_ONLY) == 0 || !buffer->IsReadOnly())
		{
			context.Fail("READ_ONLY was passed to the constructor and the header does not see it");
		}
		if (!buffer->IsExternallyAllocated())
		{
			context.Suspect("the buffer is over caller memory but IsExternallyAllocated() is false");
		}
		if (buffer->Size() != static_cast<int>(sizeof(storage)))
		{
			context.Suspect("Size() is %d for a %zu byte external buffer", buffer->Size(), sizeof(storage));
		}
	});

	// One StructInspector for the one declared class: CUtlBuffer has no base
	// with data. CUtlLeanVector (m_Memory) and CByteswap (m_Byteswap) are
	// members, not bases, so they are reported as raw spans here rather than
	// getting their own section.
	{
		StructInspector inspect(context, "CUtlBuffer", buffer, sizeof(CUtlBuffer));
		inspect.Raw   ("m_Memory",  PrivateFieldOffset<kT0BufMemory>(buffer), sizeof(T0BufferMemory));
		inspect.Int32 ("m_Get",     PrivateFieldOffset<kT0BufGet>(buffer), 0, INT32_MAX);
		inspect.Int32 ("m_Put",     PrivateFieldOffset<kT0BufPut>(buffer), 0, INT32_MAX);
		inspect.UInt8 ("m_Error",   PrivateFieldOffset<kT0BufError>(buffer));
		inspect.UInt8 ("m_Flags",   PrivateFieldOffset<kT0BufFlags>(buffer));
		inspect.Int32 ("m_nTab",    PrivateFieldOffset<kT0BufTab>(buffer), 0, INT32_MAX);
		inspect.Int32 ("m_nMaxPut", PrivateFieldOffset<kT0BufMaxPut>(buffer), 0, INT32_MAX);
		inspect.Int32 ("m_nOffset", PrivateFieldOffset<kT0BufOffset>(buffer), 0, INT32_MAX);
		inspect.Raw   ("m_GetOverflowFunc", PrivateFieldOffset<kT0BufGetFn>(buffer), sizeof(T0BufferOverflowFn));
		inspect.Raw   ("m_PutOverflowFunc", PrivateFieldOffset<kT0BufPutFn>(buffer), sizeof(T0BufferOverflowFn));
		inspect.Summarize();
		inspect.HexDump();
	}

	buffer->~CUtlBuffer();
}

// -----------------------------------------------------------------------------
//  CUtlVector - one engine-owned instance, one local
// -----------------------------------------------------------------------------

void Tier0ContainerSuite::ExerciseUtlVector(TestContext& context)
{
	context.Note("[CUtlVector] sizeof(CUtlVector<int>) %zu, sizeof(CUtlVectorMemory_Growable<int,int>) %zu",
		sizeof(T0IntVector), sizeof(T0IntMemory));

	if (schema_ == nullptr)
	{
		context.UnavailableCall("CUtlVector (engine-owned, CSchemaSystem::m_LoadedModules)",
			"the schemasystem module is not available");
	}
	else
	{
		CS2ST_CALL(context, "CUtlVector<void*> (engine-owned, CSchemaSystem::m_LoadedModules)", {
			// The cast is the same assumption SchemaSystemSuite makes and checks
			// with SchemaSystemIsReady(); repeat that check rather than trust it.
			CSchemaSystem* concrete = static_cast<CSchemaSystem*>(schema_);
			if (concrete->m_bSchemaSystemIsReady != schema_->SchemaSystemIsReady())
			{
				context.Suspect("CSchemaSystem cast not confirmed (SchemaSystemSuite explains); "
				                "the vector below may be read from the wrong place");
			}
			CheckEngineVector(context, concrete->m_LoadedModules);
		});
	}

	// What a vector built here proves is narrower: the SDK header agrees with
	// itself, and growth through tier0's allocator leaves it consistent.
	CS2ST_CALL(context, "CUtlVector<int> (local; header self-consistency only)", {
		T0IntVector numbers;
		UtlVectorGuard<T0IntVector> guard(numbers);

		FillLocalIntVector(numbers, 5);
		context.Detail("after 5 AddToTail: Count()=%d NumAllocated()=%d", numbers.Count(),
			numbers.NumAllocated());
		if (!LocalIntVectorHolds(numbers, 5))
		{
			context.Fail("elements read back wrong after AddToTail");
		}
		CheckLocalIntVector(context, numbers);

		// Past the first growth step, so the allocator's grow path runs too.
		FillLocalIntVector(numbers, 40);
		if (!LocalIntVectorHolds(numbers, 40))
		{
			context.Fail("elements read back wrong after growth");
		}
		CheckLocalIntVector(context, numbers);
	});
}

// -----------------------------------------------------------------------------
//  What is deliberately not here
// -----------------------------------------------------------------------------

void Tier0ContainerSuite::NoteUncoveredContainers(TestContext& context)
{
	context.SkipCall("CUtlOrderedMap / CUtlStringMap",
		"SchemaSystemSuite already walks the engine-owned instances "
		"(m_DeclaredClasses, m_TypeScopes); a local map would only test this plugin's own compile");
	context.SkipCall("CUtlTSHash",
		"no engine-owned instance is reachable from the interfaces s2sdk exposes, and "
		"constructing one here would test this plugin's own compile");
	context.SkipCall("CUtlMemory",
		"Source 2 has no CUtlMemory; the allocator is CUtlVectorMemory_Growable, "
		"checked through the CUtlVector rows above");
	context.SkipCall("CUtlBuffer growable constructor",
		"allocates from tier0's heap and the destructor frees with this plugin's, the split "
		"UtlVectorGuard.h works around on Windows; the external-memory constructor has no allocation");
}

} // namespace cs2st
