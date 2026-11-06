#ifndef CS2SDKTEST_TESTKIT_BASELINELOOKUP_H_
#define CS2SDKTEST_TESTKIT_BASELINELOOKUP_H_

#include "testkit/TestContext.h"

class CSerializedEntity;
class INetworkServerService;

namespace cs2st
{

const CSerializedEntity* FindInstanceBaseline(TestContext& context, INetworkServerService* service);

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_BASELINELOOKUP_H_ */
