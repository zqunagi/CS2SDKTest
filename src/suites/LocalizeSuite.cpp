#include "suites/LocalizeSuite.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <appframework/iappsystem.h>
#include <localize/ilocalize.h>
#include <tier0/buffer_string.h>

#include "core/Log.h"

namespace cs2st
{
namespace
{

// Bounds for everything that walks something the engine counts: a garbage
// index chain must end the loop, not the process.
constexpr int kMaxTokensSampled = 4;
constexpr int kMaxInstancesWalked = 16;
constexpr int kMaxFilesListed = 8;

// A fixed instant for the date rows, so a diff never differs by wall clock.
// 2001-09-09 01:46:40 UTC.
constexpr uint64 kFixedTime = 1000000000ull;

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

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

// Measures a string by its length: enough for FindLongestInstance to have an
// answer, and it touches nothing.
class LengthQuery : public ILocalizeTextQuery
{
public:
	int ComputeTextWidth(const char* pString) override
	{
		return pString != nullptr ? static_cast<int>(std::strlen(pString)) : 0;
	}
};

// ConstructStringVArgs takes a va_list, which a test body cannot build on its
// own, so it goes through a variadic wrapper.
const char* CallConstructStringVArgs(ILocalize* localize, CBufferString* out,
	const char* format, int count, ...)
{
	va_list args;
	va_start(args, count);
	const char* result = localize->ConstructStringVArgs(out, format, count, args, false);
	va_end(args);
	return result;
}

// Counts what the engine tells it. Static storage: the engine keeps the
// pointer from InstallChangeCallback until RemoveChangeCallback.
class CountingCallback : public ILocalizationChangeCallback
{
public:
	void OnLocalizationChanged(CUtlVector<CUtlString>*) override { ++calls; }
	int calls = 0;
};

CountingCallback g_change_callback;

// A token the table really holds, to probe the by-name routes with. Filled by
// ExerciseTokenSamples from the first string the engine lists.
struct TokenSample
{
	LocalizeStringIndex_t index;
	char name[128];
};

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool LocalizeSuite::Acquire(TestContext& context)
{
	localize_ = static_cast<ILocalize*>(
		context.QueryInterface("localize", LOCALIZE_INTERFACE_VERSION));

	if (localize_ == nullptr)
	{
		const Module& localize_module = context.GetModule("localize");
		if (!localize_module.IsValid())
		{
			context.UnavailableCall("ILocalize", "the localize module is not loaded");
		}
		else
		{
			context.UnavailableCall("ILocalize",
				"localize is loaded but does not answer '%s'", LOCALIZE_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(localize_),
		LOCALIZE_INTERFACE_VERSION);
	return true;
}

void LocalizeSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseLanguage(context);
	ExerciseLookup(context);
	ExerciseTokenSamples(context);
	ExerciseFormatting(context);
	ExerciseFiles(context);
	ExerciseGuardedWrites(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = localize_->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		context.Detail("-> %s", localize_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = localize_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = localize_->GetDependencies();
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
		for (int i = 0; i < count && i < 16; ++i)
		{
			context.Detail("   [%d] module \"%s\", interface \"%s\"", i,
				deps[i].m_pModuleName, Str(deps[i].m_pInterfaceName));
		}
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface (unknown name)", {
		void* none = localize_->QueryInterface("CS2SDKTestNoSuchInterface001");
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
//  Language and flag state
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseLanguage(TestContext& context)
{
	context.Note("[ILocalize] language and flag state");

	CS2ST_CALL(context, "ILocalize::GetCurrentLanguage / GetCurrentELanguage", {
		const char* name = localize_->GetCurrentLanguage();
		const ELanguage language = localize_->GetCurrentELanguage();
		context.Detail("-> \"%s\", ELanguage %d", Str(name), static_cast<int>(language));
		// The enum is a closed range; k_Lang_None is a legal "unset" answer.
		if (static_cast<int>(language) < static_cast<int>(k_Lang_None)
			|| static_cast<int>(language) >= static_cast<int>(k_Lang_MAX))
		{
			context.Fail("ELanguage %d is outside k_Lang_None..k_Lang_MAX", static_cast<int>(language));
		}
		if (name == nullptr)
		{
			context.Suspect("GetCurrentLanguage returned null");
		}
	});

	CS2ST_CALL(context, "ILocalize::GetCurrentSubLanguage", {
		context.Detail("-> \"%s\"", Str(localize_->GetCurrentSubLanguage()));
	});

	CS2ST_CALL(context, "ILocalize::GetLinguisticCaseFlags", {
		context.Detail("-> %d", localize_->GetLinguisticCaseFlags());
	});

	CS2ST_CALL(context, "ILocalize::GetLocalizeFlags", {
		context.Detail("-> 0x%x", static_cast<unsigned>(localize_->GetLocalizeFlags()));
	});

	CS2ST_CALL(context, "ILocalize::GetTrackingMode", {
		// Raw byte: a bool the engine returns in al can carry stray high bits.
		const bool tracking = localize_->GetTrackingMode();
		context.Detail("-> %s", tracking ? "true" : "false");
	});

	CS2ST_CALL(context, "ILocalize::GetFallbackImpl", {
		ILocalize* fallback = localize_->GetFallbackImpl();
		context.Detail("-> %p", static_cast<void*>(fallback));
		if (fallback == localize_)
		{
			context.Suspect("the fallback implementation is the object itself");
		}
	});

	CS2ST_CALL(context, "ILocalize::GetStatsString", {
		CBufferStringN<512> stats(false);
		localize_->GetStatsString(&stats);
		context.Detail("-> \"%s\"", stats.Get());
	});

	CS2ST_CALL(context, "ILocalize::DoKoreanJongSungRuleInPlace", {
		// Our own buffer, edited in place. ASCII has no jongsung, so the text
		// should come back unchanged; a change is only worth a Suspect.
		char text[32] = "plain ascii";
		const bool changed = localize_->DoKoreanJongSungRuleInPlace(text, static_cast<int>(sizeof(text)));
		context.Detail("-> %s, \"%s\"", changed ? "true" : "false", text);
		if (std::strcmp(text, "plain ascii") != 0)
		{
			context.Suspect("an ASCII string was rewritten by the Korean jongsung rule");
		}
	});
}

// -----------------------------------------------------------------------------
//  Lookup by an unknown token
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseLookup(TestContext& context)
{
	context.Note("[ILocalize] lookups of a token nobody registered - the Safe forms must answer, the Unsafe forms must be null");

	static const char* const kNoSuchToken = "#CS2SDKTest_NoSuchToken";

	CS2ST_CALL(context, "ILocalize::FindUnsafe / FindUnsafeLocal (unknown token)", {
		const char* shared = localize_->FindUnsafe(kNoSuchToken);
		const char* local = localize_->FindUnsafeLocal(kNoSuchToken);
		context.Detail("-> FindUnsafe \"%s\", FindUnsafeLocal \"%s\"", Str(shared), Str(local));
		if (shared != nullptr || local != nullptr)
		{
			context.Suspect("a token nobody registered resolved to text");
		}
	});

	CS2ST_CALL(context, "ILocalize::FindSafe (unknown token)", {
		// "returns an error message instead of NULL" is the header's promise.
		const char* text = localize_->FindSafe(kNoSuchToken);
		context.Detail("-> \"%s\"", Str(text));
		if (text == nullptr)
		{
			context.Fail("FindSafe returned null, the header says it returns an error message instead");
		}
	});

	CS2ST_CALL(context, "ILocalize::FindSubLanguageUnsafe / FindSubLanguageSafe (unknown token)", {
		const char* unsafe_text = localize_->FindSubLanguageUnsafe(kNoSuchToken);
		const char* safe_text = localize_->FindSubLanguageSafe(kNoSuchToken);
		context.Detail("-> unsafe \"%s\", safe \"%s\"", Str(unsafe_text), Str(safe_text));
		if (unsafe_text != nullptr)
		{
			context.Suspect("a token nobody registered resolved in the sub-language table");
		}
	});

	CS2ST_CALL(context, "ILocalize::FindIndex (unknown token)", {
		const LocalizeStringIndex_t index = localize_->FindIndex(kNoSuchToken);
		context.Detail("-> %d", static_cast<int>(index.GetRaw()));
		if (index != LOCALIZE_INVALID_STRING_INDEX)
		{
			context.Fail("an unregistered token has index %d, expected LOCALIZE_INVALID_STRING_INDEX",
				static_cast<int>(index.GetRaw()));
		}
	});

	CS2ST_CALL(context, "ILocalize::GetPrimaryInstanceByToken (unknown token)", {
		const LocalizeInstanceIndex_t instance =
			localize_->GetPrimaryInstanceByToken(kNoSuchToken, k_Lang_None);
		context.Detail("-> %d", static_cast<int>(instance.GetRaw()));
		if (instance.GetRaw() != -1)
		{
			context.Suspect("an unregistered token has an instance index (%d)",
				static_cast<int>(instance.GetRaw()));
		}
	});

	CS2ST_CALL(context, "ILocalize::GetLocalizedString (plain text)", {
		// Not a token: the engine should hand the text back, in the buffer or by return.
		CBufferStringN<128> out(false);
		const char* text = localize_->GetLocalizedString("plain text", &out);
		context.Detail("-> return \"%s\", buffer \"%s\"", Str(text), out.Get());
	});
}

// -----------------------------------------------------------------------------
//  Lookup by a token the table holds
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseTokenSamples(TestContext& context)
{
	context.Note("[ILocalize] first %d tokens the table lists; each is read through independent routes", kMaxTokensSampled);

	LocalizeStringIndex_t first;
	CS2ST_CALL(context, "ILocalize::GetFirstStringIndex", {
		first = localize_->GetFirstStringIndex();
		context.Detail("-> %d", static_cast<int>(first.GetRaw()));
	});

	if (first == LOCALIZE_INVALID_STRING_INDEX)
	{
		context.Note("the table is empty (no localization file loaded); the per-token routes have nothing to read");
	}

	// Collected with GetNextStringIndex, which is the iteration under test.
	TokenSample samples[kMaxTokensSampled] = {};
	int sample_count = 0;
	LocalizeStringIndex_t cursor = first;
	while (cursor != LOCALIZE_INVALID_STRING_INDEX && sample_count < kMaxTokensSampled)
	{
		TokenSample& sample = samples[sample_count];
		sample.index = cursor;
		const char* name = localize_->GetNameByIndex(cursor);
		if (name == nullptr)
		{
			context.Suspect("GetNameByIndex(%d) is null for an index the iteration returned",
				static_cast<int>(cursor.GetRaw()));
			break;
		}
		std::snprintf(sample.name, sizeof(sample.name), "%s", name);
		++sample_count;
		cursor = localize_->GetNextStringIndex(cursor);
	}
	context.Detail("walked %d token(s)%s", sample_count,
		sample_count == kMaxTokensSampled ? " (capped)" : "");

	for (int i = 0; i < sample_count; ++i)
	{
		const TokenSample& sample = samples[i];
		char label[192];

		std::snprintf(label, sizeof(label),
			"ILocalize::FindIndex / GetNameByIndex [token %d \"%.48s\"]", i, sample.name);
		CS2ST_CALL(context, label, {
			// Name -> index must land on the index the name came from.
			const LocalizeStringIndex_t found = localize_->FindIndex(sample.name);
			const char* round_trip = localize_->GetNameByIndex(found);
			context.Detail("-> FindIndex %d (listed as %d), name \"%s\"",
				static_cast<int>(found.GetRaw()), static_cast<int>(sample.index.GetRaw()),
				Str(round_trip));
			if (found != sample.index)
			{
				context.Fail("FindIndex(\"%s\") is %d but the iteration listed it as %d",
					sample.name, static_cast<int>(found.GetRaw()),
					static_cast<int>(sample.index.GetRaw()));
			}
			else if (round_trip == nullptr || std::strcmp(round_trip, sample.name) != 0)
			{
				context.Fail("GetNameByIndex(FindIndex(\"%s\")) is \"%s\"", sample.name, Str(round_trip));
			}
		});

		std::snprintf(label, sizeof(label),
			"ILocalize::GetValueByIndex / FindUnsafe [token %d]", i);
		CS2ST_CALL(context, label, {
			// Two routes to the text: by index and by name. The by-name route
			// may pick a sub-language, so a disagreement is Suspect.
			const char* by_index = localize_->GetValueByIndex(sample.index);
			const char* by_name = localize_->FindUnsafe(sample.name);
			context.Detail("-> by index \"%.80s\", by name \"%.80s\"", Str(by_index), Str(by_name));
			if (by_name == nullptr)
			{
				context.Suspect("FindUnsafe found nothing for a token the table lists");
			}
			else if (by_index != nullptr && std::strcmp(by_index, by_name) != 0)
			{
				context.Suspect("GetValueByIndex and FindUnsafe return different text");
			}

			const char* safe = localize_->FindSafe(sample.name);
			if (by_name != nullptr && safe != by_name && (safe == nullptr || std::strcmp(safe, by_name) != 0))
			{
				context.Suspect("FindSafe and FindUnsafe return different text for a known token");
			}
		});

		std::snprintf(label, sizeof(label),
			"ILocalize::GetFileNameByIndex / unk201 [token %d]", i);
		CS2ST_CALL(context, label, {
			const char* file = localize_->GetFileNameByIndex(sample.index);
			const ELanguage language = localize_->unk201(sample.index);
			context.Detail("-> file \"%s\", unk201 -> ELanguage %d", Str(file), static_cast<int>(language));
		});

		std::snprintf(label, sizeof(label),
			"ILocalize::GetPrimaryInstanceByIndex / ByToken / GetNextInstanceIndex / GetValueByInstanceIndex [token %d]", i);
		CS2ST_CALL(context, label, {
			const LocalizeInstanceIndex_t by_index =
				localize_->GetPrimaryInstanceByIndex(sample.index, k_Lang_None);
			const LocalizeInstanceIndex_t by_token =
				localize_->GetPrimaryInstanceByToken(sample.name, k_Lang_None);
			context.Detail("-> by index %d, by token %d",
				static_cast<int>(by_index.GetRaw()), static_cast<int>(by_token.GetRaw()));
			if (by_index != by_token)
			{
				context.Fail("the same token has instance %d by index and %d by name",
					static_cast<int>(by_index.GetRaw()), static_cast<int>(by_token.GetRaw()));
			}

			// The instance chain, capped: a cycle must not be a hang.
			LocalizeInstanceIndex_t instance = by_index;
			int walked = 0;
			while (instance.GetRaw() != -1 && walked < kMaxInstancesWalked)
			{
				const char* value = localize_->GetValueByInstanceIndex(instance);
				context.Detail("   instance %d: \"%.60s\"", static_cast<int>(instance.GetRaw()), Str(value));
				instance = localize_->GetNextInstanceIndex(instance);
				++walked;
			}
			if (walked == kMaxInstancesWalked)
			{
				context.Suspect("the instance chain did not end within %d steps", kMaxInstancesWalked);
			}
		});

		std::snprintf(label, sizeof(label),
			"ILocalize::FindLongestInstance [token %d]", i);
		CS2ST_CALL(context, label, {
			LengthQuery query;
			LocalizeInstanceIndex_t instance;
			LocalizeStringIndex_t string_index;
			const char* longest = localize_->FindLongestInstance(sample.name, &query, &instance, &string_index);
			context.Detail("-> \"%.80s\", instance %d, string %d", Str(longest),
				static_cast<int>(instance.GetRaw()), static_cast<int>(string_index.GetRaw()));
			if (longest != nullptr && string_index != sample.index)
			{
				context.Suspect("FindLongestInstance reports string %d for a token listed as %d",
					static_cast<int>(string_index.GetRaw()), static_cast<int>(sample.index.GetRaw()));
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  Formatting
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseFormatting(TestContext& context)
{
	context.Note("[ILocalize] Construct*: every call writes only into a local buffer; the date is fixed so a diff never moves with the clock");

	CS2ST_CALL(context, "ILocalize::ConstructString (format string, no arguments)", {
		CBufferStringN<256> out(false);
		const char* text = localize_->ConstructString(&out, "plain text", 0);
		context.Detail("-> return \"%s\", buffer \"%s\"", Str(text), out.Get());
	});

	CS2ST_CALL(context, "ILocalize::ConstructStringArgArray", {
		CBufferStringN<256> out(false);
		const char* args[2];
		args[0] = "first";
		args[1] = "second";
		const char* text = localize_->ConstructStringArgArray(&out, "%s1 and %s2", 2, args, false);
		context.Detail("-> return \"%s\", buffer \"%s\"", Str(text), out.Get());
	});

	CS2ST_CALL(context, "ILocalize::ConstructStringVArgs", {
		CBufferStringN<256> out(false);
		const char* text = CallConstructStringVArgs(localize_, &out, "%s1 and %s2", 2, "first", "second");
		context.Detail("-> return \"%s\", buffer \"%s\"", Str(text), out.Get());
	});

	CS2ST_CALL(context, "ILocalize::ConstructDateString", {
		// Both overloads at the same instant: the uint64 and the tm form are
		// two routes to one fact once the tm is in the host time zone, so a
		// difference is only a Suspect.
		CBufferStringN<128> from_seconds(false);
		const char* by_value = localize_->ConstructDateString(&from_seconds, LOC_DATE_NEUTRAL_TIMESTAMP, kFixedTime, false);

		std::tm broken_down = {};
		const std::time_t seconds = static_cast<std::time_t>(kFixedTime);
		// Local time, not UTC: the server's own log showed the uint64 overload
		// converts through the host time zone (09:46:40 for 1000000000 on a
		// UTC+8 host), while the tm overload formats the fields as given.
		localtime_r(&seconds, &broken_down);
		CBufferStringN<128> from_tm(false);
		const char* by_tm = localize_->ConstructDateString(&from_tm, LOC_DATE_NEUTRAL_TIMESTAMP, &broken_down, false);

		context.Detail("-> uint64 \"%s\", tm \"%s\"", Str(by_value), Str(by_tm));
		if (by_value != nullptr && by_tm != nullptr && std::strcmp(by_value, by_tm) != 0)
		{
			context.Suspect("the uint64 and tm overloads format the same instant differently");
		}
	});

	CS2ST_CALL(context, "ILocalize::ConstructRelativeDateString / ConstructRelativeTimeString", {
		CBufferStringN<128> date(false);
		CBufferStringN<128> time(false);
		const char* date_text = localize_->ConstructRelativeDateString(&date, LOC_DATE_DAY_MONTH_YEAR, kFixedTime, false);
		const char* time_text = localize_->ConstructRelativeTimeString(&time, LOC_DATE_HOUR_MINUTE, LOC_DATE_DAY_MONTH_YEAR, kFixedTime, false);
		context.Detail("-> date \"%s\", time \"%s\"", Str(date_text), Str(time_text));
	});

	CS2ST_CALL(context, "ILocalize::ConstructDurationString", {
		CBufferStringN<128> out(false);
		const char* text = localize_->ConstructDurationString(&out, LOC_DURATION_HOURS_MINUTES_SECONDS, 3661, false);
		context.Detail("-> \"%s\" (3661 s)", Str(text));
	});

	CS2ST_CALL(context, "ILocalize::ConstructNumberString", {
		CBufferStringN<128> integral(false);
		CBufferStringN<128> floating(false);
		const char* integral_text = localize_->ConstructNumberString(&integral, LOC_NUMBER_UNSIGNED, static_cast<uint64>(1234567), false);
		const char* floating_text = localize_->ConstructNumberString(&floating, LOC_NUMBER_SIGNED, 1234.5, 2, false);
		context.Detail("-> integral \"%s\", floating \"%s\"", Str(integral_text), Str(floating_text));
	});

	CS2ST_CALL(context, "ILocalize::unk101 / unk102", {
		// Unidentified, but both take plain values and write only into our
		// buffers. Recorded, never judged: the point is that the log shows
		// when their behaviour changes.
		CBufferStringN<128> out(false);
		const char* text = localize_->unk101("plain text", &out);
		double value = 0.0;
		const bool parsed = localize_->unk102("1.5", &value, 0.0);
		context.Detail("-> unk101 \"%s\" (buffer \"%s\"), unk102 %s value %g",
			Str(text), out.Get(), parsed ? "true" : "false", value);
	});

	context.SkipCall("ILocalize::ConstructString (ILocVariableSource overload)", "needs an ILocVariableSource, an engine type with no safe stand-in");
	context.SkipCall("ILocalize::ConstructMFString", "needs an ILocVariableSource, an engine type with no safe stand-in");
	context.SkipCall("ILocalize::unk103", "unidentified, takes four plain arguments whose meaning is not known; calling it blind is not safe");
	context.SkipCall("ILocalize::CompileString", "allocates an ILocalizeCompiledString the SDK gives no way to free");
}

// -----------------------------------------------------------------------------
//  Files
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseFiles(TestContext& context)
{
	context.Note("[ILocalize] localization file list");

	CS2ST_CALL(context, "ILocalize::GetLocalizationFileCount / GetLocalizationFileName", {
		const int count = localize_->GetLocalizationFileCount();
		context.Detail("-> %d file(s)", count);
		if (count < 0)
		{
			context.Fail("GetLocalizationFileCount returned %d", count);
			return;
		}
		const int shown = count < kMaxFilesListed ? count : kMaxFilesListed;
		for (int i = 0; i < shown; ++i)
		{
			const char* name = localize_->GetLocalizationFileName(i);
			context.Detail("   [%d] \"%s\"", i, Str(name));
			if (name == nullptr)
			{
				context.Suspect("GetLocalizationFileName(%d) is null below the count %d", i, count);
			}
		}
	});

	CS2ST_CALL(context, "ILocalize::unk401", {
		// Takes a single uint32 and returns text; 0 is the least surprising argument.
		context.Detail("-> \"%s\"", Str(localize_->unk401(0)));
	});
}

// -----------------------------------------------------------------------------
//  Writes that put back what they found
// -----------------------------------------------------------------------------

void LocalizeSuite::ExerciseGuardedWrites(TestContext& context)
{
	context.Note("[ILocalize] setters called with the value they just read, so the table ends as it began; "
		"the one addition (a unique token) comes last");

	CS2ST_CALL(context, "ILocalize::SetCurrentSubLanguage (same value)", {
		const char* current = localize_->GetCurrentSubLanguage();
		char saved[64];
		std::snprintf(saved, sizeof(saved), "%s", Str(current));
		localize_->SetCurrentSubLanguage(saved);
		const char* after = localize_->GetCurrentSubLanguage();
		context.Detail("-> \"%s\" before, \"%s\" after", saved, Str(after));
		if (after == nullptr || std::strcmp(after, saved) != 0)
		{
			context.Suspect("SetCurrentSubLanguage with the current value changed it to \"%s\"", Str(after));
		}
	});

	CS2ST_CALL(context, "ILocalize::SetLocalizeFlags (same value)", {
		const uint32 flags = localize_->GetLocalizeFlags();
		localize_->SetLocalizeFlags(flags);
		const uint32 after = localize_->GetLocalizeFlags();
		context.Detail("-> 0x%x before, 0x%x after", static_cast<unsigned>(flags), static_cast<unsigned>(after));
		if (after != flags)
		{
			context.Fail("GetLocalizeFlags is 0x%x after setting 0x%x", static_cast<unsigned>(after),
				static_cast<unsigned>(flags));
		}
	});

	CS2ST_CALL(context, "ILocalize::SetTrackingMode (same value)", {
		const bool mode = localize_->GetTrackingMode();
		localize_->SetTrackingMode(mode);
		const bool after = localize_->GetTrackingMode();
		context.Detail("-> %s before, %s after", mode ? "true" : "false", after ? "true" : "false");
		if (after != mode)
		{
			context.Fail("GetTrackingMode changed after setting its own value");
		}
	});

	CS2ST_CALL(context, "ILocalize::SetFallbackImpl (same value)", {
		ILocalize* fallback = localize_->GetFallbackImpl();
		localize_->SetFallbackImpl(fallback);
		ILocalize* after = localize_->GetFallbackImpl();
		context.Detail("-> %p before, %p after", static_cast<void*>(fallback), static_cast<void*>(after));
		if (after != fallback)
		{
			context.Fail("GetFallbackImpl changed after setting its own value");
		}
	});

	CS2ST_CALL(context, "ILocalize::InstallChangeCallback / RemoveChangeCallback", {
		// Installed and removed back to back; the callback is static so the
		// engine never holds a dangling pointer, even if Remove did nothing.
		g_change_callback.calls = 0;
		localize_->InstallChangeCallback(&g_change_callback, 0);
		localize_->RemoveChangeCallback(&g_change_callback);
		context.Detail("-> installed and removed, %d notification(s) in between", g_change_callback.calls);
	});

	CS2ST_CALL(context, "ILocalize::SetValueByIndex (same value)", {
		const LocalizeStringIndex_t index = localize_->GetFirstStringIndex();
		if (index == LOCALIZE_INVALID_STRING_INDEX)
		{
			context.Detail("-> table is empty, nothing to write back");
			return;
		}
		const char* value = localize_->GetValueByIndex(index);
		if (value == nullptr)
		{
			context.Detail("-> first token has no value, nothing to write back");
			return;
		}
		// Copied first: the engine may free its own copy when the value is replaced.
		char saved[512];
		std::snprintf(saved, sizeof(saved), "%s", value);
		localize_->SetValueByIndex(index, saved);
		const char* after = localize_->GetValueByIndex(index);
		context.Detail("-> \"%.80s\" written back, reads \"%.80s\"", saved, Str(after));
		if (after == nullptr || std::strcmp(after, saved) != 0)
		{
			context.Fail("GetValueByIndex returns \"%s\" after writing the same text back", Str(after));
		}
	});

	CS2ST_CALL(context, "ILocalize::CheckForFileChanges", {
		// Nothing on disk has changed during the run, so this should be a no-op.
		localize_->CheckForFileChanges();
		context.Detail("-> returned");
	});

	CS2ST_CALL(context, "ILocalize::AddFile (missing file)", {
		const bool added = localize_->AddFile("resource/cs2sdktest_no_such_file.txt");
		context.Detail("-> %s", added ? "true" : "false");
		if (added)
		{
			context.Suspect("AddFile reports success for a file that does not exist");
		}
	});

	CS2ST_CALL(context, "ILocalize::SetCurrentSubLanguageFromSystem", {
		// Re-derives the sub-language from the host and sets it. Restored to
		// what it was, so the table resolves the same way afterwards.
		char saved[64];
		std::snprintf(saved, sizeof(saved), "%s", Str(localize_->GetCurrentSubLanguage()));
		const char* from_system = localize_->SetCurrentSubLanguageFromSystem();
		context.Detail("-> \"%s\" (was \"%s\")", Str(from_system), saved);
		localize_->SetCurrentSubLanguage(saved);
	});

	CS2ST_CALL(context, "ILocalize::ReadLocalizationBuffer (one new token)", {
		// The only call that adds to the table: one token whose name nobody
		// else uses, in the language already current. Nothing removes it, so
		// it is deliberately the last call that touches the table.
		static const char kToken[] = "CS2SDKTest_AddedToken";
		static const char kText[] =
			"\"lang\"\n{\n\"Language\" \"English\"\n\"Tokens\"\n{\n\"CS2SDKTest_AddedToken\" \"added by the test\"\n}\n}\n";
		const char* language = localize_->GetCurrentLanguage();
		char block[256];
		std::snprintf(block, sizeof(block), "%s", kText);
		const bool read = localize_->ReadLocalizationBuffer("cs2sdktest_buffer", block,
			static_cast<int>(std::strlen(block)), language != nullptr ? language : "english");
		const char* found = localize_->FindUnsafe(kToken);
		context.Detail("-> %s, token now reads \"%s\"", read ? "true" : "false", Str(found));
		if (read && found == nullptr)
		{
			context.Suspect("ReadLocalizationBuffer reported success but the token cannot be found");
		}
	});
}

// -----------------------------------------------------------------------------
//  What is deliberately not called
// -----------------------------------------------------------------------------

void LocalizeSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ILocalize] calls that change or reload the live table and have no way back");

	context.SkipCall("ILocalize::~ILocalize", "destroys the engine's table; occupies two vtable slots on Itanium ABIs");
	context.SkipCall("ILocalize::RemoveAll", "empties the live table");
	context.SkipCall("ILocalize::RemoveAllFileStrings", "removes every file-backed string from the live table");
	context.SkipCall("ILocalize::SaveToFile", "writes the whole table to a file");
	context.SkipCall("ILocalize::unk301", "unidentified, takes two strings and looks like a file operation; not safe blind");
	context.SkipCall("ILocalize::ReloadLocalizationFiles", "development-only reload of the live table");
	context.SkipCall("ILocalize::ReloadLocalizationFilesInLanguage", "reloads the live table");
	context.SkipCall("ILocalize::AddLocalizationKeyValues", "adds strings to the live table; needs a KeyValues / KeyValues3 tree");
	context.SkipCall("ILocalize::LocalizedMessageBox", "shows a message box");
	context.SkipCall("ILocalize::unk402", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk403", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk404", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk405", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk406", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk407", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk408", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::unk501", "unidentified, returns an ILocalize_EconProvider the SDK does not declare; not safe blind");
	context.SkipCall("ILocalize::unk502", "unidentified, no arguments and no known effect; not safe blind");
	context.SkipCall("ILocalize::RunInternalTest_LocVariableResolve", "the engine's own self-test; may assert or print");
}

} // namespace cs2st
