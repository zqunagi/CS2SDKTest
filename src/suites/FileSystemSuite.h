/**
 * Exercises IFileSystem - the Source 2 virtual file system.
 *
 * "VFileSystem017" @ filesystem_stdio.
 *
 * A large surface (~90 virtual slots, several overloaded) whose methods are
 * mostly either read-only or destructive. This suite keeps to the read-only
 * half: everything that would write, delete, rename, mount, unmount, load a
 * module or change global filesystem state is a one-line SkipCall with the
 * reason. The probe file is gameinfo.gi in the "GAME" path ID - every Source 2
 * game ships one and every search path set-up resolves it.
 *
 * Oracles specific to this suite (two independent routes to one fact):
 *
 *   - size: Size(name) vs Size(handle) vs bytes ReadFile delivered vs
 *     Seek(TAIL)+Tell. Four routes to one file length.
 *   - position: Read(n) advances Tell by n; Seek(HEAD) returns it to 0;
 *     EndOfFile is true after a Seek(TAIL).
 *   - path round trip: RelativePathToFullPath then FullPathToRelativePath
 *     gives back the name that was asked for (case/slash-insensitive).
 *   - name dictionary: FindFileName and FindOrAddFileName agree, and
 *     String() maps the handle back to the name.
 *   - find: FindFirst's first hit is a file FileExists accepts.
 *
 * Everything the SDK attaches no contract to (search-path text, drive lists,
 * statistics counters) is recorded, not judged.
 *
 * Buffers handed to the engine for it to allocate (ReadFileEx) are given this
 * module's own allocator through pfnAlloc, so they are freed on the heap that
 * made them - CS2SDKTEST_USE_MEMOVERRIDE is off.
 */

#ifndef CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks.
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
	void NoteSideEffectingCalls(TestContext& context);

	IFileSystem* fs_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_FILESYSTEMSUITE_H_ */
