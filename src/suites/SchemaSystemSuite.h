/**
 * Exercises the schema system - the only way to reach an entity property on
 * CS2, and the largest single surface in this plugin.
 *
 * "SchemaSystem_001" @ schemasystem resolves to ISchemaSystem. Everything a
 * CS2 plugin does with entity data starts here: find a type scope, find a class
 * binding in it, walk that binding's fields, and read the byte offset a field
 * says it lives at. Break any link in that chain and property access across the
 * whole plugin ecosystem stops working.
 *
 * What is *not* being tested is worth stating first, because it is the mistake
 * this project already made once and undid (see docs/STATUS.md): the classes
 * schema knows about and the C++ structs in the SDK headers are two different
 * descriptions that do not line up, and schema cannot be used to validate SDK
 * layouts. What is tested here is whether the schema system itself still works.
 *
 * Four kinds of check, in increasing order of how much they prove:
 *
 *  1. Calls that return. Ordinary coverage - the vtable slot is still that
 *     function and it does not fault.
 *
 *  2. Struct layout via StructInspector. CSchemaSystem, CSchemaSystemTypeScope,
 *     SchemaClassInfoData_t, SchemaClassFieldData_t, SchemaEnumInfoData_t and
 *     friends are concrete data, not interfaces, so their fields can be judged
 *     by type.
 *
 *  3. Cross-checks against an independent oracle. The same fact is reachable
 *     twice - once through a virtual call, once as a struct field - and the two
 *     have to agree. A generic check says "this pointer is plausible"; an
 *     oracle says "this is the wrong object", which is a different quality of
 *     evidence. Every one of these in the suite:
 *
 *       GetScopeName()             vs  m_szScopeName at the SDK's offset
 *       SchemaSystemIsReady()      vs  CSchemaSystem::m_bSchemaSystemIsReady
 *       DefaultTypeScopeIsLocal()  vs  CSchemaSystem::m_bDefaultTypeScopeIsLocal
 *       GlobalTypeScope()          vs  a module scope's m_pGlobalTypeScope
 *       Type_Builtin(t)            vs  &m_BuiltinTypes[t] computed by offsetof
 *       Type_Builtin(t)            vs  GetSchemaBuiltinType(t)
 *       Type_Builtin(t)            vs  FindBuiltinTypeByName(name)
 *       FindDeclaredClass(n)       vs  FindRawClassBinding(n)
 *       FindDeclaredEnum(n)        vs  FindRawEnumBinding(n)
 *       FindTypeScopeForModule(n)  vs  FindOrCreateTypeScopeForModule(n)
 *       FindTypeScopeForModule(n)  vs  m_TypeScopes[n]
 *       FindType_DeclaredClass(n)  vs  the class binding's m_pTypeOf
 *       the name looked up         vs  the binding's m_pName
 *       the scope searched         vs  the binding's m_pOuterTypeScope
 *       a field type's size        vs  its class binding's m_nSizeOf
 *       m_nMin/MaxValue  vs  every enumerator's m_nValue
 *       m_nNumFields               vs  m_pFields being null or not
 *
 *     The Type_Builtin/offsetof pair is the strongest single check in this
 *     project: it compares a virtual call's return value against an address
 *     computed from the SDK's own declared layout, so agreement confirms the
 *     vtable slot and the struct layout at once.
 *
 *  4. Enumerations. Where the system will hand over a whole list - the type
 *     scopes it holds, the interned types per category, a class's fields, base
 *     classes and metadata, an enum's enumerators - the list is walked and
 *     counted. Same reasoning as the enum lists in Source2ServerSuite: the
 *     contents change when the game changes while the interface version does
 *     not move at all.
 *
 * Two structural risks to keep in mind when reading results. The SDK declares
 * `virtual ~ISchemaSystem() = 0` as the *last* member of the interface, and
 * `virtual ~CSchemaType() = 0` likewise. Under the Itanium ABI a destructor
 * takes two vtable slots wherever it is declared, so if Valve's real classes
 * declare theirs first, every method slot in those vtables is off by two.
 * ISchemaSystem::GlobalTypeScope() and CSchemaType::IsValid() are the canaries:
 * they are slot 0 of their respective vtables, and if a destructor is there
 * instead the process will not survive the call.
 *
 * No hooks. Nothing in the engine calls into a plugin through this interface.
 */

#ifndef CS2SDKTEST_SUITES_SCHEMASYSTEMSUITE_H_
#define CS2SDKTEST_SUITES_SCHEMASYSTEMSUITE_H_

#include "testkit/ITestSuite.h"

class CSchemaClassInfo;
class CSchemaEnumInfo;
class CSchemaSystem;
class CSchemaSystemTypeScope;
class CSchemaType;
class ISchemaSystem;
struct SchemaClassFieldData_t;

