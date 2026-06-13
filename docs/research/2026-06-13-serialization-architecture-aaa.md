# Serialization Architecture Research: AAA Patterns vs. Astra

**Date:** 2026-06-13
**Status:** RESEARCH -- do not implement until a design phase is run.
**Scope:** AAA/major-ECS serialization survey + Astra current-state diagnosis +
concrete evolution options for the three-job requirement (runtime snapshots,
human-editable editor data, cross-language server data).

---

## 1. Code Audit: Astra's Actual Serialization Shape

### What exists

**`BinaryArchive.hpp`**
- `BinaryArchive` base (virtual `IsLoading()`).
- `HasSerializeMethod<T, Archive>` concept: templated on the archive type. This is
  the archive-generic hook -- any type can implement `void Serialize(Archive&)` and
  it will work with ANY archive that satisfies the same interface shape.
- `SerializationTraits<T>`: specializable per type; carries `Version`, `MinVersion`,
  `HasCustomSerializer` bool, and a static `Serialize(Archive&, T&)` method. Also
  templated on Archive. Migration hook: `Migrate(Archive&, T&, uint32_t storedVersion)`.

**`BinaryWriter.hpp` / `BinaryReader.hpp`**
- Concrete classes deriving `BinaryArchive`. File and memory targets.
- `operator()(T)` overloads for POD, strings, vectors, maps, optional, pair, tuple,
  and types with `HasSerializeMethod<T, BinaryWriter/Reader>`.
- `WriteVersionedComponent<T>` / `ReadVersionedComponent<T>`: emit TypeID hash +
  version, then data via `SerializationTraits` or `HasSerializeMethod` fallback.
  These are **typed** to `BinaryWriter`/`BinaryReader` -- they call
  `SerializationTraits<T>::Serialize(*this, component)` where `*this` is always
  `BinaryWriter` or `BinaryReader`.

**`ComponentRegistry.hpp`**
- `ComponentDescriptor` carries four function pointers:
  ```cpp
  void (*serialize)(BinaryWriter& writer, void* ptr);
  void (*deserialize)(BinaryReader& reader, void* ptr);
  void (*serializeVersioned)(BinaryWriter& writer, void* ptr);
  bool (*deserializeVersioned)(BinaryReader& reader, void* ptr);
  ```
- These are CONCRETELY typed to `BinaryWriter`/`BinaryReader`. The static helpers
  `Serialize<T>`, `Deserialize<T>`, `SerializeVersioned<T>`, `DeserializeVersioned<T>`
  each hard-code one of the two concrete archive types.
- `Registry::Save()` drives all serialization through these pointers; it writes the
  `BinaryHeader` itself and is therefore binary-bound at the registry level.

**`Reflection/` system**
- `ASTRA_REFLECT_TYPE` / `ASTRA_REFLECT_FIELD` macros populate a `TypeMeta` in
  `MetaRegistry` with `FieldInfo` descriptors: name, nameHash, typeHash, offset,
  size, alignment, type-erased getter/setter, `std::any` getter/setter, attributes
  (`Range`, `Hidden`, `Serializable`, `DisplayName`, etc.).
- `FieldInfo::IsSerializable()` queries the `Serializable` attribute.
- `MetaRegistry::Instance().LinkToComponent(hash, id)` cross-links reflection to
  ECS. `InspectEntity(e)` returns `{descriptor, meta, data}` per component.
- There is a `JsonSchema` header (`Reflection/JsonSchema.hpp`) suggesting JSON output
  was considered from reflection, but it is NOT wired into `ComponentRegistry` or
  `Registry::Save`.

### The precise tension

The `HasSerializeMethod<T, Archive>` concept and `SerializationTraits<T>` template
are ALREADY archive-generic at the per-type level. A component can write:
```cpp
template<typename Archive>
void Serialize(Archive& ar) { ar(x); ar(y); }
```
and it would work with any archive that has `operator()(float)`.

