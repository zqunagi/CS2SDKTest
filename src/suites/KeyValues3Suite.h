/**
 * Checks KeyValues3 against a KeyValues3 the *engine* filled in.
 *
 * KeyValues3's implementation is not a tier0 export: tier0_perproject/
 * keyvalues3.cpp is compiled into this plugin (CS2SDKTEST_TIER1_KEYVALUES3).
 * So a KeyValues3 built and read here only proves the SDK agrees with itself.
 * The one place the engine's layout meets the SDK's code is an object the
 * engine writes into: ISource2Server::WriteStatusJson(KeyValues3*). The
 * suite hands it a root built here, then reads the result back with the SDK's
 * own accessors - the engine's idea of the layout on one side, the header's on
 * the other.
 *
 * Oracles on the engine-filled root:
 *   - the raw bitfield word decoded by hand against GetTypeEx()/GetSubType()
 *     (the bit order is part of the ABI);
 *   - GetMemberCount() against walking GetMember(id) to the end;
 *   - GetMemberName(id) round-tripped through FindMember(name).
 * Member *values* are never printed: a status document can carry the host
 * name or an address, and results go to a public page.
 *
 * The local rows (scalars, string, array, table) are labelled as
 * self-consistency only. Nothing built here is destructed - the root holds
 * engine-allocated storage and the Windows allocator split described in
 * testkit/UtlVectorGuard.h applies - so each object leaks a few hundred bytes.
 *
 * With CS2SDKTEST_TIER1_KEYVALUES3 off the suite reports itself unavailable.
 */

#ifndef CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_
#define CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_

#include "testkit/ITestSuite.h"

class ISource2Server;

namespace cs2st
{

class KeyValues3Suite : public ITestSuite
{
public:
	const char* Id() const override { return "keyvalues3"; }
	const char* DisplayName() const override { return "KeyValues3"; }
	const char* Description() const override
	{
		return "KeyValues3 read back from a document the engine wrote "
		       "(needs CS2SDKTEST_TIER1_KEYVALUES3)";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseEngineFilled(TestContext& context);
	void ExerciseLocal(TestContext& context);

	ISource2Server* server_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_KEYVALUES3SUITE_H_ */