namespace cs2st
{

class SchemaSystemSuite : public ITestSuite
{
public:
	const char* Id() const override { return "schema"; }
	const char* DisplayName() const override { return "ISchemaSystem / CSchemaSystemTypeScope"; }
	const char* Description() const override
	{
		return "Schema reflection: the only route to entity properties on CS2";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	// ---- calls -------------------------------------------------------------

	void ExerciseAppSystem(TestContext& context);
	void ExerciseSystemQueries(TestContext& context);
	void EnumerateTypeScopes(TestContext& context);
	void ExerciseScopeResolution(TestContext& context);
	void ExerciseBuiltinTypes(TestContext& context);
	void ExerciseScopeQueries(TestContext& context);
	void ExerciseClassLookups(TestContext& context);
	void ExerciseEnumLookups(TestContext& context);
	void ExerciseClassGraph(TestContext& context);
	void ExerciseSchemaTypeVirtuals(TestContext& context);
	void ExerciseAtomicLookups(TestContext& context);
	void ExerciseInternLookups(TestContext& context);

	// ---- layout ------------------------------------------------------------

	void InspectSchemaSystemLayout(TestContext& context);
	void InspectTypeScopeLayout(TestContext& context);
	void InspectInternedTypeMaps(TestContext& context);
	void InspectClassInfoLayout(TestContext& context);
	void InspectClassFields(TestContext& context);
	void InspectBaseClasses(TestContext& context);
	void InspectClassMetadata(TestContext& context);
	void InspectEnumInfoLayout(TestContext& context);
	void InspectEnumerators(TestContext& context);

	void NoteSideEffectingCalls(TestContext& context);

	/** Runs the CSchemaType virtual sweep against one live type instance. */
	void ProbeSchemaType(TestContext& context, const char* label, CSchemaType* type);

	/**
	 * Exercises the IsA<T>() / ReinterpretAs<T>() template helpers.
	 *
	 * Not virtuals - they are header-only templates resolved through
	 * SchemaTypeMap<T>::Match, which compares the object's m_eTypeCategory and
	 * m_eAtomicCategory against the pair the SCHEMATYPE_ENTRY macro registered
	 * for T. That makes them the documented downcast path a plugin uses, and it
	 * makes them checkable two ways at once: the answer for each of the eleven
	 * concrete types is known from the object's own category fields, and
	 * ReinterpretAs must return non-null exactly when IsA returns true.
	 */
	void ProbeSchemaTypeDowncasts(TestContext& context, const char* label,
	                              CSchemaType* type);

	ISchemaSystem* schema_ = nullptr;

	/**
	 * The same object as schema_, cast to the concrete class the SDK says it
	 * is. Everything read through this is an assumption being tested, which is
	 * why the two boolean fields it exposes are cross-checked against their
	 * virtual getters before anything else is believed.
	 */
	CSchemaSystem* schema_concrete_ = nullptr;

	CSchemaSystemTypeScope* global_scope_ = nullptr;
	CSchemaSystemTypeScope* server_scope_ = nullptr;

	/** The scope the class probes ran against, its name, and how it was found. */
	CSchemaSystemTypeScope* probe_scope_ = nullptr;
	const char* probe_scope_name_ = nullptr;

	/**
	 * Scope name as m_TypeScopes actually spells it, when the enumeration
	 * found one. Better than guessing at "server" vs "libserver.so".
	 */
	const char* resolved_scope_key_ = nullptr;

	/** A class binding resolved by ExerciseClassLookups, and its name. */
	CSchemaClassInfo* probe_class_ = nullptr;
	const char* probe_class_name_ = nullptr;

	/** That class's field array, vetted once for bounds. */
	SchemaClassFieldData_t* probe_fields_ = nullptr;
	int probe_field_count_ = 0;

	/** An enum binding, either looked up by name or derived from a class field. */
	CSchemaEnumInfo* probe_enum_ = nullptr;
	const char* probe_enum_name_ = nullptr;

	/** Live CSchemaType instances harvested from the probe class's fields. */
	CSchemaType* probe_type_ = nullptr;              ///< first field's type
	CSchemaType* probe_builtin_type_ = nullptr;      ///< a SCHEMA_TYPE_BUILTIN
	CSchemaType* probe_class_type_ = nullptr;        ///< a SCHEMA_TYPE_DECLARED_CLASS
	CSchemaType* probe_enum_type_ = nullptr;         ///< a SCHEMA_TYPE_DECLARED_ENUM
	CSchemaType* probe_atomic_type_ = nullptr;       ///< a SCHEMA_TYPE_ATOMIC
	CSchemaType* probe_pointer_type_ = nullptr;      ///< a SCHEMA_TYPE_POINTER
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SCHEMASYSTEMSUITE_H_ */
