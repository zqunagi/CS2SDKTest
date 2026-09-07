Test modules live here — one per SDK interface being exercised.

See `src/testkit/ITestModule.h` for the interface and the repository README for
a worked example. Adding one is: write the file, register it in
`TestRunner::CreateDefault()`, add it to `CS2SDKTEST_SOURCES` in CMakeLists.txt.