But the registry's function pointers snap that genericity closed:
```cpp
void (*serialize)(BinaryWriter& writer, void* ptr);   // binary-only
```
So the Serialize code is author-once, but the DISPATCH path is binary-bound.
Adding a JSON archive would require adding a second set of function pointers to
`ComponentDescriptor` and a second pass in `Registry::Save`. The two mechanisms
(archive-generic `Serialize` concept, binary-bound `ComponentDescriptor` pointers)
are not unified -- types that use `SerializationTraits` for binary never get their
fields enumerated by `FieldInfo`, and the reflection system never participates in
`Registry::Save`.

---

## 2. AAA and Major ECS Serialization Patterns

### 2.1 Unreal: Polymorphic Archive (`FArchive`)

**Pattern:** One virtual `Serialize(FArchive& Ar)` per type. `FArchive` is an
abstract base with subclasses for: binary save/load (`.uasset`), net serialization,
size measurement, text export (`FTextArchive`), garbage collection reference walking.
The archive knows its mode (`IsLoading()`, `IsSaving()`, `IsNetArchive()`) and the
type code branches on `Ar.IsLoading()` to unify read/write.

**Reflection layer:** `UPROPERTY` + `UClass` metadata. The engine can serialize any
`UPROPERTY`-tagged field automatically without a `Serialize` override. Custom overrides
opt out of automatic and opt in to explicit control. Migration: `CustomVersions` are
stored in the archive header; type code checks `Ar.CustomVer(FMyVersion::GUID) >=
FMyVersion::AddedNewField` to gate new fields.

**Format split:** The SAME `Serialize` method writes `.uasset` (binary) and `.uexp`
text exports; only the archive subclass changes. Network serialization is also the
same method with `FArchive::IsNetArchive()` gating net-only fields.

**Essence:** polymorphic archive is the single-source-of-truth dispatch. Reflection
adds opt-in automatic serialization for simple types. Migration is version-guarded
inline code in the `Serialize` method itself.

### 2.2 Unity: Reflection-Driven Serialization

**Pattern:** No per-type `Serialize` code for ordinary MonoBehaviours. The serializer
uses reflection (`SerializedProperty`) to enumerate fields marked `[SerializeField]`
or public. Binary (`.assets`) and YAML text (`.scene`, `.prefab`) are both produced by
the same serializer reading the same reflection data. Custom serialization requires
implementing `ISerializationCallbackReceiver` (two hooks: `OnBeforeSerialize` /
`OnAfterDeserialize`) rather than writing a full archive method.

**Format split:** Binary vs YAML is an import setting, not a code change. The
serializer emits the format from the same field list.

**Migration:** limited. Unity's approach is "serialize what you know, discard what
you don't" for unknown fields (forward compat). Backward compat is handled by
`[FormerlySerializedAs]` attribute and `SerializeReference`.

**Essence:** reflection is the single source of truth. Code opt-in is for
non-standard types only (e.g. `Dictionary<K,V>` is not serializable by default).
Format is a backend concern, fully decoupled from type code.

### 2.3 Godot: Resource + Property System

**Pattern:** Every `Resource` class exposes `get_property_list()` / `get()` / `set()`
-- a dynamic property protocol. `.tres` text format and `.res` binary format are two
backends reading the same property list. GDScript and C++ classes participate through
the same protocol; `GDCLASS` macros register `get`/`set`/`get_property_list` at
compile time.

**Migration:** `_get_property_list()` can filter out old properties at runtime.
`property_usage` flags (`PROPERTY_USAGE_STORAGE`, `PROPERTY_USAGE_NO_INSTANCE_STATE`,
etc.) are per-property opt-outs.

**Essence:** a virtual property interface decouples format backends from type internals.
Types that want control override `get`/`set`; others rely on the macro-generated
implementations. Either way, format backends see the same property list.

### 2.4 ECS Libraries

