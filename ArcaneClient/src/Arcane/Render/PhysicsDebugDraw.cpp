#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/Batcher2D.hpp>

namespace Arcane
{
    PhysicsDebugDrawOptions2D MakePhysicsDebugDrawOptions()
    {
        PhysicsDebugDrawOptions2D o;
        static_cast<PhysicsDebugSettings2D&>(o) = Settings<PhysicsDebugSettings2D>();
        static_cast<PhysicsDebugDrawSettings2D&>(o) = Settings<PhysicsDebugDrawSettings2D>();
        static_cast<PhysicsDebugColorSettings2D&>(o) = Settings<PhysicsDebugColorSettings2D>();
        static_cast<PhysicsDebugTraceSettings2D&>(o) = Settings<PhysicsDebugTraceSettings2D>();
        return o;
    }

    static void DrawGeometry(Batcher2D& batcher, const PhysicsDebugGeometry& geometry)
    {
        for (const PhysicsDebugPrimitive& primitive : geometry.primitives)
        {
            if (primitive.kind == PhysicsDebugPrimitive::Kind::Line)
                batcher.Line(primitive.a, primitive.b, primitive.size, primitive.color);
            else
                batcher.Circle(primitive.a, primitive.size, primitive.color);
        }
    }

    void DrawPhysicsDebug(const Manifold2D::Physics::PhysicsWorld& world,
                          Batcher2D& batcher, const PhysicsDebugDrawOptions2D& opts)
    {
        DrawGeometry(batcher, GatherPhysicsDebug(world, opts, Settings<PhysicsDebugStyleSettings2D>()));
    }

    void DrawNarrowphaseWorldOverlay(const Manifold2D::Physics::NarrowphaseTrace& trace,
                                     int stepIndex, Batcher2D& batcher, const Affine2D& view,
                                     std::optional<float> lineThickness, std::optional<float> emphasis)
    {
        DrawGeometry(batcher, GatherNarrowphaseWorldOverlay(trace, stepIndex, view,
            lineThickness, emphasis, Settings<PhysicsDebugTraceSettings2D>(),
            Settings<PhysicsDebugColorSettings2D>(), Settings<PhysicsDebugStyleSettings2D>()));
    }
}
