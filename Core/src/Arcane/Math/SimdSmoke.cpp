// Pilot: a real Core TU that instantiates the SIMD wrapper, so both Core flavors
// (Arcane.dll /MD and ArcaneCore static-CRT) compile Simd.hpp. No real behavior;
// this keeps the header building inside the engine until the SIMD solver (next
// spec) becomes its real consumer.
#include <Arcane/Math/Simd.hpp>
#include <Arcane/Math/SimdSmoke.hpp>

namespace Arcane { namespace Simd {

float SimdSmokeSum(const float* p, int count) noexcept
{
    f32w acc = setzero();
    int i = 0;
    for (; i + f32w::width <= count; i += f32w::width)
        acc += loadu(p + i);
    alignas(32) float lanes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    store(lanes, acc);                        // lanes is alignas(32) -> aligned store valid for width <= 8
    float sum = 0.0f;
    for (int k = 0; k < f32w::width; ++k) sum += lanes[k];
    for (; i < count; ++i) sum += p[i];       // scalar tail
    return sum;
}

} } // namespace Arcane::Simd
