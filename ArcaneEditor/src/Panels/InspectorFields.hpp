#pragma once

// Reflection-driven Inspector: pure field classification + write-backs (no
// ImGui), so the round-trip is unit-testable headlessly. See InspectorView.cpp
// for the Astra::IFieldVisitor that drives these against a live component.

#include <Arcane/Guid.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Astra/Reflection/EnumInfo.hpp>
#include <Astra/Reflection/FieldInfo.hpp>

// glm::quat/glm::vec3, for the Quat field's Euler-view helpers below. Nothing
// else in this header needed glm before -- FieldKind's Vec2/Vec3/Vec4 arms are
// only ever named by enumerator, never by the glm type itself, at this scope.
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    // AssetRef = an Arcane::Guid field: rendered as an asset-reference widget
    // (resolved name + pick popup + browser drag-target) instead of raw ints.
    // Quat = a glm::quat field: edited as three Euler-angle drags -- see the
    // QuatEulerView section below for the "Euler is a VIEW, the quaternion is
    // the STORAGE" contract that field type has to obey.
    enum class FieldKind
    { Bool, Int32, UInt32, Float, Vec2, Vec3, Vec4, Quat, AssetRef, String, Enum, ReadOnly };

    // Classify a reflected field into an editor kind. Unknown/compound types ->
    // ReadOnly (shown disabled, never crashing). Enum requires the enum type to
    // be ASTRA_REFLECT_ENUM-registered -- an unregistered enum has no names to
    // offer and stays ReadOnly.
    FieldKind ClassifyField(const Astra::FieldInfo& f) noexcept;

    // The registered enum metadata for an enum-typed field, or nullptr. The
    // lookup key is the FIELD's typeHash -- ASTRA_REFLECT_ENUM registers a
    // TypeMeta under the same TypeID hash FieldInfo records.
    [[nodiscard]] const Astra::EnumInfo* EnumInfoOf(const Astra::FieldInfo& f) noexcept;

    // Read an enum field's numeric value at its TRUE width. SpriteShape is
    // uint8-backed: Get<int32_t> would assert (hash mismatch) and a raw 4-byte
    // read would pull neighbouring bytes into the value. Sign-extends when the
    // registered metadata says the underlying type is signed. 0 for a field
    // with no registered enum (callers gate on EnumInfoOf first).
    [[nodiscard]] std::int64_t ReadEnumValue(const Astra::FieldInfo& f,
                                             const void* instance) noexcept;

    // Whether a vec4 field is a COLOR (swatch + 0..1 alpha editing) rather
    // than a plain 4-float. Decided from the C++ identifier -- "tint",
    // "color", "baseColor" -- the same documented-name-heuristic pattern as
    // AssetKindFilterForFieldName (AssetBrowser.hpp): the engine has no
    // dedicated color type, and until one exists the field NAME is the only
    // author intent on record.
    [[nodiscard]] bool IsColorFieldName(std::string_view rawFieldName) noexcept;

    // ---- glm::quat: Euler is a VIEW, the quaternion is the STORAGE --------
    //
    // The classic inspector bug: re-deriving Euler angles from the stored
    // quaternion on EVERY display frame makes an untouched rotation jitter
    // (pitch/roll silently re-picking a different-but-equivalent decomposition
    // purely from float noise) and makes editing one axis rewrite the other
    // two. The fix kept here is that the widget's Euler triple is UI STATE
    // (QuatEulerView) that is re-derived from the quaternion ONLY when the
    // quaternion changed for a reason other than the view's own last write --
    // never on a plain display frame. SyncQuatEulerView/ApplyQuatEulerEdit are
    // the whole contract; ToEuler/FromEuler below are the pure math they are
    // built from. All of this is ImGui-free and is what the [editor] suite
    // drives directly -- InspectorView.cpp's Quat case only ever calls these,
    // and is desk-verified, the same split as every other field kind here.

    // Euler order: pitch (X), yaw (Y), roll (Z) -- glm::eulerAngles' own
    // component order, and glm::quat(vec3)'s constructor
    // (ThirdParty/glm/glm/detail/type_quat.inl:220-229) treats its argument
    // the same way, so the pair is a genuine round-trip: FromEulerRadians(
    // ToEulerRadians(q)) reconstructs the SAME rotation (up to the
    // quaternion's own +-q double cover, e.g. a 180 degree turn) for
    // identity, every single-axis 90/180 degree rotation, compound rotations,
    // and pitch = 90 degree (gimbal-locked) cases -- measured directly before
    // committing to this pair rather than assumed; see the round-trip test in
    // EditorInspectorQuatTest.cpp.
    [[nodiscard]] glm::vec3 QuatToEulerRadians(const glm::quat& q) noexcept;

    // The half-angle cos/sin products already form a unit quaternion
    // algebraically; the normalize is a defensive, near-zero-cost guard
    // against FP drift on repeatedly-edited or very large inputs, not a
    // correction for a known error source.
    [[nodiscard]] glm::quat QuatFromEulerRadians(const glm::vec3& eulerRadians) noexcept;

    // The per-row UI state backing a single-selection Quat field: the Euler
    // triple currently on screen, and the quaternion it was last synced
    // against (derived FROM, in SyncQuatEulerView, or derived INTO, in
    // ApplyQuatEulerEdit). Lives on InspectorState (EditorPanels.hpp), keyed
    // per field -- see InspectorView.cpp -- because the field VISITOR is
    // rebuilt every frame and cannot itself carry cross-frame state (the same
    // reason EditGesture::GestureState/originalColor live there instead of on
    // the visitor).
    //
    // `eulerDisplay` holds whatever unit the caller's chosen entry-point pair
    // uses -- radians for SyncQuatEulerView/ApplyQuatEulerEdit, degrees for
    // SyncQuatEulerViewDegrees/ApplyQuatEulerEditDegrees (below). A single
    // QuatEulerView is only ever driven by ONE pair for its lifetime in
    // practice: it is keyed per reflected FIELD (InspectorView.cpp's
    // quatKey), and a field's AngleFormat attribute is a compile-time reflect
    // annotation that cannot flip at runtime, so the unit is unambiguous per
    // instance even though the struct itself does not tag which one is live.
    struct QuatEulerView
    {
        glm::vec3 eulerDisplay{0.0f};
        glm::quat lastQuat{1.0f, 0.0f, 0.0f, 0.0f};
        bool      valid = false;
    };

    // Returns the Euler triple (radians) the widget should display THIS
    // frame. If `liveQuat` still represents the SAME rotation (within
    // tolerance, honouring the double cover) as the quaternion `view` was
    // last synced against, the cached triple is returned UNCHANGED -- no
    // re-derivation, so a display-only frame can never silently jump to a
    // different-but-equivalent Euler solution. Only a genuine external change
    // to the field (the first call, undo/redo, scene load, a script write)
    // re-derives via QuatToEulerRadians.
    [[nodiscard]] glm::vec3 SyncQuatEulerView(QuatEulerView& view,
                                              const glm::quat& liveQuat) noexcept;

    // Applies a user edit of the FULL Euler triple (as currently shown -- the
    // caller edits the array SyncQuatEulerView handed back IN PLACE, so axes
    // the user did not touch arrive here bit-identical to what was displayed)
    // into a new quaternion, and updates `view` so the NEXT SyncQuatEulerView
    // call sees no external change and keeps showing exactly
    // `newEulerRadians` -- rather than re-deriving it from the new
    // quaternion and risking a different (also valid) decomposition. Returns
    // the quaternion to write into storage.
    [[nodiscard]] glm::quat ApplyQuatEulerEdit(QuatEulerView& view,
                                               const glm::vec3& newEulerRadians) noexcept;

    // ---- Degrees-cached cousins of the two above ---------------------------
    //
    // Directive (2026-08-22): the engine stores radians; the editor SHOWS
    // degrees; degrees are purely a display artifact with NO live conversion.
    // SyncQuatEulerView/ApplyQuatEulerEdit above cache the triple in radians,
    // which is right for a Radians-attributed field -- but converting that
    // radian cache to degrees for every draw and back for every commit (which
    // is what the ImGui call site used to do) sends the two axes the user did
    // NOT touch through a degrees<->radians round trip on every single edit.
    // That is a live conversion, and the directive rules it out even though
    // it is lossy only in the ~1e-7 radian ulp sense.
    //
    // These two cache the triple in DEGREES directly -- re-deriving via
    // QuatToEulerRadians + glm::degrees() only on a genuine external change
    // (same gate as SyncQuatEulerView, via QuatNearlySameRotation), and
    // composing via glm::radians() + QuatFromEulerRadians only when the user
    // actually edits an axis. An untouched axis is therefore never converted
    // by anything, not even losslessly: it is the literal float last shown.

    // Degrees analogue of SyncQuatEulerView.
    [[nodiscard]] glm::vec3 SyncQuatEulerViewDegrees(QuatEulerView& view,
                                                     const glm::quat& liveQuat) noexcept;

    // Degrees analogue of ApplyQuatEulerEdit. `newEulerDisplayDegrees` is the
    // full triple as currently shown (degrees) -- axes the caller did not
    // edit arrive here bit-identical to what SyncQuatEulerViewDegrees handed
    // back, exactly as ApplyQuatEulerEdit documents for its radian argument.
    [[nodiscard]] glm::quat ApplyQuatEulerEditDegrees(QuatEulerView& view,
                                                      const glm::vec3& newEulerDisplayDegrees) noexcept;

    // Multi-select's one-shot cousin of ApplyQuatEulerEdit: multi-select rows
    // are plain text-commit boxes re-seeded from live storage every frame
    // (MultiScalarRow), never a continuous drag, so there is no cross-frame
    // view to protect and no cache to keep. This decomposes `liveQuat`
    // FRESH, overwrites Euler component `axis` (0 = pitch, 1 = yaw, 2 = roll;
    // any other value is a no-op read-back), and recomposes -- one commit,
    // one entity.
    [[nodiscard]] glm::quat QuatWithEulerAxisRadians(const glm::quat& liveQuat, int axis,
                                                     float newValueRadians) noexcept;

    // Pure write-backs (no ImGui) so the round-trip is unit-testable.
    void ApplyBoolEdit (const Astra::FieldInfo& f, void* instance, bool  v) noexcept;
    void ApplyIntEdit  (const Astra::FieldInfo& f, void* instance, int   v) noexcept;
    void ApplyUIntEdit (const Astra::FieldInfo& f, void* instance, std::uint32_t v) noexcept;
    void ApplyFloatEdit(const Astra::FieldInfo& f, void* instance, float v) noexcept;
    void ApplyGuidEdit (const Astra::FieldInfo& f, void* instance, const Arcane::Guid& v) noexcept;
    void ApplyStringEdit(const Astra::FieldInfo& f, void* instance, const std::string& v) noexcept;
    // Width-correct enum write (see ReadEnumValue): stores the low `f.size`
    // bytes of `v`, leaving neighbouring struct bytes untouched.
    void ApplyEnumEdit (const Astra::FieldInfo& f, void* instance, std::int64_t v) noexcept;

    // Per-scalar-component "these differ across the selection" mask.
    //
    // Mirrors Unreal's FComponentTransformDetails::CacheDetails (vendored at
    // Arcane/.example/UnrealEngine-release/Engine/Source/Editor/
    // DetailCustomizations/Private/ComponentTransformDetails.cpp:1215-1225):
    // the first entity carrying the component seeds the values, every later
    // entity marks each component that differs, and a marked component STAYS
    // marked (UE's `&& Cached<X>.IsSet()` term). UE states the convention at
    // :1026 -- "unset means multiple differing values" -- and renders those
    // blank.
    struct FieldMixedMask
    {
        std::uint32_t bits = 0;   // bit i set = scalar component i differs
        [[nodiscard]] bool Any() const noexcept { return bits != 0; }
        [[nodiscard]] bool Test(int i) const noexcept { return (bits >> i) & 1u; }
    };

    // Scalar (raw storage) components a kind occupies: Vec4/Quat = 4, Vec3 = 3,
    // Vec2 = 2, everything else 1. See the .cpp's switch for why Quat counts
    // its 4 raw components rather than the 3 Euler boxes the widget shows.
    [[nodiscard]] int FieldComponentCount(FieldKind kind) noexcept;

    // `componentHash` is the OWNING component's descriptor hash, used to fetch
    // each entity's instance. Entities that are dead or lack the component are
    // skipped -- same rule as the Inspector's fan-out -- so they can never make
    // a field look mixed. Fewer than two live carriers is never mixed.
    [[nodiscard]] FieldMixedMask ComputeFieldMixed(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::uint64_t componentHash,
        const Astra::FieldInfo& f);
}
