/**
 * The vtable slot a member function pointer names, decoded without calling or
 * hooking anything.
 *
 * On the Itanium ABI (every Linux build) a pointer to a virtual member function
 * is two words: the first is `1 + slot * sizeof(void*)`, the second a `this`
 * adjustment. A non-virtual member keeps its code address in the first word,
 * which is even, so the low bit tells the two apart. That is all this reads.
 *
 * GamedataSuite compares the slot a header declares with the slot gamedata
 * carries; two independent sources for one number is what makes the comparison
 * worth printing. It replaces KHook::GetVtableIndex, which did the same thing
 * but came with a header the project no longer carries.
 *
 * Linux (Itanium) only. The Windows encoding is different - a pointer to a
 * thunk that has to be disassembled - and nothing here targets it any more.
 */

#ifndef CS2SDKTEST_CORE_VTABLEINDEX_H_
#define CS2SDKTEST_CORE_VTABLEINDEX_H_

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cs2st
{

/** Slot number, or -1 when `function` is not a virtual member (or not decodable). */
template <typename MemberFunctionPointer>
inline std::int32_t VtableIndexOf(MemberFunctionPointer function)
{
	static_assert(std::is_member_function_pointer<MemberFunctionPointer>::value,
		"VtableIndexOf needs a pointer to a member function");
	static_assert(sizeof(MemberFunctionPointer) == 2 * sizeof(void*),
		"unexpected member function pointer layout; this decoder is Itanium-only");

	struct Words
	{
		std::intptr_t first;
		std::intptr_t adjustment;
	};
	Words words;
	std::memcpy(&words, &function, sizeof(words));

	if ((words.first & 1) == 0)
	{
		return -1;
	}
	return static_cast<std::int32_t>((words.first - 1) / static_cast<std::intptr_t>(sizeof(void*)));
}

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_VTABLEINDEX_H_ */
