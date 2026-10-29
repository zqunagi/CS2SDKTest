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
