#include "core/SignatureScan.h"

#include <cstdio>
#include <cstring>

namespace cs2st
{
namespace
{

/** How many matches to count before declaring the scan done. */
constexpr std::size_t kMaxMatches = 2;

int HexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

} // namespace

const char* ParseBytePattern(const std::string& text, BytePattern& out)
{
	out.bytes.clear();
	out.wildcard.clear();

	// The unit is the whitespace-separated token, not the character: one
	// token is one byte - "48" - or a wildcard, written "?" or "??", the two
	// spellings the ecosystem's gamedata files use for "any byte". A '?'
	// token is whole on its own, which is why wildcard state cannot share
	// the waiting-for-a-low-nibble flag hex digits use.
	std::size_t i = 0;
	while (true)
	{
		while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
		{
			++i;
		}
		if (i >= text.size())
		{
			break;
		}

		const std::size_t start = i;
		while (i < text.size() && text[i] != ' ' && text[i] != '\t')
		{
			++i;
		}
		const std::string token = text.substr(start, i - start);

		if (token == "?" || token == "??")
		{
			out.bytes.push_back(0);
			out.wildcard.push_back(true);
			continue;
		}
		if (token.find('?') != std::string::npos)
		{
			// "?3" and "3?" - a byte that is half wildcard is not a thing
			// any scanner in the ecosystem defines.
			return "'?' cannot be combined with a hex digit";
		}
		if (token.size() < 2)
		{
			return "pattern ends with a half-written byte";
		}
		if (token.size() > 2)
		{
			return "a token longer than two digits is not one byte";
		}

		const int hi = HexDigit(token[0]);
		const int lo = HexDigit(token[1]);
		if (hi < 0 || lo < 0)
		{
			return "character is neither a hex digit, '?' nor whitespace";
		}
		out.bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
		out.wildcard.push_back(false);
	}

	if (out.bytes.empty())
	{
		return "pattern is empty";
	}
	return nullptr;
}

PatternScanResult ScanModule(const Module& module, const BytePattern& pattern)
{
	PatternScanResult result;

	if (!module.IsValid() || module.base() == nullptr || module.size() == 0)
	{
		return result;
	}
	if (pattern.empty() || pattern.size() > module.size())
	{
		return result;
	}

	const unsigned char* base = static_cast<const unsigned char*>(module.base());
	const std::size_t last_start = module.size() - pattern.size();

	for (std::size_t start = 0; start <= last_start && result.matches < kMaxMatches; ++start)
	{
		const unsigned char* window = base + start;
		bool matched = true;
		for (std::size_t i = 0; i < pattern.size(); ++i)
		{
			if (!pattern.wildcard[i] && window[i] != pattern.bytes[i])
			{
				matched = false;
				break;
			}
		}

		if (matched)
		{
			if (result.matches == 0)
			{
				result.first = window;
			}
			else
			{
				result.second = window;
			}
			++result.matches;
		}
	}

	return result;
}

} // namespace cs2st
