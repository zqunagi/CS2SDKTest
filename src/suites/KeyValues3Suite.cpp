#include "suites/KeyValues3Suite.h"

#if defined(CS2SDKTEST_WITH_KEYVALUES3)

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include <engine2/isource2server.h>
#include <tier0/buffer_string.h>
#include <tier0/keyvalues3.h>

#include "core/Log.h"
#include "testkit/PrivateField.h"
#include "testkit/StructInspector.h"

// The 8 bytes after the bitfield word; which member is live depends on the type.
CS2ST_PRIVATE_FIELD(kKv3Data, KeyValues3, KeyValues3::Data_t, m_Data);

namespace cs2st
{
namespace
{

constexpr int kMaxMembersWalked = 4096;
constexpr int kMaxNamesPrinted = 16;

/** Bit order of KeyValues3's bitfields, as declared: ctx, freeArray, TypeEx:8, SubType:8. */
unsigned DecodeTypeEx(std::uint64_t word) { return static_cast<unsigned>((word >> 2) & 0xFF); }
unsigned DecodeSubType(std::uint64_t word) { return static_cast<unsigned>((word >> 10) & 0xFF); }

std::uint64_t ReadWord(const KeyValues3* kv)
{
	std::uint64_t word;
	std::memcpy(&word, kv, sizeof(word));
	return word;
}

/** A KeyValues3 in storage that is never destructed (see the header comment). */
KeyValues3* MakeLeakedKv3()
{
	void* storage = ::operator new(sizeof(KeyValues3));
	return new (storage) KeyValues3();
}

} // namespace

bool KeyValues3Suite::Acquire(TestContext& context)
{
	server_ = static_cast<ISource2Server*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEDLL));
	if (server_ == nullptr)
	{
		context.UnavailableCall("ISource2Server", "the server module does not answer '%s'",
			INTERFACEVERSION_SERVERGAMEDLL);
	}
	// The local rows need no interface, so the suite runs either way.
	return true;
}

void KeyValues3Suite::ExerciseCalls(TestContext& context)
{
	context.Note("[KeyValues3] sizeof %zu; implementation is compiled into this plugin, "
	             "so only the engine-filled rows can contradict the engine", sizeof(KeyValues3));
	ExerciseEngineFilled(context);
	ExerciseLocal(context);
}

void KeyValues3Suite::ExerciseEngineFilled(TestContext& context)
{
	if (server_ == nullptr)
	{
		context.SkipCall("ISource2Server::WriteStatusJson", "ISource2Server was not acquired");
		return;
	}

	KeyValues3* root = MakeLeakedKv3();

	CS2ST_CALL(context, "ISource2Server::WriteStatusJson (engine writes into a KeyValues3)", {
		server_->WriteStatusJson(root);
		context.Detail("type=%s subtype=%s typeex=%u members=%d", root->GetTypeAsString(),
			root->GetSubTypeAsString(), static_cast<unsigned>(root->GetTypeEx()),
			root->IsTable() ? root->GetMemberCount() : -1);
		if (root->IsNull())
		{
			context.Suspect("the engine left the root null (nothing to write while no map is "
			                "loaded?); the rows below have nothing to read");
		}
	});

	CS2ST_CALL(context, "KeyValues3 bitfield word vs GetTypeEx/GetSubType (engine-filled)", {
		const std::uint64_t word = ReadWord(root);
		const unsigned type_ex = DecodeTypeEx(word);
		const unsigned sub_type = DecodeSubType(word);
		context.Detail("word=0x%llx decoded typeex=%u subtype=%u; accessors typeex=%u subtype=%u",
			static_cast<unsigned long long>(word), type_ex, sub_type,
			static_cast<unsigned>(root->GetTypeEx()), static_cast<unsigned>(root->GetSubType()));
		if (type_ex != static_cast<unsigned>(root->GetTypeEx())
			|| sub_type != static_cast<unsigned>(root->GetSubType()))
		{
			context.Fail("the accessors do not read the bits the header's bitfield order puts them in");
		}
		if (root->GetType() >= KV3_TYPE_COUNT)
		{
			context.Fail("GetType() %d is outside the declared KV3Type_t range",
				static_cast<int>(root->GetType()));
		}
	});

	if (!root->IsTable())
	{
		context.SkipCall("KeyValues3 member walk", "the engine did not produce a table");
	}
	else
	{
		CS2ST_CALL(context, "KeyValues3 GetMemberCount vs GetMember walk (engine-filled)", {
			const int count = root->GetMemberCount();
			int walked = 0;
			int named = 0;
			int round_tripped = 0;
			for (KV3MemberId_t id = 0; id < count && id < kMaxMembersWalked; ++id)
			{
				KeyValues3* member = root->GetMember(id);
				if (member == nullptr)
				{
					break;
				}
				++walked;

				const char* name = root->GetMemberName(id);
				if (name != nullptr && name[0] != '\0')
				{
					++named;
					if (root->FindMember(CKV3MemberName(name)) == member)
					{
						++round_tripped;
					}
				}
				if (member->GetType() >= KV3_TYPE_COUNT)
				{
					context.Fail("member %d has type %d, outside KV3Type_t", id,
						static_cast<int>(member->GetType()));
					break;
				}
			}
			context.Detail("count=%d walked=%d named=%d found-by-name=%d", count, walked, named,
				round_tripped);
			if (count < 0 || count > kMaxMembersWalked)
			{
				context.Fail("GetMemberCount() %d is not a count", count);
			}
			else if (walked != count)
			{
				context.Fail("GetMemberCount() says %d but GetMember() ends after %d", count, walked);
			}
			if (round_tripped != named)
			{
				context.Fail("%d names did not come back through FindMember(name)",
					named - round_tripped);
			}
		});

		CS2ST_CALL(context, "KeyValues3 member names and types (values are not printed)", {
			const int count = root->GetMemberCount();
			for (KV3MemberId_t id = 0; id < count && id < kMaxNamesPrinted; ++id)
			{
				const KeyValues3* member = root->GetMember(id);
				context.Detail("[%d] \"%.40s\" : %s", id, root->GetMemberName(id),
					member != nullptr ? member->GetTypeAsString() : "<null>");
			}
		});

		CS2ST_CALL(context, "KeyValues3::ToString of the engine-filled table", {
			CBufferStringN<256> text(false);
			root->ToString(text);
			context.Detail("length=%d overflowed=%d (text itself is not printed)", text.Length(),
				text.IsOverflowed() ? 1 : 0);
			if (text.Length() == 0)
			{
				context.Fail("a table with members serialised to an empty string");
			}
		});
	}

	{
		StructInspector inspect(context, "KeyValues3", root, sizeof(KeyValues3));
		inspect.Raw("bitfield word (ctx/free/TypeEx/SubType/count/flags)", 0, 8);
		if (root->IsTable() || root->IsArray())
		{
			inspect.Pointer("m_Data (table/array pointer)", PrivateFieldOffset<kKv3Data>(root));
		}
		else
		{
			inspect.Raw("m_Data (type is not a container)", PrivateFieldOffset<kKv3Data>(root), 8);
		}
		inspect.Summarize();
		inspect.HexDump();
	}
}

