#pragma once
#include <algorithm>
#include <utility>

#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    template <class T>
    std::vector<Pt<T>> GrahamScanBuild(std::span<const Pt<T>> in)
    {
        std::vector<Pt<T>> pts(in.begin(), in.end());
        const std::size_t n = pts.size();

        std::size_t piv = 0;          // lowest y, tie lowest x
        for (std::size_t i = 1; i < n; ++i)
            if (pts[i].y < pts[piv].y ||
                (pts[i].y == pts[piv].y && pts[i].x < pts[piv].x))
                piv = i;
        std::swap(pts[0], pts[piv]);
        const Pt<T> p0 = pts[0];

        std::sort(pts.begin() + 1, pts.end(), [&](const Pt<T>& a, const Pt<T>& b)
        {
            const T c = Cross<T>(p0, a, b);
            if (c != T(0)) return c > T(0);   // CCW (left) first
            const T da = (a.x - p0.x) * (a.x - p0.x) + (a.y - p0.y) * (a.y - p0.y);
            const T db = (b.x - p0.x) * (b.x - p0.x) + (b.y - p0.y) * (b.y - p0.y);
            return da < db;                   // nearer first among equal angle
        });

        std::vector<Pt<T>> st;
        for (std::size_t i = 0; i < n; ++i)
        {
            while (st.size() >= 2 &&
                   Cross<T>(st[st.size() - 2], st[st.size() - 1], pts[i]) <= T(0))
                st.pop_back();
            st.push_back(pts[i]);
        }
        return st;
    }
} // namespace Arcane::Geometry::detail
