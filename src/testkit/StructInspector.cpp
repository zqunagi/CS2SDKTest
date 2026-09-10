#include "testkit/StructInspector.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/Log.h"

namespace cs2st
{
namespace
{

/**
 * Values a float field in a game struct could actually hold. Times, ratios and
 * intervals all sit well inside this; a float read out of the wrong offset
 * lands outside it or is not finite at all.
 */
constexpr float kMaxPlausibleFloat = 1.0e9f;

/**
 * How far String() looks for a terminator before concluding the field is not a
 * char* at all.
 *
 * Deliberately much larger than the preview buffer, and unrelated to it: a
 * string being longer than what gets printed is not evidence of anything. Only
 * the absence of a terminator anywhere in this window is.
 */
constexpr std::size_t kMaxStringScan = 4096;

/** The first 64 KiB is the null guard region and is never mapped. */
constexpr std::uintptr_t kNullGuardLimit = 0x10000;

/**
 * User-space addresses on x86-64 stay under 2^47 on both platforms - the top
 * bits of a canonical user pointer are zero. Bytes that are really a float or
 * two packed ints almost always violate this.
 */
constexpr std::uintptr_t kUserAddressLimit = std::uintptr_t(1) << 47;

/**
 * Address plausibility, without any alignment requirement.
 *
 * @param require_alignment  True for object pointers, which point at aligned
 *                           objects. Must be false for char*: string literals
 *                           are packed into .rodata one after another and land
 *                           on arbitrary bytes, so demanding alignment there
 *                           rejects perfectly good pointers.
 */
bool LooksLikePointer(std::uintptr_t value, bool require_alignment)
{
	if (value == 0)
	{
		return true; // a null field is perfectly normal
	}
	if (value < kNullGuardLimit)
	{
		return false;
	}
	if (value >= kUserAddressLimit)
	{
		return false;
	}
	if (require_alignment && (value & 0x7) != 0)
	{
		return false;
	}
	return true;
}

bool IsPrintable(unsigned char ch)
{
	return ch >= 0x20 && ch < 0x7F;
}

/**
 * Names the byte as a well-known fill pattern, if it is one.
 *
 * A field holding 0xFF or 0xCD is far more likely to be memory nobody wrote
 * than a misread of real data, and saying so in the message saves the reader
 * from re-deriving it.
 */
const char* FillPatternNote(unsigned char byte)
{
	switch (byte)
	{
		case 0xFF:
		case 0xCC:
		case 0xCD:
		case 0xDD:
		case 0xAB:
		case 0xBA:
			return " - a common uninitialised-memory fill value";
		default:
			return "";
	}
}

} // namespace

StructInspector::StructInspector(TestContext& context, const char* type_name,
                                 const void* base, std::size_t size)
	: context_(context)
	, type_name_(type_name)
	, base_(base)
	, size_(size)
{
}

std::string StructInspector::Target(const char* field) const
{
	return std::string(type_name_) + "::" + field;
}

const unsigned char* StructInspector::BytesAt(std::size_t offset) const
{
	return static_cast<const unsigned char*>(base_) + offset;
}

bool StructInspector::InBounds(const std::string& target, std::size_t offset, std::size_t width)
{
	if (base_ == nullptr)
	{
		context_.UnavailableCall(target, "no instance to inspect");
		return false;
	}
	if (offset + width > size_)
	{
		context_.SkipCall(target, "@0x%02zx +%zu is past sizeof(%s)=%zu",
			offset, width, type_name_, size_);
		return false;
	}
	return true;
}

void StructInspector::Violation(unsigned flags, const char* format, ...)
{
	char message[512];
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	if (written < 0)
	{
		return;
	}

	if ((flags & kMayBeUninitialized) != 0)
	{
		context_.Suspect("%s (field may never be initialised)", message);
	}
	else
	{
		context_.Fail("%s", message);
	}
}

void StructInspector::Check(const std::string& target, std::size_t offset, std::size_t width,
                            const std::function<void()>& body)
{
	if (!InBounds(target, offset, width))
	{
		return;
	}

	++checked_;

	// Per field rather than per struct: if the pointer we were handed is
	// garbage, the first field faults and the rest still get their turn.
	const CallOutcome outcome = context_.RunCall(target, body);

	// kSuspect is deliberately not counted as a failure: it means the value is
	// explained, and letting it drive "first divergence" would point the
	// reader at a field that is fine.
	if (outcome != CallOutcome::kOk &&
		outcome != CallOutcome::kSkipped &&
		outcome != CallOutcome::kSuspect)
	{
		++failed_;
		if (offset < first_failure_offset_)
		{
			first_failure_offset_ = offset;
			first_failure_field_ = target;
		}
	}
	else if (outcome == CallOutcome::kSuspect)
	{
		++suspect_;
	}
}

// -----------------------------------------------------------------------------
//  Field checks
// -----------------------------------------------------------------------------

void StructInspector::Bool(const char* field, std::size_t offset, unsigned flags)
{
	const std::string target = Target(field);
	Check(target, offset, 1, [&]() {
		// Read as a byte, never as bool: reading it as bool lets the compiler
		// collapse any non-zero to true, destroying the evidence.
		const unsigned char raw = *BytesAt(offset);
		context_.Detail("@0x%02zx = %u", offset, raw);
		if (raw > 1)
		{
			Violation(flags,
				"a bool byte can only be 0 or 1; 0x%02x means this offset is not a bool%s",
				raw, FillPatternNote(raw));
		}
	});
}

void StructInspector::Pointer(const char* field, std::size_t offset, unsigned flags)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(void*), [&]() {
		std::uintptr_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		context_.Detail("@0x%02zx = 0x%016llx%s", offset,
			static_cast<unsigned long long>(value), value == 0 ? " (null)" : "");

		if (!LooksLikePointer(value, true))
		{
			Violation(flags, "not a plausible pointer");
		}
	});
}