**flecs:** The `ecs_meta` module stores full type descriptions (field names, types,
offsets, array sizes) for any component registered with `FLECS_META`. JSON serializer
(`ecs_ptr_to_json`) and expression serializer both operate purely on the meta
description. No per-component code for JSON output -- register the meta, get JSON
for free. Migration is manual (custom `ecs_move`/`ecs_copy` hooks).

**EnTT:** ships no built-in serializer. The `meta` system provides runtime type
reflection (by manual registration). Common idiom: integrate `cereal` or `nlohmann`
via SFINAE/concept-detected free functions `to_json`/`from_json` per type. The archive
pattern is user-controlled; EnTT's own snapshot API (`entt::snapshot` /
`entt::snapshot_loader`) is archive-agnostic (templates on user archive type), making
it similar to Astra's `HasSerializeMethod` concept but at the registry level.

**Bevy:** `Reflect` derive macro generates runtime reflection (field names, types,
partial-reflect getters). `Scene` format serializes components via `ReflectSerializer`
(reads from `Reflect` trait). Binary serialization for hot-reload uses the same trait
but via `serde::Serialize`. `TypeRegistry` maps type names to `ReflectSerialize`
impls. One component registration populates both reflection and all serializers.

**Dominant ECS consensus:** reflection/meta as single source of truth; serializers
are pure consumers of the meta description. Per-type `Serialize` overrides are
escape hatches for non-trivial types, not the primary path.

### 2.5 Summary Table

| Engine | Primary path | Per-type code | Format flexibility | Migration |
|--------|-------------|---------------|-------------------|-----------|
| Unreal | Polymorphic archive | `Serialize(FArchive&)` | Swap archive subclass | Inline version guards |
| Unity  | Reflection-driven | Opt-in ISerializationCallbackReceiver | Backend setting | `[FormerlySerializedAs]`, forward-compat skip |
| Godot  | Property protocol | Override `get/set` or macro | `.tres`/`.res` backends | Filter in `get_property_list` |
| flecs  | Meta description | Register meta, done | JSON / expr / binary from same meta | Manual hooks |
| EnTT   | User archive (template) | Per-type free functions | Archive is a template param | User-controlled |
| Bevy   | `Reflect` trait | Derive macro | `serde` + `ReflectSerialize` | Trait-based |

**The dominant pattern is: one registration (reflection/meta) drives all format
backends; per-type code is an escape hatch, not the default path.**

---

## 3. Diagnosis: What Is Hacky/Limiting in Astra Today

**Problem 1: Registry-level binary lock-in (`ComponentDescriptor` function pointers)**
The four pointers `serialize`, `deserialize`, `serializeVersioned`, `deserializeVersioned`
are typed to `BinaryWriter`/`BinaryReader`. Adding a JSON archive requires:
(a) adding four more function pointers per descriptor, (b) updating `RegisterComponentImpl`
for each new format, (c) maintaining N format slots as more formats are added.
This is not scalable and violates the "one registration" principle seen in every major
engine.

**Problem 2: Two parallel mechanisms that never meet**
The archive-generic `HasSerializeMethod<T, Archive>` concept lets a component author
write a single `template<typename Ar> void Serialize(Ar& ar)` that works with any
archive. The reflection system captures field names and offsets at registration time
with `IsSerializable()` per field. But these two systems never connect:
- `Registry::Save` drives through `ComponentDescriptor` pointers (binary-bound).
- `ASTRA_REFLECT` data is never consulted by `Registry::Save`.
- A JSON archive backed by `FieldInfo` would bypass `HasSerializeMethod` entirely.
They are independently useful but not unified into one truth.

**Problem 3: No type-erased archive abstraction**
There is no `IArchive` interface. Making the registry format-agnostic would require
either (a) an abstract archive type with virtual dispatch or (b) a template parameter
on the registry/save path. Currently there is neither.

