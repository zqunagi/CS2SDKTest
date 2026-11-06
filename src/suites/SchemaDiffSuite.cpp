#include "suites/SchemaDiffSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <engine2/engineevents.h>
#include <entity2/entitycomponent.h>
#include <entity2/entityidentity.h>
#include <entity2/entityinstance.h>
#include <entity2/entitykeyvalues.h>
#include <entity2/ientity2_entityio.h>
#include <entity2/entityscriptframework.h>
#include <entity2/entitysystem.h>
#include <entity2/gametime.h>
#include <entity2/variant.h>
#include <gametrace.h>
#include <mathlib/aabb.h>
#include <const.h>
#include <rendersystem/rendersystemtypes.h>
#include <soundflags.h>
#include <tier0/basetypes_schema.h>
#include <in_buttons.h>
#include <shareddefs.h>
#include <networktransmitcomponent.h>
#include <networksystem/networksystemtypes.h>
#include <networksystem/networkvar.h>
#include <resourcefile/resourcetype.h>
#include <schemasystem/schemasystem.h>

#include "core/FileSystemUtil.h"
#include "testkit/PrivateField.h"

namespace cs2st
{

// The private members of CEntityIdentity. Has to sit in cs2st itself rather
// than an anonymous namespace: the explicit instantiation behind the macro
// must be made from the namespace that owns PrivateFieldBind.
CS2ST_PRIVATE_FIELD(SchemaDiffIdentityAttributes,  CEntityIdentity, CEntityAttributeTable*,  m_pAttributes);
CS2ST_PRIVATE_FIELD(SchemaDiffGameTimeValue,       GameTime_t,      float,                   m_Value);

namespace
{

constexpr int kMaxPlausibleCount = 1000000;
constexpr int kMaxFieldsPerClass = 20000;

// ---- what the engine says ---------------------------------------------------

struct SnapBase
{
	std::string name;
	unsigned offset = 0;
};

struct SnapField
{
	std::string name;
	std::string type;
	int offset = 0;
};

struct SnapClass
{
	std::string scope;
	std::string name;
	int size = 0;
	int align = 0;
	std::vector<SnapBase> bases;
	std::vector<SnapField> fields;
};

struct SnapEnumerator
{
	std::string name;
	long long value = 0;
};

struct SnapEnum
{
	std::string scope;
	std::string name;
	int size = 0;
	std::vector<SnapEnumerator> values;
};

struct Snapshot
{
	std::vector<SnapClass> classes;
	std::vector<SnapEnum> enums;
};

std::string Safe(const char* text)
{
	return text != nullptr ? std::string(text) : std::string();
}

void CaptureScope(Snapshot& out, const std::string& scope, CSchemaSystemTypeScope* type_scope)
{
	if (type_scope == nullptr)
	{
		return;
	}

	auto& classes = type_scope->m_DeclaredClasses.m_Map;
	const int class_count = classes.Count();
	for (int i = 0; i < class_count && i < kMaxPlausibleCount; ++i)
	{
		if (!classes.IsValidIndex(i))
		{
			continue;
		}
		CSchemaType_DeclaredClass* declared = classes.Element(i);
		CSchemaClassInfo* info = declared != nullptr ? declared->m_pClassInfo : nullptr;
		if (info == nullptr || info->m_pName == nullptr)
		{
			continue;
		}

		SnapClass entry;
		entry.scope = scope;
		entry.name = info->m_pName;
		entry.size = info->m_nSizeOf;
		entry.align = info->m_nAlignOf;

		if (info->m_pBaseClasses != nullptr)
		{
			for (int b = 0; b < info->m_nNumBaseClasses; ++b)
			{
				SnapBase base;
				base.offset = info->m_pBaseClasses[b].m_nOffset;
				if (info->m_pBaseClasses[b].m_pClass != nullptr)
				{
					base.name = Safe(info->m_pBaseClasses[b].m_pClass->m_pName);
				}
				entry.bases.push_back(base);
			}
		}

		if (info->m_pFields != nullptr && info->m_nNumFields <= kMaxFieldsPerClass)
		{
			for (int f = 0; f < info->m_nNumFields; ++f)
			{
				const SchemaClassFieldData_t& field = info->m_pFields[f];
				SnapField item;
				item.name = Safe(field.m_pName);
				if (field.m_pType != nullptr)
				{
					item.type = Safe(field.m_pType->m_sTypeName.Get());
				}
				item.offset = field.m_nSingleInheritanceOffset;
				entry.fields.push_back(item);
			}
		}
		out.classes.push_back(entry);
	}

	auto& enums = type_scope->m_DeclaredEnums.m_Map;
	const int enum_count = enums.Count();
	for (int i = 0; i < enum_count && i < kMaxPlausibleCount; ++i)
	{
		if (!enums.IsValidIndex(i))
		{
			continue;
		}
		CSchemaType_DeclaredEnum* declared = enums.Element(i);
		CSchemaEnumInfo* info = declared != nullptr ? declared->m_pEnumInfo : nullptr;
		if (info == nullptr || info->m_pName == nullptr)
		{
			continue;
		}

		SnapEnum entry;
		entry.scope = scope;
		entry.name = info->m_pName;
		entry.size = info->m_nSizeOf;
		if (info->m_pEnumerators != nullptr)
		{
			for (int v = 0; v < info->m_nNumEnumerators; ++v)
			{
				SnapEnumerator item;
				item.name = Safe(info->m_pEnumerators[v].m_pName);
				item.value = info->m_pEnumerators[v].m_nValue;
				entry.values.push_back(item);
			}
		}
		out.enums.push_back(entry);
	}
}

void CaptureAll(Snapshot& out, CSchemaSystem* system)
{
	CaptureScope(out, "global", system->GlobalTypeScope());

	const int scope_count = system->m_TypeScopes.GetNumStrings();
	for (int i = 0; i < scope_count && i < kMaxPlausibleCount; ++i)
	{
		const char* name = system->m_TypeScopes.String(i);
		CaptureScope(out, name != nullptr ? name : "?", system->m_TypeScopes[i]);
	}
}

// ---- the dump ---------------------------------------------------------------

void AppendJsonString(std::string& out, const std::string& text)
{
	out += '"';
	for (const char raw : text)
	{
		const unsigned char c = static_cast<unsigned char>(raw);
		switch (c)
		{
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20)
			{
				char escape[8];
				std::snprintf(escape, sizeof(escape), "\\u%04x", c);
				out += escape;
			}
			else
			{
				out += raw;
			}
		}
	}
	out += '"';
}

std::string ToJson(const Snapshot& snap)
{
	std::string out = "{\"format\":1,\"classes\":[";
	for (std::size_t i = 0; i < snap.classes.size(); ++i)
	{
		const SnapClass& c = snap.classes[i];
		out += i == 0 ? "" : ",";
		out += "{\"scope\":";
		AppendJsonString(out, c.scope);
		out += ",\"name\":";
		AppendJsonString(out, c.name);
		out += ",\"size\":" + std::to_string(c.size);
		out += ",\"align\":" + std::to_string(c.align);
		out += ",\"bases\":[";
		for (std::size_t b = 0; b < c.bases.size(); ++b)
		{
			out += b == 0 ? "" : ",";
			out += "{\"name\":";
			AppendJsonString(out, c.bases[b].name);
			out += ",\"offset\":" + std::to_string(c.bases[b].offset) + "}";
		}
		out += "],\"fields\":[";
		for (std::size_t f = 0; f < c.fields.size(); ++f)
		{
			out += f == 0 ? "" : ",";
			out += "{\"name\":";
			AppendJsonString(out, c.fields[f].name);
			out += ",\"type\":";
			AppendJsonString(out, c.fields[f].type);
			out += ",\"offset\":" + std::to_string(c.fields[f].offset) + "}";
		}
		out += "]}";
	}
	out += "],\"enums\":[";
	for (std::size_t i = 0; i < snap.enums.size(); ++i)
	{
		const SnapEnum& e = snap.enums[i];
		out += i == 0 ? "" : ",";
		out += "{\"scope\":";
		AppendJsonString(out, e.scope);
		out += ",\"name\":";
		AppendJsonString(out, e.name);
		out += ",\"size\":" + std::to_string(e.size) + ",\"values\":[";
		for (std::size_t v = 0; v < e.values.size(); ++v)
		{
			out += v == 0 ? "" : ",";
			out += "{\"name\":";
			AppendJsonString(out, e.values[v].name);
			out += ",\"value\":" + std::to_string(e.values[v].value) + "}";
		}
		out += "]}";
	}
	out += "]}\n";
	return out;
}

/** Same directory rule as ResultSink: CST_RESULT_DIR, else the log directory. */
std::string DumpDirectory()
{
	const char* env_dir = std::getenv("CST_RESULT_DIR");
	if (env_dir != nullptr && env_dir[0] != '\0')
	{
		return env_dir;
	}
	return fs::LogDirectory();
}

bool WriteDump(TestContext& context, const Snapshot& snap)
{
	const std::string directory = DumpDirectory();
	if (!fs::CreateDirectories(directory))
	{
		context.Detail("-> cannot create '%s'; no dump file", directory.c_str());
		return false;
	}
	const std::string path = fs::Join(directory, "schema-dump.json");
	const std::string json = ToJson(snap);

	std::FILE* file = std::fopen(path.c_str(), "wb");
	if (file == nullptr)
	{
		context.Detail("-> cannot open '%s' for writing", path.c_str());
		return false;
	}
	const std::size_t written = std::fwrite(json.data(), 1, json.size(), file);
	std::fclose(file);

	context.Detail("-> %zu classes, %zu enums, %zu bytes -> %s",
		snap.classes.size(), snap.enums.size(), written, path.c_str());
	return written == json.size();
}

// ---- what the SDK says ------------------------------------------------------

struct SdkField
{
	const char* name;
	std::size_t offset;
};

struct SdkClass
{
	const char* name;
	std::size_t size;
	std::vector<SdkField> fields;
};

struct SdkEnumerator
{
	const char* name;
	long long value;
};

struct SdkEnum
{
	const char* name;
	std::vector<SdkEnumerator> values;
};

#define SDK_FIELD(type, member) { #member, offsetof(type, member) }
#define SDK_ENUMERATOR(name) { #name, static_cast<long long>(name) }

/**
 * Every SDK class that mirrors a schema class by hand.
 *
 * To cover another class: add it here, field by field. Names are what the
 * schema calls them, which the SDK keeps as the member name. A member the
 * schema does not list is reported, so a typo here is visible rather than
 * silently skipped.
 */
std::vector<SdkClass> BuildSdkClasses()
{
	std::vector<SdkClass> classes;

	// The private members need an object to be measured against. Nothing is
	// constructed or read: only the distance from base to member is taken.
	alignas(16) static unsigned char scratch[sizeof(CEntityIdentity)];
	const CEntityIdentity* base = reinterpret_cast<const CEntityIdentity*>(scratch);

	SdkClass identity;
	identity.name = "CEntityIdentity";
	identity.size = sizeof(CEntityIdentity);
	identity.fields = {
		SDK_FIELD(CEntityIdentity, m_nameStringTableIndex),
		SDK_FIELD(CEntityIdentity, m_name),
		SDK_FIELD(CEntityIdentity, m_designerName),
		SDK_FIELD(CEntityIdentity, m_flags),
		SDK_FIELD(CEntityIdentity, m_worldGroupId),
		SDK_FIELD(CEntityIdentity, m_fDataObjectTypes),
		SDK_FIELD(CEntityIdentity, m_PathIndex),
		{ "m_pAttributes", PrivateFieldOffset<SchemaDiffIdentityAttributes>(base) },
		SDK_FIELD(CEntityIdentity, m_pPrev),
		SDK_FIELD(CEntityIdentity, m_pNext),
		SDK_FIELD(CEntityIdentity, m_pPrevByClass),
		SDK_FIELD(CEntityIdentity, m_pNextByClass),
	};
	classes.push_back(identity);

	// ---- batch 1: entity2 and the small value types they are built from -----
	// Each list is the SDK's data members that the schema also names; members
	// the schema does not list (vtable pointers, untagged fields) are left out
	// of the table on purpose - the diff reports the schema-only side itself.

	SdkClass instance;
	instance.name = "CEntityInstance";
	instance.size = sizeof(CEntityInstance);
	instance.fields = {
		SDK_FIELD(CEntityInstance, m_iszPrivateVScripts),
		SDK_FIELD(CEntityInstance, m_pEntity),
		SDK_FIELD(CEntityInstance, m_CScriptComponent),
	};
	classes.push_back(instance);

	SdkClass component;
	component.name = "CEntityComponent";
	component.size = sizeof(CEntityComponent);
	classes.push_back(component);

	SdkClass script;
	script.name = "CScriptComponent";
	script.size = sizeof(CScriptComponent);
	script.fields = {
		SDK_FIELD(CScriptComponent, m_scriptClassName),
	};
	classes.push_back(script);

	SdkClass helper;
	helper.name = "CEntityComponentHelper";
	helper.size = sizeof(CEntityComponentHelper);
	helper.fields = {
		SDK_FIELD(CEntityComponentHelper, m_flags),
		SDK_FIELD(CEntityComponentHelper, m_pInfo),
		SDK_FIELD(CEntityComponentHelper, m_nPriority),
		SDK_FIELD(CEntityComponentHelper, m_pNext),
	};
	classes.push_back(helper);

	SdkClass info;
	info.name = "EntComponentInfo_t";
	info.size = sizeof(EntComponentInfo_t);
	info.fields = {
		SDK_FIELD(EntComponentInfo_t, m_pName),
		SDK_FIELD(EntComponentInfo_t, m_pCPPClassname),
		SDK_FIELD(EntComponentInfo_t, m_pNetworkDataReferencedDescription),
		SDK_FIELD(EntComponentInfo_t, m_pNetworkDataReferencedPtrPropDescription),
		SDK_FIELD(EntComponentInfo_t, m_nRuntimeIndex),
		SDK_FIELD(EntComponentInfo_t, m_nFlags),
		SDK_FIELD(EntComponentInfo_t, m_pBaseClassComponentHelper),
	};
	classes.push_back(info);

	SdkClass chainer;
	chainer.name = "CNetworkVarChainer";
	chainer.size = sizeof(CNetworkVarChainer);
	chainer.fields = {
		SDK_FIELD(CNetworkVarChainer, m_PathIndex),
	};
	classes.push_back(chainer);

	SdkClass path_index;
	path_index.name = "ChangeAccessorFieldPathIndex_t";
	path_index.size = sizeof(ChangeAccessorFieldPathIndex_t);
	path_index.fields = {
		SDK_FIELD(ChangeAccessorFieldPathIndex_t, m_Value),
	};
	classes.push_back(path_index);

	const GameTime_t game_time_object;
	SdkClass game_time;
	game_time.name = "GameTime_t";
	game_time.size = sizeof(GameTime_t);
	game_time.fields = {
		{ "m_Value", PrivateFieldOffset<SchemaDiffGameTimeValue>(&game_time_object) },
	};
	classes.push_back(game_time);

	SdkClass resource_id;
	resource_id.name = "ResourceId_t";
	resource_id.size = sizeof(ResourceId_t);
	resource_id.fields = {
		SDK_FIELD(ResourceId_t, m_Value),
	};
	classes.push_back(resource_id);

	SdkClass loop_state;
	loop_state.name = "EngineLoopState_t";
	loop_state.size = sizeof(EngineLoopState_t);
	loop_state.fields = {
		SDK_FIELD(EngineLoopState_t, m_nPlatWindowWidth),
		SDK_FIELD(EngineLoopState_t, m_nPlatWindowHeight),
		SDK_FIELD(EngineLoopState_t, m_nRenderWidth),
		SDK_FIELD(EngineLoopState_t, m_nRenderHeight),
	};
	classes.push_back(loop_state);

	SdkClass client_output;
	client_output.name = "EventClientOutput_t";
	client_output.size = sizeof(EventClientOutput_t);
	client_output.fields = {
		SDK_FIELD(EventClientOutput_t, m_LoopState),
		SDK_FIELD(EventClientOutput_t, m_flRenderTime),
		SDK_FIELD(EventClientOutput_t, m_flRealTime),
		SDK_FIELD(EventClientOutput_t, m_flRenderFrameTimeUnbounded),
		SDK_FIELD(EventClientOutput_t, m_bRenderOnly),
	};
	classes.push_back(client_output);

	SdkClass io_event;
	io_event.name = "EntityIOQueuePrioritizedEvent_t";
	io_event.size = sizeof(EntityIOQueuePrioritizedEvent_t);
	io_event.fields = {
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_flFireTime),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_targetType),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_pTarget),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_pTargetInput),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_hActivator),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_hCaller),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_hEntTarget),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_variantValue),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_PulseArguments),
		SDK_FIELD(EntityIOQueuePrioritizedEvent_t, m_paramMap),
	};
	classes.push_back(io_event);

	// ---- batch 2: geometry, physics surfaces, entity I/O, transmit -----------

	SdkClass aabb;
	aabb.name = "AABB_t";
	aabb.size = sizeof(AABB_t);
	aabb.fields = {
		SDK_FIELD(AABB_t, m_vMinBounds),
		SDK_FIELD(AABB_t, m_vMaxBounds),
	};
	classes.push_back(aabb);

	SdkClass hitbox;
	hitbox.name = "CHitBox";
	hitbox.size = sizeof(CHitBox);
	hitbox.fields = {
		SDK_FIELD(CHitBox, m_name),
		SDK_FIELD(CHitBox, m_sSurfaceProperty),
		SDK_FIELD(CHitBox, m_sBoneName),
		SDK_FIELD(CHitBox, m_vMinBounds),
		SDK_FIELD(CHitBox, m_vMaxBounds),
		SDK_FIELD(CHitBox, m_flShapeRadius),
		SDK_FIELD(CHitBox, m_nBoneNameHash),
		SDK_FIELD(CHitBox, m_nGroupId),
		SDK_FIELD(CHitBox, m_nShapeType),
		SDK_FIELD(CHitBox, m_bTranslationOnly),
		SDK_FIELD(CHitBox, m_CRC),
		SDK_FIELD(CHitBox, m_cRenderColor),
		SDK_FIELD(CHitBox, m_nHitBoxIndex),
	};
	classes.push_back(hitbox);

	SdkClass surface;
	surface.name = "CPhysSurfaceProperties";
	surface.size = sizeof(CPhysSurfaceProperties);
	surface.fields = {
		SDK_FIELD(CPhysSurfaceProperties, m_name),
		SDK_FIELD(CPhysSurfaceProperties, m_nameHash),
		SDK_FIELD(CPhysSurfaceProperties, m_baseNameHash),
		SDK_FIELD(CPhysSurfaceProperties, m_bHidden),
		SDK_FIELD(CPhysSurfaceProperties, m_description),
		SDK_FIELD(CPhysSurfaceProperties, m_physics),
		SDK_FIELD(CPhysSurfaceProperties, m_vehicleParams),
		SDK_FIELD(CPhysSurfaceProperties, m_audioSounds),
		SDK_FIELD(CPhysSurfaceProperties, m_audioParams),
	};
	classes.push_back(surface);

	SdkClass surface_physics;
	surface_physics.name = "CPhysSurfacePropertiesPhysics";
	surface_physics.size = sizeof(CPhysSurfacePropertiesPhysics);
	surface_physics.fields = {
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_friction),
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_elasticity),
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_density),
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_thickness),
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_softContactFrequency),
		SDK_FIELD(CPhysSurfacePropertiesPhysics, m_softContactDampingRatio),
	};
	classes.push_back(surface_physics);

	SdkClass surface_vehicle;
	surface_vehicle.name = "CPhysSurfacePropertiesVehicle";
	surface_vehicle.size = sizeof(CPhysSurfacePropertiesVehicle);
	surface_vehicle.fields = {
		SDK_FIELD(CPhysSurfacePropertiesVehicle, m_wheelDrag),
		SDK_FIELD(CPhysSurfacePropertiesVehicle, m_wheelFrictionScale),
	};
	classes.push_back(surface_vehicle);

	SdkClass surface_sounds;
	surface_sounds.name = "CPhysSurfacePropertiesSoundNames";
	surface_sounds.size = sizeof(CPhysSurfacePropertiesSoundNames);
	surface_sounds.fields = {
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_impactSoft),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_impactHard),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_scrapeSmooth),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_scrapeRough),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_bulletImpact),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_rolling),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_break),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_strain),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_meleeImpact),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_pushOff),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_skidStop),
		SDK_FIELD(CPhysSurfacePropertiesSoundNames, m_resonant),
	};
	classes.push_back(surface_sounds);

	SdkClass surface_audio;
	surface_audio.name = "CPhysSurfacePropertiesAudio";
	surface_audio.size = sizeof(CPhysSurfacePropertiesAudio);
	surface_audio.fields = {
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_reflectivity),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_hardnessFactor),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_roughnessFactor),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_roughThreshold),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_hardThreshold),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_hardVelocityThreshold),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_flStaticImpactVolume),
		SDK_FIELD(CPhysSurfacePropertiesAudio, m_flOcclusionFactor),
	};
	classes.push_back(surface_audio);

	SdkClass transmit;
	transmit.name = "CNetworkTransmitComponent";
	transmit.size = sizeof(CNetworkTransmitComponent);
	transmit.fields = {
		SDK_FIELD(CNetworkTransmitComponent, m_nTransmitStateOwnedCounter),
	};
	classes.push_back(transmit);

	SdkClass io_output;
	io_output.name = "CEntityIOOutput";
	io_output.size = sizeof(CEntityIOOutput);
	classes.push_back(io_output);

	SdkClass keyvalues;
	keyvalues.name = "CEntityKeyValues";
	keyvalues.size = sizeof(CEntityKeyValues);
	classes.push_back(keyvalues);

	SdkClass variant_alloc;
	variant_alloc.name = "CVariantDefaultAllocator";
	variant_alloc.size = sizeof(CVariantDefaultAllocator);
	classes.push_back(variant_alloc);

	return classes;
}

