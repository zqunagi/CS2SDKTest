/**
 * A TOML subset reader for configs/gamedata.toml.
 *
 * Not a TOML implementation. The gamedata file is written by this project,
 * for this project, and uses exactly four constructs: [table.sub.key]
 * headers (the key quoted when it carries characters bare keys cannot, like
 * "ISource2GameEntities::CheckTransmit"), `name = "string"`, `name = integer`
 * and `name = negative integer`. Comments run from # to end of line. That is
 * the whole grammar, and keeping it this small is the point: a gamedata file
 * a plugin parses at load time is attack surface, and every TOML feature not
 * parsed is one that cannot misbehave.
 *
 * The reader is strict on purpose. Anything it does not recognise is an error
 * naming the line, so a typo in the file fails the suite loudly at the parse
 * row instead of silently testing one key fewer.
 *
 * No <filesystem>, no exceptions - same constraints as the rest of core/
 * (see FileSystemUtil.h for why).
 */

#ifndef CS2SDKTEST_CORE_TOMLDATA_H_
#define CS2SDKTEST_CORE_TOMLDATA_H_

#include <map>
#include <string>
#include <vector>

namespace cs2st
{

class TomlData
{
public:
	struct Entry
	{
		std::string text;   ///< raw string value, when the key held one
		long long number = 0; ///< integer value, when the key held one
		bool is_number = false;
	};

	/** Outcome of Parse; error() says what went wrong when ok is false. */
	bool ok() const { return ok_; }
	int error_line() const { return error_line_; }
	const std::string& error() const { return error_; }

	const std::vector<std::string>& tables() const { return order_; }
	const Entry* Find(const std::string& table) const;

	/** Integer value of a table's "windows", "linux" or shared "value" key. */
	const Entry* FindPlatform(const std::string& table, bool windows) const;

	static TomlData Parse(const std::string& text);

private:
	std::map<std::string, Entry> entries_; // full table path -> value
	std::vector<std::string> order_;       // tables in file order
	bool ok_ = false;
	int error_line_ = 0;
	std::string error_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_TOMLDATA_H_ */
