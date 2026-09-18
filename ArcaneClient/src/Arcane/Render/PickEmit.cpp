#include <Arcane/Render/PickEmit.hpp>

#include <Arcane/Render/SpriteGeometry.hpp>   // SpriteWorldQuad -- THE sprite corner rule
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>   // std::as_const

namespace Arcane
{
    namespace
    {
        // Rotate a body-local offset by the body's world angle (radians).
        glm::vec2 RotateVec(glm::vec2 v, float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::vec2(v.x * c - v.y * s, v.x * s + v.y * c);
        }
    }

    void CollectPickables(Astra::Registry& registry, std::vector<PickDrawable>& out)
    {
        // ---- PASS 1: sprites -------------------------------------------------
        // The DRAWN set, through the DRAWN corner rule. The view filter is
        // RenderSystems.hpp's own (a Hidden sprite is not drawn, so it is not
        // pickable either), and base size + pivot come from the SpriteTable
        // exactly as submission resolves them -- an unresolved or non-Rect
        // sprite is a 1x1 m quad at the centre pivot -- so the silhouette
        // keeps matching the drawn quad.
        {
            const SpriteTable* spriteTable = registry.GetResource<SpriteTable>();
            auto spriteView = registry.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
            spriteView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const SpriteRenderer& sp)
            {
                const SpriteEntry* entry =
                    (sp.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sp.sprite)
                        : nullptr;
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);

                // THE ONE CORNER RULE (SpriteGeometry.hpp): the same four world
                // points the sprite submission draws, so the silhouette matches
                // the drawn quad under ANY projection -- a mirrored or off-centre
                // pivot included (the retired "centre + angle" derivation placed
                // a mirrored off-centre pivot on the wrong side).
                PickDrawable d;
                d.entity  = e;
                d.kind    = PickDrawable::Kind::Quad;
                d.corners = SpriteWorldQuad(xf.matrix, baseSize, pivot).corners;
                out.push_back(d);
            });
        }

        // ---- PASS 2: physics colliders ---------------------------------------
        // One PickDrawable per Fixture on every live tracked body, iterated via
        // an archetype-stable View<Collider2D, PhysicsBodyRef> -- NOT the
        // PhysicsResource::entityToBody unordered_map. The drawable index IS the
        // hit-proxy id (id = index+1), so the order must be DETERMINISTIC: the
        // same rule PhysicsSystem's create pass follows ("order must not depend on
        // unordered_map hash/bucket layout"). The body pose is read from the live
        // PhysicsWorld via PhysicsBodyRef::handle so the silhouette registers with
        // the physics-debug overlay; fixture dims + local offset are scaled by
        // PhysicsBodyRef::appliedScale (the scale the create pass baked into the
        // body's fixtures, mirroring MakeScaledShape / MakeFixtureDef) so a scaled
        // body picks at its drawn size. Polygon fixtures carry no authored vertex
        // array (see PhysicsComponents.hpp) -- v1 approximates with the fixture's
        // halfW/halfH box, per design spec SS8.2 ("Polygon -> Box using its AABB").
        //
        // No physics world on this registry means no colliders -- NOT an early
        // return: the meshes below are collected regardless.
        if (PhysicsResource* res = registry.GetResource<PhysicsResource>(); res && res->world)
        {
            Phys::PhysicsWorld& world = *res->world;

            auto colliderView = registry.CreateView<const Collider2D, const PhysicsBodyRef>();
            colliderView.ForEach([&](Astra::Entity entity, const Collider2D& col, const PhysicsBodyRef& ref)
            {
                if (ref.handle == Phys::kInvalidBody) return;
                if (!world.IsValid(ref.handle))       return;

                const Phys::Vec2 bp = world.Position(ref.handle);
                const glm::vec2  bodyPos(static_cast<float>(bp.x), static_cast<float>(bp.y));
                const float      bodyAngle = static_cast<float>(world.GetAngle(ref.handle));

                // The silhouette's world z: the solver is planar, so it comes
                // from the entity's own WorldTransform (its translation column)
                // -- what places the body's sprite -- and is 0 without one.
                // Looked up ONCE per entity, not per fixture.
                const WorldTransform* xf = std::as_const(registry).GetComponent<WorldTransform>(entity);
                const float worldZ = xf ? xf->matrix[3].z : 0.0f;

                // Scale the create pass baked into this body's fixtures (identity
                // unless the entity carries an authored Transform.scale). Mirrors
                // PhysicsSystem::MakeScaledShape: per-axis for Aabb, |sx| length /
                // |sy| radius for Capsule, max(|sx|,|sy|) for Circle.
                const glm::vec2 scale = ref.appliedScale;
                const float     sx    = std::abs(scale.x);
                const float     sy    = std::abs(scale.y);
                const float     sMax  = std::max(sx, sy);

                for (const Fixture& fx : col.fixtures)
                {
                    // Fixture local offset scales per-axis with the body's baked scale
                    // (signed, matching MakeFixtureDef), then rotates into world space.
                    const glm::vec2 localScaled(fx.localPos.x * scale.x, fx.localPos.y * scale.y);
                    const glm::vec2 worldCenter  = bodyPos + RotateVec(localScaled, bodyAngle);
                    const float     fixtureAngle = bodyAngle + fx.localAngle;

                    // WORLD space, METRES, world-sense angle: nothing here knows
                    // the camera. The id pass projects.
                    PickDrawable d;
                    d.entity = entity;
                    d.center = glm::vec3(worldCenter, worldZ);
                    d.angle  = fixtureAngle;

                    switch (fx.kind)
                    {
                    case Phys::ShapeKind::Circle:
                        d.kind   = PickDrawable::Kind::Circle;
                        d.radius = fx.radius * sMax;
                        break;
                    case Phys::ShapeKind::Capsule:
                        d.kind    = PickDrawable::Kind::Capsule;
                        d.halfLen = fx.halfLen * sx;
                        d.radius  = fx.radius  * sy;
                        break;
                    case Phys::ShapeKind::Aabb:
                        d.kind        = PickDrawable::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    case Phys::ShapeKind::Polygon:
                        // v1: no vertex data available -- fall back to the fixture's
                        // halfW/halfH box fields (scaled) as its AABB stand-in.
                        d.kind        = PickDrawable::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    }

                    out.push_back(d);
                }
            });
        }

        // ---- PASS 3: meshes -------------------------------------------------
        // One drawable per MeshRenderer entity that CollectMeshInstances would draw
        // (Render/MeshSubmissionSystem.hpp: WorldTransform + MeshRenderer, not Hidden,
        // resolved through the MeshTable to a mesh with sections). The whole mesh is
        // one silhouette -- sections carry materials, not identity -- so an entity
        // gets ONE id whatever its section count.
        {
            const MeshTable* meshTable = registry.GetResource<MeshTable>();
            auto meshView = registry.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>();
            meshView.ForEach([&](Astra::Entity e, const WorldTransform& xf, const MeshRenderer& mr)
            {
                const MeshEntry* entry = meshTable ? meshTable->Resolve(mr.mesh) : nullptr;
                if (!entry || entry->data.sections.empty())
                    return;
                PickDrawable d;
                d.entity = e;
                d.kind   = PickDrawable::Kind::Mesh;
                d.world  = xf.matrix;
                d.mesh   = mr.mesh;
                out.push_back(d);
            });
        }
    }

    uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < ordered.size(); ++k)
            if (ordered[k] == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }

    glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)
    {
        const int s = (int)ss;
        int x = (int)std::floor(pixel1x.x) * s + s / 2;
        int y = (int)std::floor(pixel1x.y) * s + s / 2;
        x = std::clamp(x, 0, (int)idW - 1);
        y = std::clamp(y, 0, (int)idH - 1);
        return glm::ivec2(x, y);
    }

    // ------------------------------------------------------------------
    // The id pass's geometry -- ONE emitter, so no recorder can assign ids a
    // different way. See the header's block comment.
    // ------------------------------------------------------------------

    uint32_t PickKindCode(PickDrawable::Kind kind)
    {
        switch (kind)
        {
        case PickDrawable::Kind::Quad:    return 0u;
        case PickDrawable::Kind::Circle:  return 1u;
        case PickDrawable::Kind::Capsule: return 2u;
        case PickDrawable::Kind::Box:     return 3u;
        case PickDrawable::Kind::Mesh:    return 4u;   // never reaches the 2D path
        }
        return 0u;
    }

    glm::vec2 PickBoundHalfExtents(const PickDrawable& d)
    {
        switch (d.kind)
        {
        case PickDrawable::Kind::Circle:  return glm::vec2(d.radius, d.radius);
        case PickDrawable::Kind::Capsule: return glm::vec2(d.halfLen + d.radius, d.radius);
        case PickDrawable::Kind::Box:     return d.halfExtents;
        case PickDrawable::Kind::Quad:
            // Half the TL->TR and TL->BL edge lengths: the corners are the
            // geometry, so the bound is derived, never authored.
            return glm::vec2(0.5f * glm::length(d.corners[1] - d.corners[0]),
                             0.5f * glm::length(d.corners[3] - d.corners[0]));
        case PickDrawable::Kind::Mesh:
        default:                          return glm::vec2(0.0f);
        }
    }

    void BuildPickIdGeometry(std::span<const PickDrawable> drawables,
                             std::vector<PickIdVertex>& outVertices,
                             std::vector<uint32_t>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        // Bounding-quad corner sign pattern for the physics shapes: one loop
        // around the bound, (-,-) (+,-) (+,+) (-,+), so the same two-triangle
        // index pattern a sprite Quad's TL,TR,BR,BL corners use serves both.
        // Winding is irrelevant: the 2D id pipeline culls nothing.
        static const glm::vec2 kSigns[4] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

        for (std::size_t di = 0; di < drawables.size(); ++di)
        {
            const PickDrawable& d = drawables[di];
            const uint32_t id     = (uint32_t)di + 1u;   // 1-based

            // A Mesh drawable has no quad: the pick node rasterises its resident
            // triangles through the second pipeline. It KEEPS its index, so
            // the id the mesh path writes and the ids this path writes share
            // one numbering.
            if (d.kind == PickDrawable::Kind::Mesh)
                continue;

            const uint32_t code   = PickKindCode(d.kind);
            const uint32_t base   = (uint32_t)outVertices.size();

            if (d.kind == PickDrawable::Kind::Quad)
            {
                // The four world corners VERBATIM. Kind code 0 covers the
                // whole quad in the PS, so `local` is unused (zero).
                for (int i = 0; i < 4; ++i)
                {
                    PickIdVertex v;
                    v.pos     = d.corners[static_cast<std::size_t>(i)];
                    v.local   = glm::vec2(0.0f);
                    v.radius  = 0.0f;
                    v.halfLen = 0.0f;
                    v.kind    = code;
                    v.id      = id;
                    outVertices.push_back(v);
                }
            }
            else
            {
                // Rotate the bounding quad by the drawable's world angle in the
                // XY plane about its centre. The PS coverage test uses the
                // UNROTATED `local`.
                const glm::vec2 bound = PickBoundHalfExtents(d);
                const float c = std::cos(d.angle);
                const float s = std::sin(d.angle);
                for (int i = 0; i < 4; ++i)
                {
                    const glm::vec2 local(kSigns[i].x * bound.x, kSigns[i].y * bound.y);
                    const glm::vec2 rot(c * local.x - s * local.y,
                                        s * local.x + c * local.y);
                    PickIdVertex v;
                    v.pos     = d.center + glm::vec3(rot, 0.0f);
                    v.local   = local;
                    v.radius  = d.radius;
                    v.halfLen = d.halfLen;
                    v.kind    = code;
                    v.id      = id;
                    outVertices.push_back(v);
                }
            }

            const uint32_t quad[6] = { base, base + 1, base + 2,
                                       base, base + 2, base + 3 };
            outIndices.insert(outIndices.end(), quad, quad + 6);
        }
    }
}