std::vector<SdkEnum> BuildSdkEnums()
{
	std::vector<SdkEnum> enums;

	SdkEnum classt;
	classt.name = "Class_T";
	classt.values = {
		SDK_ENUMERATOR(CLASS_NONE),
		SDK_ENUMERATOR(CLASS_PLAYER),
		SDK_ENUMERATOR(CLASS_PLAYER_ALLY),
		SDK_ENUMERATOR(CLASS_C4_FOR_RADAR),
		SDK_ENUMERATOR(CLASS_FOOT_CONTACT_SHADOW),
		SDK_ENUMERATOR(CLASS_WEAPON),
		SDK_ENUMERATOR(CLASS_WATER_SPLASHER),
		SDK_ENUMERATOR(CLASS_HUDMODEL_WEAPON),
		SDK_ENUMERATOR(CLASS_HUDMODEL_ARMS),
		SDK_ENUMERATOR(CLASS_HUDMODEL_ADDON),
		SDK_ENUMERATOR(CLASS_WORLDMODEL_GLOVES),
		SDK_ENUMERATOR(CLASS_DOOR),
		SDK_ENUMERATOR(CLASS_PLANTED_C4),
		SDK_ENUMERATOR(NUM_CLASSIFY_CLASSES),
	};
	enums.push_back(classt);

	SdkEnum damagetypes;
	damagetypes.name = "DamageTypes_t";
	damagetypes.values = {
		SDK_ENUMERATOR(DMG_GENERIC),
		SDK_ENUMERATOR(DMG_CRUSH),
		SDK_ENUMERATOR(DMG_BULLET),
		SDK_ENUMERATOR(DMG_SLASH),
		SDK_ENUMERATOR(DMG_BURN),
		SDK_ENUMERATOR(DMG_VEHICLE),
		SDK_ENUMERATOR(DMG_FALL),
		SDK_ENUMERATOR(DMG_BLAST),
		SDK_ENUMERATOR(DMG_CLUB),
		SDK_ENUMERATOR(DMG_SHOCK),
		SDK_ENUMERATOR(DMG_SONIC),
		SDK_ENUMERATOR(DMG_ENERGYBEAM),
		SDK_ENUMERATOR(DMG_BUCKSHOT),
		SDK_ENUMERATOR(DMG_BLAST_SURFACE),
		SDK_ENUMERATOR(DMG_DISSOLVE),
		SDK_ENUMERATOR(DMG_DROWN),
		SDK_ENUMERATOR(DMG_POISON),
		SDK_ENUMERATOR(DMG_RADIATION),
		SDK_ENUMERATOR(DMG_DROWNRECOVER),
		SDK_ENUMERATOR(DMG_ACID),
		SDK_ENUMERATOR(DMG_LASTGENERICFLAG),
		SDK_ENUMERATOR(DMG_HEADSHOT),
	};
	enums.push_back(damagetypes);

	SdkEnum entitydissolvetype;
	entitydissolvetype.name = "EntityDissolveType_t";
	entitydissolvetype.values = {
		SDK_ENUMERATOR(ENTITY_DISSOLVE_INVALID),
		SDK_ENUMERATOR(ENTITY_DISSOLVE_NORMAL),
		SDK_ENUMERATOR(ENTITY_DISSOLVE_ELECTRICAL),
		SDK_ENUMERATOR(ENTITY_DISSOLVE_ELECTRICAL_LIGHT),
		SDK_ENUMERATOR(ENTITY_DISSOLVE_CORE),
	};
	enums.push_back(entitydissolvetype);

	SdkEnum entitydormancytype;
	entitydormancytype.name = "EntityDormancyType_t";
	entitydormancytype.values = {
		SDK_ENUMERATOR(ENTITY_NOT_DORMANT),
		SDK_ENUMERATOR(ENTITY_DORMANT),
		SDK_ENUMERATOR(ENTITY_SUSPENDED),
	};
	enums.push_back(entitydormancytype);

	SdkEnum entityeffects;
	entityeffects.name = "EntityEffects_t";
	entityeffects.values = {
		SDK_ENUMERATOR(DEPRICATED_EF_NOINTERP),
		SDK_ENUMERATOR(EF_NOSHADOW),
		SDK_ENUMERATOR(EF_NODRAW),
		SDK_ENUMERATOR(EF_NORECEIVESHADOW),
		SDK_ENUMERATOR(EF_PARENT_ANIMATES),
		SDK_ENUMERATOR(EF_NODRAW_BUT_TRANSMIT),
		SDK_ENUMERATOR(EF_MAX_BITS),
	};
	enums.push_back(entityeffects);

	SdkEnum entityiotargettype;
	entityiotargettype.name = "EntityIOTargetType_t";
	entityiotargettype.values = {
		SDK_ENUMERATOR(ENTITY_IO_TARGET_INVALID),
		SDK_ENUMERATOR(ENTITY_IO_TARGET_ENTITYNAME),
		SDK_ENUMERATOR(ENTITY_IO_TARGET_EHANDLE),
		SDK_ENUMERATOR(ENTITY_IO_TARGET_ENTITYNAME_OR_CLASSNAME),
	};
	enums.push_back(entityiotargettype);

	SdkEnum flags;
	flags.name = "Flags_t";
	flags.values = {
		SDK_ENUMERATOR(FL_ONGROUND),
		SDK_ENUMERATOR(FL_DUCKING),
		SDK_ENUMERATOR(FL_WATERJUMP),
		SDK_ENUMERATOR(FL_BOT),
		SDK_ENUMERATOR(FL_FROZEN),
		SDK_ENUMERATOR(FL_ATCONTROLS),
		SDK_ENUMERATOR(FL_CLIENT),
		SDK_ENUMERATOR(FL_FAKECLIENT),
		SDK_ENUMERATOR(FL_FLY),
		SDK_ENUMERATOR(FL_SUPPRESS_SAVE),
		SDK_ENUMERATOR(FL_IN_VEHICLE),
		SDK_ENUMERATOR(FL_GODMODE),
		SDK_ENUMERATOR(FL_NOTARGET),
		SDK_ENUMERATOR(FL_AIMTARGET),
		SDK_ENUMERATOR(FL_GRENADE),
		SDK_ENUMERATOR(FL_DONTTOUCH),
		SDK_ENUMERATOR(FL_BASEVELOCITY),
		SDK_ENUMERATOR(FL_CONVEYOR),
		SDK_ENUMERATOR(FL_OBJECT),
		SDK_ENUMERATOR(FL_ONFIRE),
		SDK_ENUMERATOR(FL_DISSOLVING),
		SDK_ENUMERATOR(FL_TRANSRAGDOLL),
		SDK_ENUMERATOR(FL_UNBLOCKABLE_BY_PLAYER),
	};
	enums.push_back(flags);

	SdkEnum hitgroup;
	hitgroup.name = "HitGroup_t";
	hitgroup.values = {
		SDK_ENUMERATOR(HITGROUP_INVALID),
		SDK_ENUMERATOR(HITGROUP_GENERIC),
		SDK_ENUMERATOR(HITGROUP_HEAD),
		SDK_ENUMERATOR(HITGROUP_CHEST),
		SDK_ENUMERATOR(HITGROUP_STOMACH),
		SDK_ENUMERATOR(HITGROUP_LEFTARM),
		SDK_ENUMERATOR(HITGROUP_RIGHTARM),
		SDK_ENUMERATOR(HITGROUP_LEFTLEG),
		SDK_ENUMERATOR(HITGROUP_RIGHTLEG),
		SDK_ENUMERATOR(HITGROUP_NECK),
		SDK_ENUMERATOR(HITGROUP_UNUSED),
		SDK_ENUMERATOR(HITGROUP_GEAR),
		SDK_ENUMERATOR(HITGROUP_SPECIAL),
		SDK_ENUMERATOR(HITGROUP_COUNT),
	};
	enums.push_back(hitgroup);

	SdkEnum inputbitmask;
	inputbitmask.name = "InputBitMask_t";
	inputbitmask.values = {
		SDK_ENUMERATOR(IN_NONE),
		SDK_ENUMERATOR(IN_ALL),
		SDK_ENUMERATOR(IN_ATTACK),
		SDK_ENUMERATOR(IN_JUMP),
		SDK_ENUMERATOR(IN_DUCK),
		SDK_ENUMERATOR(IN_FORWARD),
		SDK_ENUMERATOR(IN_BACK),
		SDK_ENUMERATOR(IN_USE),
		SDK_ENUMERATOR(IN_TURNLEFT),
		SDK_ENUMERATOR(IN_TURNRIGHT),
		SDK_ENUMERATOR(IN_MOVELEFT),
		SDK_ENUMERATOR(IN_MOVERIGHT),
		SDK_ENUMERATOR(IN_ATTACK2),
		SDK_ENUMERATOR(IN_RELOAD),
		SDK_ENUMERATOR(IN_SPEED),
		SDK_ENUMERATOR(IN_JOYAUTOSPRINT),
		SDK_ENUMERATOR(IN_FIRST_MOD_SPECIFIC_BIT),
		SDK_ENUMERATOR(IN_USEORRELOAD),
		SDK_ENUMERATOR(IN_SCORE),
		SDK_ENUMERATOR(IN_ZOOM),
		SDK_ENUMERATOR(IN_LOOK_AT_WEAPON),
	};
	enums.push_back(inputbitmask);

	SdkEnum itemflagtypes;
	itemflagtypes.name = "ItemFlagTypes_t";
	itemflagtypes.values = {
		SDK_ENUMERATOR(ITEM_FLAG_NONE),
		SDK_ENUMERATOR(ITEM_FLAG_CAN_SELECT_WITHOUT_AMMO),
		SDK_ENUMERATOR(ITEM_FLAG_NOAUTORELOAD),
		SDK_ENUMERATOR(ITEM_FLAG_NOAUTOSWITCHEMPTY),
		SDK_ENUMERATOR(ITEM_FLAG_LIMITINWORLD),
		SDK_ENUMERATOR(ITEM_FLAG_EXHAUSTIBLE),
		SDK_ENUMERATOR(ITEM_FLAG_DOHITLOCATIONDMG),
		SDK_ENUMERATOR(ITEM_FLAG_NOAMMOPICKUPS),
		SDK_ENUMERATOR(ITEM_FLAG_NOITEMPICKUP),
	};
	enums.push_back(itemflagtypes);

	SdkEnum lifestate;
	lifestate.name = "LifeState_t";
	lifestate.values = {
		SDK_ENUMERATOR(LIFE_ALIVE),
		SDK_ENUMERATOR(LIFE_DYING),
		SDK_ENUMERATOR(LIFE_DEAD),
		SDK_ENUMERATOR(LIFE_RESPAWNABLE),
		SDK_ENUMERATOR(LIFE_RESPAWNING),
		SDK_ENUMERATOR(NUM_LIFESTATES),
	};
	enums.push_back(lifestate);

	SdkEnum movecollide;
	movecollide.name = "MoveCollide_t";
	movecollide.values = {
		SDK_ENUMERATOR(MOVECOLLIDE_DEFAULT),
		SDK_ENUMERATOR(MOVECOLLIDE_FLY_BOUNCE),
		SDK_ENUMERATOR(MOVECOLLIDE_FLY_CUSTOM),
		SDK_ENUMERATOR(MOVECOLLIDE_FLY_SLIDE),
		SDK_ENUMERATOR(MOVECOLLIDE_COUNT),
		SDK_ENUMERATOR(MOVECOLLIDE_MAX_BITS),
	};
	enums.push_back(movecollide);

	SdkEnum movetype;
	movetype.name = "MoveType_t";
	movetype.values = {
		SDK_ENUMERATOR(MOVETYPE_NONE),
		SDK_ENUMERATOR(MOVETYPE_OBSOLETE),
		SDK_ENUMERATOR(MOVETYPE_WALK),
		SDK_ENUMERATOR(MOVETYPE_FLY),
		SDK_ENUMERATOR(MOVETYPE_FLYGRAVITY),
		SDK_ENUMERATOR(MOVETYPE_VPHYSICS),
		SDK_ENUMERATOR(MOVETYPE_PUSH),
		SDK_ENUMERATOR(MOVETYPE_NOCLIP),
		SDK_ENUMERATOR(MOVETYPE_OBSERVER),
		SDK_ENUMERATOR(MOVETYPE_LADDER),
		SDK_ENUMERATOR(MOVETYPE_CUSTOM),
		SDK_ENUMERATOR(MOVETYPE_LAST),
		SDK_ENUMERATOR(MOVETYPE_INVALID),
		SDK_ENUMERATOR(MOVETYPE_MAX_BITS),
	};
	enums.push_back(movetype);

	SdkEnum observermode;
	observermode.name = "ObserverMode_t";
	observermode.values = {
		SDK_ENUMERATOR(OBS_MODE_NONE),
		SDK_ENUMERATOR(OBS_MODE_FIXED),
		SDK_ENUMERATOR(OBS_MODE_IN_EYE),
		SDK_ENUMERATOR(OBS_MODE_CHASE),
		SDK_ENUMERATOR(OBS_MODE_ROAMING),
		SDK_ENUMERATOR(NUM_OBSERVER_MODES),
	};
	enums.push_back(observermode);

	SdkEnum renderfx;
	renderfx.name = "RenderFx_t";
	renderfx.values = {
		SDK_ENUMERATOR(kRenderFxNone),
		SDK_ENUMERATOR(kRenderFxPulseSlow),
		SDK_ENUMERATOR(kRenderFxPulseFast),
		SDK_ENUMERATOR(kRenderFxPulseSlowWide),
		SDK_ENUMERATOR(kRenderFxPulseFastWide),
		SDK_ENUMERATOR(kRenderFxFadeSlow),
		SDK_ENUMERATOR(kRenderFxFadeFast),
		SDK_ENUMERATOR(kRenderFxSolidSlow),
		SDK_ENUMERATOR(kRenderFxSolidFast),
		SDK_ENUMERATOR(kRenderFxStrobeSlow),
		SDK_ENUMERATOR(kRenderFxStrobeFast),
		SDK_ENUMERATOR(kRenderFxStrobeFaster),
		SDK_ENUMERATOR(kRenderFxFlickerSlow),
		SDK_ENUMERATOR(kRenderFxFlickerFast),
		SDK_ENUMERATOR(kRenderFxFadeOut),
		SDK_ENUMERATOR(kRenderFxFadeIn),
		SDK_ENUMERATOR(kRenderFxPulseFastWider),
		SDK_ENUMERATOR(kRenderFxMax),
	};
	enums.push_back(renderfx);

	SdkEnum rendermode;
	rendermode.name = "RenderMode_t";
	rendermode.values = {
		SDK_ENUMERATOR(kRenderNormal),
		SDK_ENUMERATOR(kRenderTransAlpha),
		SDK_ENUMERATOR(kRenderNone),
		SDK_ENUMERATOR(kRenderModeCount),
	};
	enums.push_back(rendermode);

	SdkEnum rendermultisampletype;
	rendermultisampletype.name = "RenderMultisampleType_t";
	rendermultisampletype.values = {
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_INVALID),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_NONE),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_2X),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_4X),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_6X),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_8X),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_16X),
		SDK_ENUMERATOR(RENDER_MULTISAMPLE_TYPE_COUNT),
	};
	enums.push_back(rendermultisampletype);

	SdkEnum solidtype;
	solidtype.name = "SolidType_t";
	solidtype.values = {
		SDK_ENUMERATOR(SOLID_NONE),
		SDK_ENUMERATOR(SOLID_BSP),
		SDK_ENUMERATOR(SOLID_BBOX),
		SDK_ENUMERATOR(SOLID_OBB),
		SDK_ENUMERATOR(SOLID_SPHERE),
		SDK_ENUMERATOR(SOLID_POINT),
		SDK_ENUMERATOR(SOLID_VPHYSICS),
		SDK_ENUMERATOR(SOLID_CAPSULE),
		SDK_ENUMERATOR(SOLID_CYLINDER),
		SDK_ENUMERATOR(SOLID_LAST),
	};
	enums.push_back(solidtype);

	SdkEnum takedamageflags;
	takedamageflags.name = "TakeDamageFlags_t";
	takedamageflags.values = {
		SDK_ENUMERATOR(DFLAG_NONE),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_HEALTH_CHANGES),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_PHYSICS_FORCE),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_EFFECTS),
		SDK_ENUMERATOR(DFLAG_PREVENT_DEATH),
		SDK_ENUMERATOR(DFLAG_FORCE_DEATH),
		SDK_ENUMERATOR(DFLAG_ALWAYS_GIB),
		SDK_ENUMERATOR(DFLAG_NEVER_GIB),
		SDK_ENUMERATOR(DFLAG_REMOVE_NO_RAGDOLL),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_DAMAGE_MODIFICATION),
		SDK_ENUMERATOR(DFLAG_ALWAYS_FIRE_DAMAGE_EVENTS),
		SDK_ENUMERATOR(DFLAG_RADIUS_DMG),
		SDK_ENUMERATOR(DFLAG_FORCEREDUCEARMOR_DMG),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_INTERRUPT_FLINCH),
		SDK_ENUMERATOR(DFLAG_IGNORE_DESTRUCTIBLE_PARTS),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_BREAKABLES),
		SDK_ENUMERATOR(DFLAG_FORCE_PHYSICS_FORCE),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_SCREENSPACE_DAMAGE_FX),
		SDK_ENUMERATOR(DFLAG_ALLOW_NON_AUTHORITATIVE),
		SDK_ENUMERATOR(DMG_LASTDFLAG),
		SDK_ENUMERATOR(DFLAG_IGNORE_ARMOR),
		SDK_ENUMERATOR(DFLAG_SUPPRESS_UTILREMOVE),
	};
	enums.push_back(takedamageflags);

	SdkEnum threestate;
	threestate.name = "ThreeState_t";
	threestate.values = {
		SDK_ENUMERATOR(TRS_FALSE),
		SDK_ENUMERATOR(TRS_TRUE),
		SDK_ENUMERATOR(TRS_NONE),
	};
	enums.push_back(threestate);

	SdkEnum soundlevel;
	soundlevel.name = "soundlevel_t";
	soundlevel.values = {
		SDK_ENUMERATOR(SNDLVL_NONE),
		SDK_ENUMERATOR(SNDLVL_20dB),
		SDK_ENUMERATOR(SNDLVL_25dB),
		SDK_ENUMERATOR(SNDLVL_30dB),
		SDK_ENUMERATOR(SNDLVL_35dB),
		SDK_ENUMERATOR(SNDLVL_40dB),
		SDK_ENUMERATOR(SNDLVL_45dB),
		SDK_ENUMERATOR(SNDLVL_50dB),
		SDK_ENUMERATOR(SNDLVL_55dB),
		SDK_ENUMERATOR(SNDLVL_IDLE),
		SDK_ENUMERATOR(SNDLVL_60dB),
		SDK_ENUMERATOR(SNDLVL_65dB),
		SDK_ENUMERATOR(SNDLVL_STATIC),
		SDK_ENUMERATOR(SNDLVL_70dB),
		SDK_ENUMERATOR(SNDLVL_NORM),
		SDK_ENUMERATOR(SNDLVL_75dB),
		SDK_ENUMERATOR(SNDLVL_80dB),
		SDK_ENUMERATOR(SNDLVL_TALKING),
		SDK_ENUMERATOR(SNDLVL_85dB),
		SDK_ENUMERATOR(SNDLVL_90dB),
		SDK_ENUMERATOR(SNDLVL_95dB),
		SDK_ENUMERATOR(SNDLVL_100dB),
		SDK_ENUMERATOR(SNDLVL_105dB),
		SDK_ENUMERATOR(SNDLVL_110dB),
		SDK_ENUMERATOR(SNDLVL_120dB),
		SDK_ENUMERATOR(SNDLVL_130dB),
		SDK_ENUMERATOR(SNDLVL_GUNFIRE),
		SDK_ENUMERATOR(SNDLVL_140dB),
		SDK_ENUMERATOR(SNDLVL_150dB),
		SDK_ENUMERATOR(SNDLVL_180dB),
	};
	enums.push_back(soundlevel);

	return enums;
}

