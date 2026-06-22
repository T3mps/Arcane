#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    // Andrew's monotone chain. `pts` MUST be lexicographically sorted + deduped
    // (the wrapper guarantees this; Chan also slices the sorted set, so slices stay
    // sorted). Returns a CCW boundary; `<= 0` pops keep it to minimal vertices.
    // Handles 1- and 2-point inputs (returns the point / the segment) so Chan can
    // call it on tiny groups.
    template <class T>
    std::vector<Pt<T>> MonotoneChainBuild(std::span<const Pt<T>> pts)
    {
        const std::size_t n = pts.size();
        if (n <= 2) return std::vector<Pt<T>>(pts.begin(), pts.end());

        std::vector<Pt<T>> h(2 * n);
        std::size_t k = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            while (k >= 2 && Cross<T>(h[k - 2], h[k - 1], pts[i]) <= T(0)) --k;
            h[k++] = pts[i];
        }
        const std::size_t lower = k + 1;
        for (std::size_t i = n - 1; i-- > 0;)   // i = n-2 .. 0
        {
            while (k >= lower && Cross<T>(h[k - 2], h[k - 1], pts[i]) <= T(0)) --k;
            h[k++] = pts[i];
        }
        h.resize(k - 1);   // last == first
        return h;
    }
} // namespace Arcane::Geometry::detail
