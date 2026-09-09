/**
 * Getting offsetof-grade offsets for private members.
 *
 * The problem this exists for. Checking a struct's layout means comparing two
 * things: the offset the SDK's declaration implies, and the bytes the game
 * actually put there. Reading the bytes is never the hard part - any address
 * plus any number will read memory. The hard part is the *offset*, and it has
 * to be computed from the SDK headers rather than written down, or the check
 * stops tracking the headers the moment somebody updates the submodule (see
 * the note at the top of StructInspector.h).
 *
 * For a public member `offsetof` supplies that. For a protected one a derived
 * class that adds no members can name it, so `offsetof` still works. For a
 * private one neither applies - and several SDK structs are entirely private,
 * ConVarData among them, with sixteen fields and not one public data member to
 * anchor on.
 *
 * What makes it possible anyway is a deliberate hole in the access rules:
 *
 *   [temp.spec]/6 - "The usual access checking rules do not apply to names
 *   used to specify explicit instantiations."
 *
 * So `&SomeClass::m_private` is ill-formed almost everywhere, but well-formed
 * inside an explicit instantiation. Capture the pointer-to-member there, stash
 * it in a static, and later turn it into an offset by taking the address of
 * the member through it and subtracting the object base. Every step is
 * standard C++ - no #define private public, no reinterpret_cast of a
 * member-pointer, no undefined behaviour - and the offset comes from the
 * header declaration, which is the whole requirement.
 *
 * Usage, at namespace scope (see the note on the macro about *which*
 * namespace):
 *
 *     CS2ST_PRIVATE_FIELD(TagName, ConVarData, const char*, m_pszName);
 *
 *     // later, with any live instance:
 *     const std::size_t offset = PrivateFieldOffset<TagName>(data);
 *     inspect.String("m_pszName", offset);
 *
 * Two limitations worth knowing:
 *
 *   - It needs a live object. The offset is measured by differencing two
 *     addresses, which is defined behaviour only for a real instance; there is
 *     no null-pointer trick here.
 *   - It cannot name an array member cleanly (a pointer to `uint8[64]` is
 *     awkward and buys nothing). Array fields are better reached through
 *     whatever accessor returns their address - ConVarData::Value() does
 *     exactly that for m_Values.
 */

#ifndef CS2SDKTEST_TESTKIT_PRIVATEFIELD_H_
#define CS2SDKTEST_TESTKIT_PRIVATEFIELD_H_

#include <cstddef>

namespace cs2st
{

/**
 * Where a captured pointer-to-member lives.
 *
 * One instantiation per tag, so one static per field being reached.
 */
template <typename Tag>
struct PrivateFieldStash
{
	typedef typename Tag::MemberPtr MemberPtr;
	static MemberPtr pointer;
};

template <typename Tag>
typename PrivateFieldStash<Tag>::MemberPtr PrivateFieldStash<Tag>::pointer;

/**
 * The capture itself.
 *
 * Explicitly instantiating this is what suspends access checking on the
 * template argument. Instantiating the class also instantiates its static data
 * member, whose constructor runs during static initialisation and files the
 * pointer away - which is why nothing has to name the private member again at
 * the point of use.
 */
template <typename Tag, typename Tag::MemberPtr Ptr>
struct PrivateFieldBind
{
	PrivateFieldBind()
	{
		PrivateFieldStash<Tag>::pointer = Ptr;
	}

	static PrivateFieldBind instance;
};

template <typename Tag, typename Tag::MemberPtr Ptr>
PrivateFieldBind<Tag, Ptr> PrivateFieldBind<Tag, Ptr>::instance;

/**
 * The offset of the captured member within `object`.
 *
 * Both addresses come from the same live instance, so the subtraction is
 * defined. Returns SIZE_MAX if the pointer was never captured, which can only
 * happen if the explicit instantiation is missing.
 */
template <typename Tag>
std::size_t PrivateFieldOffset(const typename Tag::Owner* object)
{
	if (object == nullptr || PrivateFieldStash<Tag>::pointer == nullptr)
	{
		return static_cast<std::size_t>(-1);
	}

	const unsigned char* base = reinterpret_cast<const unsigned char*>(object);
	const unsigned char* field = reinterpret_cast<const unsigned char*>(
		&(object->*PrivateFieldStash<Tag>::pointer));

	return static_cast<std::size_t>(field - base);
}

} // namespace cs2st

/**
 * Declares a tag and captures `owner::member` through it.
 *
 * Must appear at namespace scope, and specifically in a namespace enclosing
 * cs2st - an explicit instantiation has to appear in an enclosing namespace of
 * its template, so `namespace cs2st { ... }` works and an anonymous namespace
 * nested inside it does not.
 *
 * `type` is the member's declared type; it must not contain a top-level comma
 * (wrap a multi-argument template in a typedef first).
 */
#define CS2ST_PRIVATE_FIELD(tag, owner, type, member) \
	struct tag \
	{ \
		typedef owner Owner; \
		typedef type Owner::*MemberPtr; \
	}; \
	template struct ::cs2st::PrivateFieldBind<tag, &owner::member>

#endif /* CS2SDKTEST_TESTKIT_PRIVATEFIELD_H_ */