// ---- the diff ---------------------------------------------------------------

constexpr int kMaxListedExtras = 40;

void CompareClass(TestContext& context, const SdkClass& sdk, const Snapshot& snap)
{
	int found = 0;
	for (const SnapClass& engine : snap.classes)
	{
		if (engine.name != sdk.name)
		{
			continue;
		}
		++found;
		context.Detail("scope '%s': engine size %d, SDK sizeof %zu, %zu engine fields, %zu SDK fields",
			engine.scope.c_str(), engine.size, sdk.size, engine.fields.size(), sdk.fields.size());

		if (static_cast<std::size_t>(engine.size) != sdk.size)
		{
			context.Fail("%s: engine says %d bytes, SDK declares %zu",
				sdk.name, engine.size, sdk.size);
		}

		std::vector<bool> matched(engine.fields.size(), false);
		for (const SdkField& field : sdk.fields)
		{
			bool present = false;
			for (std::size_t i = 0; i < engine.fields.size(); ++i)
			{
				if (engine.fields[i].name != field.name)
				{
					continue;
				}
				present = true;
				matched[i] = true;
				if (static_cast<std::size_t>(engine.fields[i].offset) != field.offset)
				{
					context.Fail("%s::%s: engine offset 0x%x, SDK offset 0x%zx",
						sdk.name, field.name, engine.fields[i].offset, field.offset);
				}
				break;
			}
			if (!present)
			{
				context.Fail("%s::%s: the SDK declares it (0x%zx) but the schema has no such field",
					sdk.name, field.name, field.offset);
			}
		}

		int listed = 0;
		int extras = 0;
		for (std::size_t i = 0; i < engine.fields.size(); ++i)
		{
			if (matched[i])
			{
				continue;
			}
			++extras;
			if (listed++ < kMaxListedExtras)
			{
				context.Detail("SDK lacks %s::%s  type %s  @0x%x",
					sdk.name, engine.fields[i].name.c_str(), engine.fields[i].type.c_str(),
					engine.fields[i].offset);
			}
		}
		if (extras > kMaxListedExtras)
		{
			context.Detail("... and %d more schema fields the SDK does not declare",
				extras - kMaxListedExtras);
		}
	}

	if (found == 0)
	{
		context.Detail("-> no scope has a class named '%s'", sdk.name);
		context.Fail("%s: the SDK declares it but the schema does not know it", sdk.name);
	}
}