void StructInspector::String(const char* field, std::size_t offset, unsigned flags)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(const char*), [&]() {
		std::uintptr_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		// Alignment is not required here - see the header. A string literal
		// lands wherever the linker packed it.
		if (!LooksLikePointer(value, false))
		{
			context_.Detail("@0x%02zx = 0x%016llx", offset,
				static_cast<unsigned long long>(value));
			Violation(flags, "not a plausible pointer");
			return;
		}
		if (value == 0)
		{
			context_.Detail("@0x%02zx = (null)", offset);
			return;
		}

		// Dereferencing is the actual test. If the pointer is plausible but
		// not mapped this faults, and the guard turns that into the finding.
		const char* text = reinterpret_cast<const char*>(value);
		char preview[64];
		std::size_t length = 0;
		bool terminated = false;
		bool printable = true;

		// Two lengths, not one, and conflating them was a real bug: how far the
		// scan looks for a terminator is the test, while the preview only decides
		// how much gets printed. Tying the first to the second reported every
		// string longer than the buffer as a broken layout - including help text
		// this plugin had registered itself and was reading back verbatim, which
		// is the strongest evidence available that the offset was right.
		//
		// The scan is still bounded. An offset that is not a char* points at
		// bytes with no terminator among them, and walking those unbounded is how
		// a probe stops being a probe.
		while (length < kMaxStringScan)
		{
			const unsigned char ch = static_cast<unsigned char>(text[length]);
			if (ch == 0)
			{
				terminated = true;
				break;
			}
			if (length < sizeof(preview) - 1)
			{
				// Printability is judged only over the bytes actually shown. A
				// long string that is legible for its first 63 characters is a
				// string; demanding the whole of it be printable would start
				// failing on embedded newlines and UTF-8 in engine help text.
				if (!IsPrintable(ch))
				{
					printable = false;
				}
				preview[length] = static_cast<char>(ch);
			}
			++length;
		}
		preview[length < sizeof(preview) - 1 ? length : sizeof(preview) - 1] = '\0';

		context_.Detail("@0x%02zx -> \"%s\"%s (%zu bytes%s)", offset, preview,
			length >= sizeof(preview) ? "..." : "", length,
			terminated ? "" : " with no terminator yet");

		if (!printable)
		{
			Violation(flags, "contains non-printable bytes");
		}
		else if (!terminated)
		{
			Violation(flags, "no terminator in the first %zu bytes", kMaxStringScan);
		}
	});
}

