#ifndef CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_

#include "testkit/ITestSuite.h"

class IFileSystem;

namespace cs2st
{

class FileSystemSuite : public ITestSuite
{
public:
	const char* Id() const override { return "filesystem"; }
	const char* DisplayName() const override { return "IFileSystem"; }
	const char* Description() const override
	{
		return "The virtual file system: handle I/O, search paths, path "
		       "resolution, directory finds, name dictionary, UGC/VPK state";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseFileQueries(TestContext& context);
	void ExerciseHandleIo(TestContext& context);
	void ExerciseBufferReads(TestContext& context);
	void ExercisePaths(TestContext& context);
	void ExerciseFinds(TestContext& context);
	void ExerciseNameDictionary(TestContext& context);
	void ExerciseMiscState(TestContext& context);
	void ExerciseScratchCalls(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	IFileSystem* fs_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_ */