void CompareEnum(TestContext& context, const SdkEnum& sdk, const Snapshot& snap)
{
	int found = 0;
	for (const SnapEnum& engine : snap.enums)
	{
		if (engine.name != sdk.name)
		{
			continue;
		}
		++found;
		context.Detail("scope '%s': %zu engine enumerators, %zu SDK enumerators",
			engine.scope.c_str(), engine.values.size(), sdk.values.size());

		std::vector<bool> matched(engine.values.size(), false);
		for (const SdkEnumerator& value : sdk.values)
		{
			bool present = false;
			for (std::size_t i = 0; i < engine.values.size(); ++i)
			{
				if (engine.values[i].name != value.name)
				{
					continue;
				}
				present = true;
				matched[i] = true;
				if (engine.values[i].value != value.value)
				{
					context.Fail("%s::%s: engine value %lld, SDK value %lld",
						sdk.name, value.name, engine.values[i].value, value.value);
					if (engine.size > 0 && engine.size < 8)
					{
						// The schema widens values to int64; a 1-byte enum whose
						// SDK underlying type is unsigned reads -1 as 255.
						const unsigned long long mask = (1ull << (8 * engine.size)) - 1;
						if ((static_cast<unsigned long long>(engine.values[i].value) & mask)
							== (static_cast<unsigned long long>(value.value) & mask))
						{
							context.Detail("same %d-byte pattern: the SDK's underlying type "
								"reads it with the other signedness", engine.size);
						}
					}
				}
				break;
			}
			if (!present)
			{
				// Perhaps renamed rather than removed: say what holds that value now.
				const char* holder = nullptr;
				for (const SnapEnumerator& candidate : engine.values)
				{
					if (candidate.value == value.value)
					{
						holder = candidate.name.c_str();
						break;
					}
				}
				context.Fail("%s::%s = %lld: not in the schema (that value is '%s' there)",
					sdk.name, value.name, value.value, holder != nullptr ? holder : "unused");
			}
		}

		int listed = 0;
		int extras = 0;
		for (std::size_t i = 0; i < engine.values.size(); ++i)
		{
			if (matched[i])
			{
				continue;
			}
			++extras;
			if (listed++ < kMaxListedExtras)
			{
				context.Detail("SDK lacks %s::%s = %lld",
					sdk.name, engine.values[i].name.c_str(), engine.values[i].value);
			}
		}
		if (extras > kMaxListedExtras)
		{
			context.Detail("... and %d more enumerators the SDK does not declare",
				extras - kMaxListedExtras);
		}
	}

	if (found == 0)
	{
		context.Detail("-> no scope has an enum named '%s'", sdk.name);
		context.Fail("%s: the SDK declares it but the schema does not know it", sdk.name);
	}
}

} // namespace

