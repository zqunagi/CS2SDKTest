/**
 * What the loader entry points call once the library is inside a process.
 *
 * Linux: the constructor in Entry.cpp. Windows: DllMain in DllMain.cpp. Both
 * just call this; it returns at once unless the process is cs2 and CST_RUN=1.
 */

#ifndef CS2SDKTEST_ENTRY_H_
#define CS2SDKTEST_ENTRY_H_

namespace cs2st
{

void Start();

} // namespace cs2st

#endif /* CS2SDKTEST_ENTRY_H_ */