void StructInspector::Float(const char* field, std::size_t offset, unsigned flags)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(float), [&]() {
		float value = 0.0f;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		context_.Detail("@0x%02zx = %g", offset, static_cast<double>(value));

		if (!std::isfinite(value))
		{
			Violation(flags, "not a finite float");
		}
		else if (std::fabs(value) > kMaxPlausibleFloat)
		{
			Violation(flags, "magnitude is implausible for a game value");
		}
	});
}

void StructInspector::Double(const char* field, std::size_t offset, unsigned flags)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(double), [&]() {
		double value = 0.0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		context_.Detail("@0x%02zx = %g", offset, value);

		if (!std::isfinite(value))
		{
			Violation(flags, "not a finite double");
		}
	});
}

void StructInspector::Int32(const char* field, std::size_t offset)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::int32_t), [&]() {
		std::int32_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));
		// Reported, not judged: every bit pattern is a legal int32.
		context_.Detail("@0x%02zx = %d", offset, value);
	});
}

void StructInspector::Int32(const char* field, std::size_t offset,
                            std::int64_t min, std::int64_t max)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::int32_t), [&]() {
		std::int32_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		context_.Detail("@0x%02zx = %d", offset, value);

		if (value < min || value > max)
		{
			context_.Fail("outside the range this field's type allows (%lld..%lld)",
				static_cast<long long>(min), static_cast<long long>(max));
		}
	});
}

void StructInspector::UInt32(const char* field, std::size_t offset)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::uint32_t), [&]() {
		std::uint32_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));
		context_.Detail("@0x%02zx = %u", offset, value);
	});
}

void StructInspector::Int64(const char* field, std::size_t offset)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::int64_t), [&]() {
		std::int64_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));
		context_.Detail("@0x%02zx = %lld", offset, static_cast<long long>(value));
	});
}

void StructInspector::UInt64(const char* field, std::size_t offset)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::uint64_t), [&]() {
		std::uint64_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));
		context_.Detail("@0x%02zx = %llu", offset,
			static_cast<unsigned long long>(value));
	});
}

void StructInspector::Enum(const char* field, std::size_t offset,
                           std::int64_t min, std::int64_t max)
{
	const std::string target = Target(field);
	Check(target, offset, sizeof(std::int32_t), [&]() {
		std::int32_t value = 0;
		std::memcpy(&value, BytesAt(offset), sizeof(value));

		context_.Detail("@0x%02zx = %d", offset, value);

		if (value < min || value > max)
		{
			context_.Fail("outside the declared enumerator range (%lld..%lld)",
				static_cast<long long>(min), static_cast<long long>(max));
		}
	});
}

void StructInspector::Raw(const char* field, std::size_t offset, std::size_t width)
{
	const std::string target = Target(field);
	Check(target, offset, width, [&]() {
		char hex[3 * 16 + 8];
		std::size_t written = 0;
		const std::size_t shown = width < 16 ? width : 16;

		for (std::size_t i = 0; i < shown && written + 3 < sizeof(hex); ++i)
		{
			written += static_cast<std::size_t>(std::snprintf(
				hex + written, sizeof(hex) - written, "%02x ", BytesAt(offset)[i]));
		}
		if (written > 0)
		{
			hex[written - 1] = '\0';
		}
		else
		{
			hex[0] = '\0';
		}

		context_.Detail("@0x%02zx +%zu = %s%s", offset, width, hex,
			width > shown ? " ..." : "");
	});
}

