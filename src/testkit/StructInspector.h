/**
 * Checks a struct's memory against the layout the SDK believes it has.
 *
 * The trick this is built on: for several C++ types the set of legal values is
 * far smaller than the set of values their bytes can hold.
 *
 *     bool     1 byte,  256 possible,   2 legal  (0 or 1)
 *     pointer  8 bytes, 2^64 possible,  null or a canonical mapped address
 *     float    4 bytes, 2^32 possible,  finite values only - NaN/Inf is a
 *                                       large, easily-hit chunk of the encoding
 *     enum     4 bytes, 2^32 possible,  usually three or four
 *
 * So a field whose value falls outside its own type's legal range proves the
 * offset is not that field. Nothing else is needed to reach that conclusion -
 * no baseline file, no schema, no knowledge of which game build is running.
 * The judgement comes from the C++ type, which is why one implementation works
 * for every struct.
 *
 * That is also its limit. This is a falsifier, not a prover: all-green means
 * "no contradiction found", not "the layout is right". It is blind to two
 * same-typed fields swapping places, to almost anything involving ints (every
 * bit pattern is a legal int), and to fields appended past the end of what the
 * SDK declares.
 *
 * Signal strength is cumulative. One bool passing means little - random bytes
 * clear it about 1 time in 128. A dozen bools, pointers and floats all passing
 * is a different matter. And because misalignment accumulates, the *first*
 * failing offset is the interesting one: everything after it is downstream of
 * whatever Valve inserted there.
 *
 * Offsets must come from offsetof() rather than being written out as numbers.
 * What is being compared is "the layout the SDK assumes" against "the memory
 * the game handed us", so the assumption has to be computed from the SDK
 * headers - a hand-copied 0x54 silently stops tracking a submodule bump.
 */

#ifndef CS2SDKTEST_TESTKIT_STRUCTINSPECTOR_H_
#define CS2SDKTEST_TESTKIT_STRUCTINSPECTOR_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "testkit/TestContext.h"

namespace cs2st
{

/** Per-field qualifiers that change how a violation is reported. */
enum FieldFlags : unsigned
{
	kFieldNone = 0,

	/**
	 * The engine may never write this field.
	 *
	 * Several SDK structs have empty constructors - CGlobalVarsBase's is
	 * literally `{}` - so a field the engine does not use holds whatever the
	 * allocator left there. A value outside the type's range then says nothing
	 * about the layout, and reporting it as a layout failure trains people to
	 * ignore the check.
	 *
	 * Marked fields report SUSPECT instead of FAIL. The check is not relaxed:
	 * the value is still printed and still counted, so if this field ever
	 * starts holding something new the row still changes.
	 *
	 * Only mark a field when the surrounding fields prove the layout is fine -
	 * an isolated violation between two correct fields cannot be misalignment,
	 * because misalignment does not repair itself.
	 */
	kMayBeUninitialized = 1u << 0,
};

class StructInspector
{
public:
	/**
	 * @param type_name  Name used in the log, e.g. "CGlobalVars".
	 * @param base       Start of the instance to inspect.
	 * @param size       sizeof() as the SDK declares it. Note this may itself
	 *                   be wrong if the game grew the struct - reads are bounds
	 *                   checked against it, but the bound is an assumption too.
	 */
	StructInspector(TestContext& context, const char* type_name,
	                const void* base, std::size_t size);

	// ---- field checks ------------------------------------------------------

	/** The raw byte must be exactly 0 or 1. The sharpest probe available. */
	void Bool(const char* field, std::size_t offset, unsigned flags = kFieldNone);

	/**
	 * Null, or an address that could plausibly be one: canonical, past the
	 * null guard region, and 8-aligned because object pointers point at
	 * aligned objects.
	 */
	void Pointer(const char* field, std::size_t offset, unsigned flags = kFieldNone);

	/**
	 * A pointer to a C string: plausible pointer, and when non-null the bytes
	 * it points at must read as printable and terminated. Stronger than
	 * Pointer() - three conditions instead of one.
	 *
	 * Note this does *not* require alignment. String literals sit packed in
	 * .rodata at whatever byte they land on, so an unaligned char* is normal
	 * where an unaligned object pointer would be suspicious.
	 */
	void String(const char* field, std::size_t offset, unsigned flags = kFieldNone);

	/** Must be finite, and of a magnitude a game value could actually have. */
	void Float(const char* field, std::size_t offset, unsigned flags = kFieldNone);
	void Double(const char* field, std::size_t offset, unsigned flags = kFieldNone);

	/**
	 * Integers have no general-purpose check - every bit pattern is a legal
	 * int - so the no-range overloads only report the value. Pass a range only
	 * where the *type* implies one (a count cannot be negative), not where the
	 * game's semantics do; the moment ranges encode semantics this stops being
	 * reusable.
	 */
	void Int32(const char* field, std::size_t offset);
	void Int32(const char* field, std::size_t offset, std::int64_t min, std::int64_t max);
	void UInt32(const char* field, std::size_t offset);
	void Int64(const char* field, std::size_t offset);
	void UInt64(const char* field, std::size_t offset);

	/** Value must fall inside the declared enumerator range. */
	void Enum(const char* field, std::size_t offset, std::int64_t min, std::int64_t max);

	/** Reports the bytes without judging them; for unidentified padding. */
	void Raw(const char* field, std::size_t offset, std::size_t width);

	// ---- output ------------------------------------------------------------

	/**
	 * Prints the instance as hex plus ASCII, for eyeballing a misalignment.
	 *
	 * Dumps `extra_bytes` *past* what the SDK thinks sizeof is, on purpose:
	 * a struct that grew is invisible if you only ever look at the bytes the
	 * old declaration covers. Those trailing bytes are a deliberate
	 * out-of-bounds read - they are fetched through the fault guard as a
	 * separate step, so if the instance really does end there the dump loses
	 * its tail instead of the server losing its process.
	 *
	 * The boundary is marked in the output; rows past it carry a suffix.
	 */
	void HexDump(std::size_t extra_bytes = 64, std::size_t max_bytes = 1024);

	/** Prints the tally and points at the first failing offset. */
	void Summarize();

	std::size_t checked() const { return checked_; }
	std::size_t failed() const { return failed_; }
	std::size_t suspect() const { return suspect_; }

private:
	std::string Target(const char* field) const;
	const unsigned char* BytesAt(std::size_t offset) const;
	bool InBounds(const std::string& target, std::size_t offset, std::size_t width);

	/** Routes a violation to Fail or Suspect depending on the field's flags. */
	void Violation(unsigned flags, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/** Runs one field check through the guard and updates the tallies. */
	void Check(const std::string& target, std::size_t offset, std::size_t width,
	           const std::function<void()>& body);

	TestContext& context_;
	const char* type_name_;
	const void* base_;
	std::size_t size_;

	std::size_t checked_ = 0;
	std::size_t failed_ = 0;
	std::size_t suspect_ = 0;
	std::size_t first_failure_offset_ = SIZE_MAX;
	std::string first_failure_field_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_STRUCTINSPECTOR_H_ */
