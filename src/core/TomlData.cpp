#include "core/TomlData.h"

#include <cctype>
#include <cstdlib>

namespace cs2st
{
namespace
{

std::string Trim(const std::string& text)
{
	std::size_t begin = 0;
	while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
	{
		++begin;
	}
	std::size_t end = text.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
	{
		--end;
	}
	return text.substr(begin, end - begin);
}

/** Strips a # comment, honouring that inside a quoted string # is literal. */
std::string StripComment(const std::string& line)
{
	bool in_string = false;
	for (std::size_t i = 0; i < line.size(); ++i)
	{
		const char c = line[i];
		if (c == '"' && (i == 0 || line[i - 1] != '\\'))
		{
			in_string = !in_string;
		}
		else if (c == '#' && !in_string)
		{
			return line.substr(0, i);
		}
	}
	return line;
}

/** True for the characters a bare TOML key may contain (A-Za-z0-9_-). */
bool IsBareKeyChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/**
 * Reads one dotted-key table header body: the part inside [ ]. Dots outside
 * quotes split into segments; quoted segments may hold anything.
 */
bool ParseTableKey(const std::string& body, std::string& joined, std::string& error)
{
	joined.clear();
	bool in_quotes = false;
	std::string segment;

	const auto flush = [&]() -> bool
	{
		if (segment.empty())
		{
			error = "empty segment in table name";
			return false;
		}
		if (!joined.empty())
		{
			joined += '.';
		}
		joined += segment;
		segment.clear();
		return true;
	};

	for (std::size_t i = 0; i < body.size(); ++i)
	{
		const char c = body[i];
		if (in_quotes)
		{
			if (c == '"')
			{
				in_quotes = false;
			}
			else
			{
				segment += c;
			}
		}
		else if (c == '"')
		{
			// A quote mid-segment (abc"def") is not TOML; reject it rather
			// than guess at what was meant.
			if (!segment.empty())
			{
				error = "quoted section not at the start of a table name segment";
				return false;
			}
			in_quotes = true;
		}
		else if (c == '.')
		{
			if (!flush())
			{
				return false;
			}
		}
		else if (IsBareKeyChar(c))
		{
			segment += c;
		}
		else
		{
			error = std::string("character '") + c + "' not allowed in a table name";
			return false;
		}
	}

	if (in_quotes)
	{
		error = "unterminated quoted table name";
		return false;
	}
	return flush();
}

} // namespace

TomlData TomlData::Parse(const std::string& text)
{
	TomlData out;
	out.ok_ = true;

	std::string current_table;
	std::size_t line_no = 0;
	std::size_t pos = 0;

	auto fail = [&](const std::string& message)
	{
		out.ok_ = false;
		out.error_line_ = static_cast<int>(line_no);
		out.error_ = message;
	};

	while (pos <= text.size() && out.ok_)
	{
		std::size_t newline = text.find('\n', pos);
		const bool last_line = (newline == std::string::npos);
		std::string line = text.substr(pos, (last_line ? text.size() : newline) - pos);
		pos = last_line ? text.size() + 1 : newline + 1;
		++line_no;

		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		line = Trim(StripComment(line));
		if (line.empty())
		{
			if (last_line)
			{
				break;
			}
			continue;
		}

		if (line[0] == '[')
		{
			if (line.size() < 3 || line.back() != ']')
			{
				fail("malformed table header");
				break;
			}
			std::string joined;
			if (!ParseTableKey(line.substr(1, line.size() - 2), joined, out.error_))
			{
				fail(out.error_);
				break;
			}
			if (out.entries_.count(joined) != 0)
			{
				fail("table defined twice: " + joined);
				break;
			}
			current_table = joined;
			out.order_.push_back(joined);
		}
		else
		{
			const std::size_t eq = line.find('=');
			if (eq == std::string::npos || current_table.empty())
			{
				fail("line is neither a [table] header nor inside one");
				break;
			}

			const std::string key = Trim(line.substr(0, eq));
			std::string value = Trim(line.substr(eq + 1));
			if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
			{
				fail("key names may contain letters, digits, '_' and '-' only");
				break;
			}

			Entry entry;
			if (!value.empty() && value[0] == '"')
			{
				if (value.size() < 2 || value.back() != '"')
				{
					fail("unterminated string value");
					break;
				}
				entry.text = value.substr(1, value.size() - 2);
			}
			else
			{
				char* end = nullptr;
				entry.number = std::strtoll(value.c_str(), &end, 10);
				entry.is_number = true;
				if (end == nullptr || *end != '\0')
				{
					fail("value is neither a quoted string nor an integer: '" + value + "'");
					break;
				}
				entry.text = value;
			}

			const std::string full = current_table + "." + key;
			if (out.entries_.count(full) != 0)
			{
				fail("key defined twice: " + full);
				break;
			}
			out.entries_[full] = entry;
		}

		if (last_line)
		{
			break;
		}
	}

	return out;
}

const TomlData::Entry* TomlData::Find(const std::string& table) const
{
	const auto it = entries_.find(table);
	return (it != entries_.end()) ? &it->second : nullptr;
}

const TomlData::Entry* TomlData::FindPlatform(const std::string& table, bool windows) const
{
	if (const Entry* direct = Find(table + (windows ? ".windows" : ".linux")); direct != nullptr)
	{
		return direct;
	}
	return Find(table + ".value");
}

} // namespace cs2st
