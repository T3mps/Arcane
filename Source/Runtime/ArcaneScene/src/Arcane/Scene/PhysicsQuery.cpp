#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    namespace
    {
        // Written only by image-load registrars; read-only once any thread runs Extract.
        PhysicsDebugFillFn2D     g_debugFill2D = nullptr;
        PhysicsSilhouetteFillFn2D g_silhouetteFill2D = nullptr;
    }

    bool SetPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept
    {
        const bool empty = g_debugFill2D == nullptr && g_silhouetteFill2D == nullptr;
        if (!empty && (g_debugFill2D != debug || g_silhouetteFill2D != silhouettes))
        {
            ARC_ERROR("SetPhysicsQueryFns2D: refused -- a different 2D physics query pair is already registered");
            return false;
        }
        g_debugFill2D = debug;
        g_silhouetteFill2D = silhouettes;
        return true;
    }

    PhysicsDebugGeometry FillPhysicsDebug2D(const Astra::Registry& reg, const PhysicsDebugExtractRequest2D& request)
    {
        return g_debugFill2D ? g_debugFill2D(reg, request) : PhysicsDebugGeometry{};
    }

    void FillColliderSilhouettes2D(const Astra::Registry& reg, std::vector<PhysicsColliderSilhouette2D>& out)
    {
        if (g_silhouetteFill2D) g_silhouetteFill2D(reg, out);
    }
}
