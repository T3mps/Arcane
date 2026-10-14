#pragma once
#include <Arcane/Reflection.hpp>
#include <glm/vec3.hpp>

#if !defined(ARC_BUILD_DIST)

struct ArcDemoArgs {
    int n = 3;
    glm::vec3 position{0.f, 0.f, 0.f};
};
ARC_REFLECT_TYPE(ArcDemoArgs)
    ARC_REFLECT_FIELD(ArcDemoArgs, n)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(ArcDemoArgs, position)
ARC_REFLECT_TYPE_END()

#endif
