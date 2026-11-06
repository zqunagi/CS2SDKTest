#include "testkit/BaselineLookup.h"

#include <cstdint>

#include <engine2/gameshareddefs.h>
#include <engine2/inetworkgameserver.h>
#include <engine2/inetworkserverservice.h>
#include <engine2/ivengineserver2.h>
#include <engine2/networkgameserverbase.h>
#include <networksystem/serializedentity.h>

namespace cs2st
{
namespace
{

constexpr int kMaxWalk = 4096;
constexpr int kMaxListed = 8;
constexpr int kMaxFieldsPlausible = 256 * 64;

bool LooksLikePointer(const void* p)
{
	const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
	return v >= 0x10000 && v < 0x0000800000000000ull && (v & 7) == 0;
}

} // namespace

const CSerializedEntity* FindInstanceBaseline(TestContext& context, INetworkServerService* service)
{
	context.Note("[CNetworkGameServerBase::m_InstanceBaselines] the source of real serializers");

	if (service == nullptr)
	{
		context.UnavailableCall("INetworkServerService", "engine2 does not answer '%s'",
			NETWORKSERVERSERVICE_INTERFACE_VERSION);
		return nullptr;
	}

	const CSerializedEntity* found = nullptr;
	CS2ST_CALL(context, "CNetworkGameServerBase::m_InstanceBaselines (walk)", {
		CNetworkGameServerBase* server = service->GetIGameServer();
		if (server == nullptr)
		{
			context.SkipCall("CNetworkGameServerBase::m_InstanceBaselines (walk)",
				"no game server; no level is loaded");
			return;
		}

		const int count = server->m_InstanceBaselines.Count();
		const int max_element = server->m_InstanceBaselines.MaxElement();
		context.Detail("-> %d baseline(s), %d slot(s)", count, max_element);
		if (count < 0 || count > kMaxWalk || max_element < 0 || max_element > kMaxWalk * 4)
		{
			context.Suspect("count %d / slots %d is not plausible; the map layout is probably off",
				count, max_element);
			return;
		}

		int listed = 0;
		int walked = 0;
		for (int i = 0; i < max_element && walked < kMaxWalk; ++i, ++walked)
		{
			if (!server->m_InstanceBaselines.IsValidIndex(i))
			{
				continue;
			}
			const CSerializedEntity* entity = server->m_InstanceBaselines.Element(i);
			if (entity == nullptr || !LooksLikePointer(entity))
			{
				continue;
			}
			if (listed < kMaxListed)
			{
				context.Detail("   baseline[key %d] %p, %d field(s)",
					server->m_InstanceBaselines.Key(i), static_cast<const void*>(entity),
					entity->m_nFieldCount);
				++listed;
			}
			if (found == nullptr && entity->m_nFieldCount > 0
				&& entity->m_nFieldCount < kMaxFieldsPlausible)
			{
				found = entity;
			}
		}

		if (found == nullptr)
		{
			context.Detail("(no baseline with fields yet)");
		}
	});
	return found;
}


} // namespace cs2st