void KeyValues3Suite::ExerciseLocal(TestContext& context)
{
	context.Note("[KeyValues3 local] self-consistency only: built and read by the same compiled code");

	CS2ST_CALL(context, "KeyValues3 scalar round trip (local; self-consistency only)", {
		KeyValues3* kv = MakeLeakedKv3();

		kv->SetInt(-42);
		const bool int_ok = kv->GetInt() == -42 && kv->GetType() == KV3_TYPE_INT;
		kv->SetUInt64(0x1122334455667788ull);
		const bool u64_ok = kv->GetUInt64() == 0x1122334455667788ull && kv->GetType() == KV3_TYPE_UINT;
		kv->SetBool(true);
		const bool bool_ok = kv->GetBool() && kv->GetType() == KV3_TYPE_BOOL;
		kv->SetDouble(2.5);
		const bool dbl_ok = std::fabs(kv->GetDouble() - 2.5) < 1e-12 && kv->GetType() == KV3_TYPE_DOUBLE;
		kv->SetToNull();
		const bool null_ok = kv->IsNull();

		context.Detail("int=%d u64=%d bool=%d double=%d null=%d", int_ok, u64_ok, bool_ok, dbl_ok, null_ok);
		if (!int_ok || !u64_ok || !bool_ok || !dbl_ok || !null_ok)
		{
			context.Fail("a scalar did not read back what was set");
		}
	});

	CS2ST_CALL(context, "KeyValues3 string, array and table round trip (local; self-consistency only)", {
		KeyValues3* kv = MakeLeakedKv3();

		kv->SetString("cs2sdktest");
		const bool string_ok = std::strcmp(kv->GetString(), "cs2sdktest") == 0
			&& kv->GetType() == KV3_TYPE_STRING;

		kv->SetToEmptyArray();
		for (int i = 0; i < 3; ++i)
		{
			kv->ArrayAddElementToTail()->SetInt(i + 100);
		}
		const bool array_ok = kv->IsArray() && kv->GetArrayElementCount() == 3
			&& kv->GetArrayElement(2) != nullptr && kv->GetArrayElement(2)->GetInt() == 102;

		kv->SetToEmptyTable();
		kv->SetMemberInt("alpha", 1);
		kv->SetMemberString("beta", "two");
		const bool table_ok = kv->IsTable() && kv->GetMemberCount() == 2
			&& kv->GetMemberInt("alpha") == 1
			&& std::strcmp(kv->GetMemberString("beta"), "two") == 0
			&& kv->FindMember("gamma") == nullptr;

		context.Detail("string=%d array=%d table=%d", string_ok, array_ok, table_ok);
		if (!string_ok || !array_ok || !table_ok)
		{
			context.Fail("a container did not read back what was set");
		}
	});
}

} // namespace cs2st

#else // CS2SDKTEST_WITH_KEYVALUES3

namespace cs2st
{

bool KeyValues3Suite::Acquire(TestContext& context)
{
	context.UnavailableCall("KeyValues3",
		"tier0_perproject/keyvalues3.cpp is not in this build (CS2SDKTEST_TIER1_KEYVALUES3 is off)");
	return false;
}

void KeyValues3Suite::ExerciseCalls(TestContext&) {}
void KeyValues3Suite::ExerciseEngineFilled(TestContext&) {}
void KeyValues3Suite::ExerciseLocal(TestContext&) {}

} // namespace cs2st

#endif // CS2SDKTEST_WITH_KEYVALUES3