bool SchemaDiffSuite::Acquire(TestContext& context)
{
	ISchemaSystem* iface = static_cast<ISchemaSystem*>(
		context.QueryInterface("schemasystem", SCHEMASYSTEM_INTERFACE_VERSION));
	if (iface == nullptr)
	{
		context.UnavailableCall("ISchemaSystem", "schemasystem does not answer '%s'",
			SCHEMASYSTEM_INTERFACE_VERSION);
		return false;
	}

	// Same assumption SchemaSystemSuite tests; this suite relies on it holding.
	schema_ = static_cast<CSchemaSystem*>(iface);
	context.Note("interface at %p", static_cast<void*>(iface));
	return true;
}

void SchemaDiffSuite::ExerciseCalls(TestContext& context)
{
	Snapshot snapshot;
	bool captured = false;

	CS2ST_CALL(context, "SchemaDiff::capture", {
		if (!schema_->SchemaSystemIsReady())
		{
			context.Detail("-> SchemaSystemIsReady() is false; nothing to read yet");
			return;
		}
		CaptureAll(snapshot, schema_);
		captured = !snapshot.classes.empty();
		context.Detail("-> %zu classes, %zu enums", snapshot.classes.size(), snapshot.enums.size());
		if (!captured)
		{
			context.Fail("the schema system is ready but declares no classes");
		}
	});

	if (!captured)
	{
		context.SkipCall("SchemaDiff::dump", "nothing was captured");
		context.SkipCall("SchemaDiff::compare", "nothing was captured");
		return;
	}

	CS2ST_CALL(context, "SchemaDiff::dump", {
		WriteDump(context, snapshot);
	});

	for (const SdkClass& sdk : BuildSdkClasses())
	{
		const std::string target = std::string("SchemaDiff::class ") + sdk.name;
		CS2ST_CALL(context, target, {
			CompareClass(context, sdk, snapshot);
		});
	}

	for (const SdkEnum& sdk : BuildSdkEnums())
	{
		const std::string target = std::string("SchemaDiff::enum ") + sdk.name;
		CS2ST_CALL(context, target, {
			CompareEnum(context, sdk, snapshot);
		});
	}
}

} // namespace cs2st