**Problem 4: Migration lives only in binary**
`SerializationTraits<T>::Migrate(BinaryReader&, T&, uint32_t version)` is typed to
`BinaryReader`. If a JSON archive is added later, migration must be duplicated or
reimplemented.

**What is NOT a problem:**
- The per-component `Serialize` concept and `SerializationTraits` are well-designed.
  They are archive-generic at the type level. The problem is above them, in the
  registry dispatch layer.
- `BinaryWriter`/`BinaryReader` are fine for the binary jobs. They are fast, correct,
  and have versioning. They do not need to be replaced, only joined by peers.
- The `Serializable` attribute in `FieldInfo` correctly marks fields that should
  participate in all serialization. This is the right hook for a reflection-driven path.

---

## 4. Evolution Options

### Option A: Polymorphic Archive (Unreal-style)

**Description:** Introduce an abstract `IArchive` (or a concept) that defines the
interface (`IsLoading()`, `ReadBytes`/`WriteBytes`, `operator()` for primitives).
`BinaryWriter` and `BinaryReader` satisfy it. Add a `JsonWriter`/`JsonReader` that
also satisfies it.

Change `ComponentDescriptor`'s serialize pointers to accept `IArchive*` (or be
templated). Change `Registry::Save` to accept an `IArchive&` rather than hard-coding
a `BinaryWriter`. The existing `HasSerializeMethod<T, Archive>` concept already
handles per-component dispatch if the archive type is correct.

```cpp
// Before
void (*serialize)(BinaryWriter& writer, void* ptr);

// After (virtual dispatch)
void (*serialize)(IArchive& ar, void* ptr);

// Or (two-pointer approach: binary + json kept separate for perf):
void (*serializeBinary)(BinaryWriter& writer, void* ptr);
void (*serializeJson)(JsonWriter& writer, void* ptr);  // added alongside
```

**Serving the 3 jobs:**
- Snapshot/hot-reload: `BinaryWriter` archive, same path as today. No change.
- Editor text: `JsonWriter` archive, same `Registry::Save(jsonArchive)` call.
- Cross-language server: `JsonWriter` to produce/consume `nlohmann::json`.

**Versioning/migration:** version guards live in each component's `Serialize` method
(`if (ar.GetVersion() < 2) { ... }`). Migration is format-agnostic because it lives
in the component code.

**Astra change impact:** Medium.
- Add `IArchive` interface or concept.
- Change four function pointers in `ComponentDescriptor` to use `IArchive*`.
- Update `RegisterComponentImpl` (4 static helpers change signature).
- `BinaryWriter`/`BinaryReader` implement `IArchive` (or satisfy the concept) --
  largely just adding the interface declaration.
- Add `JsonWriter`/`JsonReader` implementing the same interface.
- `Registry::Save` signature changes to accept `IArchive&`; the binary header
  write stays as-is for the binary case or is skipped for JSON.
- Any component with a typed `Serialize(BinaryWriter&)` override must change to
  `template<typename Ar> void Serialize(Ar&)` or use `IArchive&`. Most components
  with trivially-copyable fields need no changes (POD fast path).
- `SerializationTraits` stays identical but its `Serialize` is called via
  `IArchive&` dispatch.

