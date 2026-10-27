/**
 * Checks the tier0 container layouts every plugin compiles into itself:
 * CUtlVector (and its CUtlVectorMemory_Growable allocator), CUtlString and
 * CUtlBuffer.
 *
 * These are templates and inline accessors, so a wrong layout does not fail at
 * a call - it fails wherever a plugin and the engine hand one of them to each
 * other, as a crash that points nowhere. The suite is about the two places a
 * layout can be compared with something the *engine* wrote:
 *
 *   - CUtlString and CUtlBuffer have their mutating half implemented inside
 *     tier0 (DLL_CLASS_IMPORT: Set, Append, the CUtlBuffer constructors), and
 *     the read half inline in the SDK header. Build one through the engine,
 *     read it back through the header: the engine's idea of the layout is on
 *     one side and the SDK's on the other.
 *   - CSchemaSystem::m_LoadedModules is a CUtlVector the engine owns. Its
 *     m_Size / m_pMemory / m_nAllocationCount, read raw, must agree with what
 *     Count() and Base() say.
 *
 * A CUtlVector<int> built locally is checked too, but it only proves the SDK
 * header agrees with itself (and that growth through tier0's allocator
 * works) - it is labelled that way in the log, because a local object cannot
 * contradict the engine.
 *
 * Not covered, with the reason on the matching rows:
 *   - CUtlOrderedMap / CUtlStringMap: SchemaSystemSuite already walks the
 *     engine-owned instances (m_DeclaredClasses, m_TypeScopes).
 *   - CUtlTSHash: no engine-owned instance is reachable from s2sdk's
 *     interfaces; building one here would test our own compile.
 *   - CUtlMemory: Source 2 has no such class; the allocator is
 *     CUtlVectorMemory_Growable, covered through the vectors above.
 */

#ifndef CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_
#define CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_

#include "testkit/ITestSuite.h"

class ISchemaSystem;

namespace cs2st
{

class Tier0ContainerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "tier0containers"; }
	const char* DisplayName() const override { return "tier0 containers (CUtlVector / CUtlString / CUtlBuffer)"; }
	const char* Description() const override
	{
		return "Container layouts checked against objects the engine built or owns";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseUtlString(TestContext& context);
	void ExerciseUtlBuffer(TestContext& context);
	void ExerciseBufferString(TestContext& context);
	void ExerciseUtlVector(TestContext& context);
	void NoteUncoveredContainers(TestContext& context);

	// Null when schemasystem is not loaded; only the engine-owned vector row
	// needs it, the rest of the suite runs regardless.
	ISchemaSystem* schema_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_TIER0CONTAINERSUITE_H_ */
