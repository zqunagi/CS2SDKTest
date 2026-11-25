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
 * worth printing.
 *
 * On MSVC x64 the first word of the pointer is the address of a small "vcall"
 * thunk the compiler generates for that function, and the slot is in its code:
 *
 *   mov rax, [rcx]            48 8B 01
 *   jmp [rax]                 FF 20               slot 0
 *   jmp [rax + disp8]         FF 60 dd
 *   jmp [rax + disp32]        FF A0 dd dd dd dd
 *
 * With incremental linking the pointer may first go through a one-instruction
 * `jmp rel32` (E9) stub, which is followed. A non-virtual member points at real
 * code, which does not start with that sequence, and gives -1.
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
#if defined(_MSC_VER)
	// One word for single inheritance, more for multiple/virtual; the code
	// address is the first word in every case.
	static_assert(sizeof(MemberFunctionPointer) >= sizeof(void*),
		"unexpected member function pointer layout");

	const unsigned char* code = nullptr;
	std::memcpy(&code, &function, sizeof(code));
	for (int hops = 0; hops < 4 && code != nullptr && code[0] == 0xE9; ++hops)
	{
		std::int32_t rel = 0;
		std::memcpy(&rel, code + 1, sizeof(rel));
		code = code + 5 + rel;
	}
	if (code == nullptr || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x01)
	{
		return -1;
	}
	code += 3;

	std::int32_t displacement = 0;
	if (code[0] == 0xFF && code[1] == 0x20)
	{
		displacement = 0;
	}
	else if (code[0] == 0xFF && code[1] == 0x60)
	{
		displacement = static_cast<std::int8_t>(code[2]);
	}
	else if (code[0] == 0xFF && code[1] == 0xA0)
	{
		std::memcpy(&displacement, code + 2, sizeof(displacement));
	}
	else
	{
		return -1;
	}
	return displacement / static_cast<std::int32_t>(sizeof(void*));
#else
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
#endif
}

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_VTABLEINDEX_H_ */