**Trade-offs:**
- (+) Single author-once `Serialize` per type covers all formats.
- (+) Clean path to add formats without touching existing component types.
- (+) Migration lives in one place (the type's Serialize method).
- (-) Virtual dispatch in `IArchive` operators costs a virtual call per field write.
  For the binary hot-reload path this is a regression. Mitigated by keeping the
  binary path separate (two pointer slots) or using CRTP/concept instead of virtual.
- (-) `operator()` overloads must be generalized. Currently `BinaryWriter::operator()`
  is non-virtual; a virtual `IArchive` approach requires redesign.
- (-) A concept-based approach avoids virtual cost but requires the descriptor
  function pointers to be templated -- which requires type-erasing the archive, which
  brings back the virtual call problem at a higher level.

**Recommended implementation:** a two-slot approach. Keep `serializeBinary(BinaryWriter&)` for
performance-critical binary; add `serializeReflected(TypedJsonWriter&)` as a second slot
populated from `FieldInfo` automatically (see Option B hybrid below). This avoids virtual
dispatch on the hot path while supporting JSON from a separate slot.

### Option B: Reflection-as-Truth (flecs/Bevy/Unity-style)

**Description:** The `ASTRA_REFLECT` system already captures field names, offsets,
sizes, and `Serializable` attributes. Add a format-agnostic serializer that walks
`TypeMeta::ForEachField`, reads/writes values via `FieldInfo::getter`/`setter`, and
produces JSON (or any other format). The `ComponentDescriptor` adds a single function
pointer:

```cpp
void (*serializeViaReflection)(void* instance, JsonWriter& out);
bool (*deserializeViaReflection)(void* instance, const JsonValue& in);
```

populated automatically from `FieldInfo` for any reflected type. No per-type
`Serialize` method needed for JSON -- only for types with non-trivial or non-standard
shapes (nested refs, asset handles).

The binary path (`serialize`/`deserialize` pointing to `BinaryWriter`) is kept as-is
for snapshots and hot reload. The reflection path is added ALONGSIDE it for editor
and cross-language uses.

```cpp
// In RegisterComponentImpl<T>:
if (desc.meta) {
    desc.serializeJson = &SerializeViaReflection<T>;
    desc.deserializeJson = &DeserializeViaReflection<T>;
}
```

`SerializeViaReflection<T>` walks `meta->ForEachField`, checks `IsSerializable()`,
uses `FieldInfo::GetAny(instance)` to read the value, and emits JSON keyed by
`field.name`. `DeserializeViaReflection<T>` reads JSON, looks up fields by name
hash, and calls `FieldInfo::SetAny(instance, value)`.

**Serving the 3 jobs:**
- Snapshot/hot-reload: binary path unchanged (fast, CRC-checked, versioned).
- Editor text: reflection path; output is named-field JSON, human-readable, diffable.
- Cross-language server: same reflection path; produces standard JSON that any
  language can consume without knowledge of the C++ type layout.

**Versioning/migration:** field-name-keyed JSON is naturally forward/backward compat.
Unknown fields in the JSON are silently skipped on load (forward compat for old
readers). Missing fields use the default-constructed value (backward compat for new
readers loading old files). Per-component `Serialize` overrides can opt into custom
JSON behavior by adding an `ASTRA_SERIALIZE_JSON` macro that installs a custom
function pointer instead of the auto-generated one.

**Astra change impact:** Low-Medium.
- No change to existing binary path, `BinaryWriter`, `BinaryReader`, or
  `HasSerializeMethod`.
- Add `JsonWriter`/`JsonReader` types (thin wrappers over `nlohmann::json` or
  similar) -- these live OUTSIDE `ComponentRegistry`; they are consumers of
  `FieldInfo`.
- Add two function pointer slots to `ComponentDescriptor`.
- Add `SerializeViaReflection<T>` / `DeserializeViaReflection<T>` static helpers
  in `RegisterComponentImpl`.
- Types that are NOT reflected (no `ASTRA_REFLECT_TYPE`) have null JSON pointers
  and fall back to... nothing (or use `HasSerializeMethod<T, JsonWriter>` as a
  manual escape hatch).
- `std::any` in `FieldInfo::getterAny`/`setterAny` handles primitive and struct
  fields; nested structs that are themselves reflected recurse cleanly.

**Trade-offs:**
- (+) Zero per-component code for the common case (reflected fields).
- (+) Human-readable output guaranteed (field names, not offsets).
- (+) Forward and backward compatible out of the box.
- (+) Binary path untouched -- hot-reload performance unchanged.
- (+) `FieldInfo::IsSerializable()` / `Hidden` attribute already does the right thing.
- (-) Types must be reflected (`ASTRA_REFLECT_TYPE`) to get JSON for free. Unreflected
  types produce nothing. This is an incentive to always reflect, not a blocker.
- (-) `std::any` in the setterAny path adds overhead per field on deserialization.
  Acceptable for editor load; unacceptable for game hot path (binary path is used there).
- (-) Nested compound fields (e.g. `glm::vec2`) need to themselves be registered in
  MetaRegistry for recursive descent, OR the JSON writer needs a fallback for
  registered POD structs (write as flat JSON array for known math types).
- (-) No single `Serialize` method across formats -- binary behavior and JSON behavior
  are defined in two separate places if customization is needed.

### Option C: Keep Binary for Runtime + Separate Reflection Layer for Editor/Server

**Description:** Do nothing to Astra's `Registry::Save` or `ComponentDescriptor`.
Add a SEPARATE `AstraJsonSerializer` utility (in the ENGINE, not in Astra core) that:
- Accepts a `Registry&` and a `ComponentRegistry&`.
- Iterates entities via views.
- For each component with a non-null `desc.meta`, walks `TypeMeta::ForEachField`
  and builds a `nlohmann::json` object.
- For components without meta, skips or emits a binary-base64 blob.

This is purely additive. Astra is not changed. The engine owns the JSON layer.

**Serving the 3 jobs:**
- Snapshot/hot-reload: `Registry::Save()` -- unchanged, fast.
- Editor text: `AstraJsonSerializer::SaveScene(registry, componentRegistry, path)` --
  new engine utility, not Astra core.
- Cross-language server: same utility; output is standard JSON.

**Versioning/migration:** same as Option B for the JSON layer (field-name-keyed, forward
compat by skip, backward compat by default). Binary versioning unchanged.

**Astra change impact:** None. Zero changes to Astra.

**Trade-offs:**
- (+) No Astra changes. Least risk.
- (+) Binary hot-reload path is completely isolated.
- (+) Engine can experiment with the JSON format before committing.
- (-) Two serialization mechanisms forever. "What is the truth?" becomes a question
  every time a component type changes. If a component has both a `Serialize(BinaryWriter&)`
  override AND a reflected JSON layer, they can drift.
- (-) Unreflected components are invisible to the JSON layer. The editor cannot
  inspect or save them. Pressure to reflect everything is good, but unreflected
  components produce silent gaps that are hard to diagnose.
- (-) If the team later wants "author component data in JSON and load it at startup"
  (the editor-data-as-truth flow), the engine-side utility must also handle JSON
  load into a registry -- which is essentially the same work as Option B but
  without the clean type-erased pointer slot in the descriptor.

---

## 5. Recommendation

**Adopt Option B (reflection-as-truth for text/editor formats) as the primary
evolution, with Option C as the zero-cost first step.**

**Phase 1 (no Astra changes, immediate): implement the engine-side JSON serializer
(Option C).** This unlocks editor data and cross-language use with zero risk. Write
`Arcane::SceneSerializer::SaveJson(registry, componentRegistry, path)` and
`LoadJson(...)`. Require all engine component types to be reflected. This validates
the design with real types before touching Astra.

**Phase 2 (minor Astra change): add two JSON function pointer slots to
`ComponentDescriptor` (Option B proper).** Once the engine-side serializer is
proven, move `SerializeViaReflection<T>` / `DeserializeViaReflection<T>` into
`RegisterComponentImpl`, so the JSON capability is registered at the same time as
the binary capability. This makes the JSON path first-class and eliminates the
engine-side iteration-over-all-archetypes hack.

**Do NOT pursue Option A (polymorphic archive) as the primary approach** for this
project's scale. The virtual-dispatch cost on the binary hot-reload path is a
regression, and the effort of making every `operator()` overload virtual is
disproportionate. The binary and JSON jobs are different enough in structure (binary
is offset-indexed, JSON is name-keyed) that a single `Serialize` method that does
both cleanly is harder to write than two separate representations derived from one
type registration.

**However, borrow ONE idea from Option A:** the `HasSerializeMethod<T, Archive>` concept
already exists and is archive-generic. For types that need custom JSON serialization
(e.g. asset handles, entity references), add a convention:

```cpp
template<typename Ar>
void Serialize(Ar& ar);   // binary path

// JSON escape hatch:
void SerializeJson(JsonWriter& out) const;
void DeserializeJson(const JsonValue& in);
```

These are distinct named methods (not archive-generic overloading), because the
binary and JSON layouts legitimately differ for complex types. The reflection-driven
path is the default; these methods are opt-out overrides.

### Migration story

- **Binary format:** unchanged. `SerializationTraits<T>::Version` + `Migrate` cover
  binary evolution. Hot-reload survives layout changes via the versioning already
  in place.
- **JSON format (editor files):** field-name-keyed JSON is structurally forward/backward
  compat. Add new fields = old readers skip them. Remove fields = new readers use
  default. Rename fields = add `[FormerlySerializedAs]`-style attribute (`AliasName`)
  to `FieldInfo` so the JSON loader tries both names. This is a tiny attribute addition
  to `Attribute.hpp`.
- **Cross-language server:** JSON is the lingua franca. The reflection-generated JSON
  schema (`JsonSchema.hpp` already exists in Astra) can be emitted to a file so the
  Lua client and the C++ server agree on field names. One schema per component type,
  versioned by `SerializationTraits<T>::Version`.

### Summary of recommended changes to Astra (Phase 2)

| File | Change |
|------|--------|
| `Component/ComponentDescriptor.hpp` | Add `serializeJson` / `deserializeJson` function pointer slots |
| `Component/ComponentRegistry.hpp` | Populate JSON slots from `TypeMeta` in `RegisterComponentImpl<T>`; no-op if `desc.meta == nullptr` |
| `Reflection/Attribute.hpp` | Add `AliasName` attribute (optional; for field renames in JSON) |
| `Reflection/JsonSchema.hpp` | Already exists; ensure it is wired to `TypeMeta::ForEachField` with `IsSerializable()` filtering |

No changes to `BinaryWriter`, `BinaryReader`, `BinaryArchive`, `Registry::Save`,
or any existing component's `Serialize` method.

---

## 6. File References

| File | Relevance |
|------|-----------|
| `ThirdParty/Astra/include/Astra/Serialization/BinaryArchive.hpp` | `BinaryArchive` base, `HasSerializeMethod` concept, `SerializationTraits` |
| `ThirdParty/Astra/include/Astra/Serialization/BinaryWriter.hpp` | `BinaryWriter`: typed to concrete archive, `WriteVersionedComponent` |
| `ThirdParty/Astra/include/Astra/Serialization/BinaryReader.hpp` | `BinaryReader`: typed to concrete archive, `ReadVersionedComponent` |
| `ThirdParty/Astra/include/Astra/Component/ComponentRegistry.hpp` | Binary-bound function pointers; `RegisterComponentImpl`; `MetaRegistry::LinkToComponent` |
| `ThirdParty/Astra/include/Astra/Reflection/Reflection.hpp` | Entry point; pulls in FieldInfo, TypeMeta, MetaRegistry, JsonSchema |
| `ThirdParty/Astra/include/Astra/Reflection/FieldInfo.hpp` | Per-field descriptor with typed getter/setter, `IsSerializable()`, `std::any` accessors |
| `ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp` | `Serializable`, `Hidden`, `ReadOnly`, `DisplayName` attributes |
| `ThirdParty/Astra/include/Astra/Reflection/JsonSchema.hpp` | JSON schema generation (already exists, not yet wired to Registry::Save) |
| `docs/superpowers/research/2026-06-13-arcane-scene-system-research.md` | Section 1.4: full registry serialization context, hot-reload sequence |
