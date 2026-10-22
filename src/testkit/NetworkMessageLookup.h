#pragma once

#include <cstddef>
#include <cstring>

#include <networksystem/inetworkmessages.h>

namespace cs2st
{

/**
 * True when a registry key names `wanted`, with or without the " [id]" suffix
 * some builds append ("CNETMsg_Tick [4]").
 */
inline bool RegistryNameIs(const char* key, const char* wanted)
{
	if (key == nullptr)
	{
		return false;
	}
	const std::size_t length = std::strlen(wanted);
	if (std::strncmp(key, wanted, length) != 0)
	{
		return false;
	}
	const char* rest = key + length;
	if (*rest == '\0')
	{
		return true;
	}
	if (rest[0] != ' ' || rest[1] != '[')
	{
		return false;
	}
	rest += 2;
	if (*rest == '\0')
	{
		return false;
	}
	while (*rest >= '0' && *rest <= '9')
	{
		++rest;
	}
	return rest[0] == ']' && rest[1] == '\0';
}

/**
 * Finds a registered message by name on builds that key the registry as
 * "Name [id]" (libengine2 renders CProtobufBinding::GetName with "%s [%d]").
 *
 * The bare name is tried first, then the id when the caller knows it, then the
 * partial lookup. Every answer is checked against `name` through
 * RegistryNameIs, because the partial lookup returns the first prefix match
 * and "CMsgSource1LegacyGameEvent" is also a prefix of "...EventList".
 *
 * @param how  Set to the step that found it, or left untouched when none did.
 */
inline INetworkMessageInternal* FindRegisteredMessage(INetworkMessages* messages,
                                                      const char* name, int id,
                                                      const char** how)
{
	INetworkMessageInternal* message = messages->FindNetworkMessage(name);
	if (message != nullptr)
	{
		*how = "by name";
		return message;
	}

	if (id >= 0)
	{
		message = messages->FindNetworkMessageById(id);
		if (message != nullptr && RegistryNameIs(message->GetUnscopedName(), name))
		{
			*how = "by id (the registry key carries an id suffix)";
			return message;
		}
	}

	message = messages->FindNetworkMessagePartial(name);
	if (message != nullptr && RegistryNameIs(message->GetUnscopedName(), name))
	{
		*how = "through the partial lookup (the registry key carries an id suffix)";
		return message;
	}

	return nullptr;
}

}  // namespace cs2st