// -----------------------------------------------------------------------------
//  Output
// -----------------------------------------------------------------------------

void StructInspector::HexDump(std::size_t extra_bytes, std::size_t max_bytes)
{
	if (base_ == nullptr)
	{
		return;
	}

	const std::size_t body = size_ < max_bytes ? size_ : max_bytes;

	// Copied out through the guard rather than read straight from the printer
	// loop, so a bad base pointer produces one recorded line instead of a
	// half-written dump and a dead process.
	std::vector<unsigned char> bytes(body, 0);
	bool body_read = false;
	context_.RunCall(Target("<hexdump>"), [&]() {
		std::memcpy(bytes.data(), BytesAt(0), body);
		body_read = true;
		context_.Detail("%zu byte(s) of %s", body, type_name_);
	});

	if (!body_read)
	{
		return;
	}

	// The bytes past sizeof are the point of the exercise: a field appended by
	// the game is invisible from inside the SDK's declaration. Reading them is
	// knowingly out of bounds, so it gets its own guarded step - the instance
	// really might end exactly there.
	std::size_t tail = 0;
	if (extra_bytes > 0 && body == size_)
	{
		std::vector<unsigned char> extra(extra_bytes, 0);
		bool tail_read = false;
		context_.RunCall(Target("<hexdump past end>"), [&]() {
			std::memcpy(extra.data(), BytesAt(size_), extra_bytes);
			tail_read = true;
			context_.Detail("%zu byte(s) beyond sizeof - compare after an update",
				extra_bytes);
		});

		if (tail_read)
		{
			bytes.insert(bytes.end(), extra.begin(), extra.end());
			tail = extra_bytes;
		}
	}

	const std::size_t count = bytes.size();

	LogRaw("    %s @ %p, sizeof = %zu (0x%zx)%s%s\n",
		type_name_, base_, size_, size_,
		body < size_ ? ", dump truncated" : "",
		tail > 0 ? ", plus bytes past the end" : "");

	for (std::size_t row = 0; row < count; row += 16)
	{
		char hex[16 * 3 + 2] = {0};
		char ascii[17] = {0};
		std::size_t hex_written = 0;

		for (std::size_t i = 0; i < 16; ++i)
		{
			if (row + i >= count)
			{
				hex_written += static_cast<std::size_t>(std::snprintf(
					hex + hex_written, sizeof(hex) - hex_written, "   "));
				ascii[i] = ' ';
				continue;
			}

			const unsigned char byte = bytes[row + i];
			hex_written += static_cast<std::size_t>(std::snprintf(
				hex + hex_written, sizeof(hex) - hex_written, "%02x ", byte));
			ascii[i] = IsPrintable(byte) ? static_cast<char>(byte) : '.';
		}

		// Marked per row rather than with a divider line: sizeof is rarely a
		// multiple of 16, so a divider would sit in the middle of a row and
		// imply a boundary that is not there.
		const char* marker = "";
		if (tail > 0 && row + 16 > body)
		{
			marker = (row >= body) ? "  << past sizeof" : "  << crosses sizeof";
		}

		LogRaw("    %04zx  %s %s%s\n", row, hex, ascii, marker);
	}
}

void StructInspector::Summarize()
{
	if (failed_ == 0)
	{
		context_.Note("%s: %zu field(s) checked, %zu suspect, no contradictions "
		              "(does not prove the layout is right)",
			type_name_, checked_, suspect_);
		return;
	}

	context_.Note("%s: %zu of %zu field(s) failed, %zu suspect",
		type_name_, failed_, checked_, suspect_);

	// Misalignment accumulates, so the first bad offset is where the layout
	// actually diverged; everything after it is fallout from the same cause.
	if (first_failure_offset_ != SIZE_MAX)
	{
		context_.Note("first divergence at @0x%02zx (%s) - fields after it are "
		              "suspect regardless of whether they passed",
			first_failure_offset_, first_failure_field_.c_str());
	}
}

} // namespace cs2st
