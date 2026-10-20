#ifndef CS2SDKTEST_TESTKIT_UTLVECTORGUARD_H_
#define CS2SDKTEST_TESTKIT_UTLVECTORGUARD_H_

#include "core/Platform.h"

/**
 * Keeps a CUtlVector's destructor from freeing its buffer on Windows.
 *
 * CUtlVector grows through tier0's UtlVectorMemory_Alloc (the engine heap) but
 * releases with the plain CRT free() compiled into this plugin. On Linux both
 * resolve to the one glibc heap, so the destructor is fine. On Windows they
 * are different heaps: the free() corrupts the heap and the process dies with
 * a fast-fail that the SEH call guard cannot catch. The same applies to a
 * buffer the game filled in.
 *
 * Declare one right after the vector. Its destructor runs first, destroys the
 * elements (CUtlString frees through tier0, which is consistent), then on
 * Windows drops the buffer with Detach() - a few KB leaked per call, on
 * purpose. Elsewhere it does nothing, so Linux behaviour is unchanged.
 */
template <typename TVector>
class UtlVectorGuard
{
public:
	explicit UtlVectorGuard(TVector& vector) : vector_(vector) {}
	~UtlVectorGuard()
	{
#if CS2ST_WINDOWS
		vector_.RemoveAll();
		(void)vector_.Detach();
#endif
	}

	UtlVectorGuard(const UtlVectorGuard&) = delete;
	UtlVectorGuard& operator=(const UtlVectorGuard&) = delete;

private:
	TVector& vector_;
};

#endif /* CS2SDKTEST_TESTKIT_UTLVECTORGUARD_H_ */
