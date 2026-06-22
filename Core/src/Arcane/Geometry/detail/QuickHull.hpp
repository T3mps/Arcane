#pragma once
#include <Arcane/Geometry/detail/Predicates.hpp>

namespace Arcane::Geometry::detail
{
    template <class T>
    void QuickHullRec(std::span<const Pt<T>> pts, std::size_t a, std::size_t b,
                      const std::vector<std::size_t>& set, std::vector<Pt<T>>& out)
    {
        // Farthest point strictly left of a->b.
        T best = T(0);
        std::size_t c = static_cast<std::size_t>(-1);
        for (std::size_t idx : set)
        {
            const T d = Cross<T>(pts[a], pts[b], pts[idx]);
            if (d > best) { best = d; c = idx; }
        }
        if (c == static_cast<std::size_t>(-1)) { out.push_back(pts[a]); return; }

        std::vector<std::size_t> leftAC, leftCB;
        for (std::size_t idx : set)
        {
            if (idx == c) continue;
            if (Cross<T>(pts[a], pts[c], pts[idx]) > T(0))      leftAC.push_back(idx);
            else if (Cross<T>(pts[c], pts[b], pts[idx]) > T(0)) leftCB.push_back(idx);
        }
        QuickHullRec<T>(pts, a, c, leftAC, out);
        QuickHullRec<T>(pts, c, b, leftCB, out);
    }

    template <class T>
    std::vector<Pt<T>> QuickHullBuild(std::span<const Pt<T>> pts)
    {
        const std::size_t n = pts.size();
        std::size_t minI = 0, maxI = 0;
        for (std::size_t i = 1; i < n; ++i)
        {
            if (Less<T>(pts[i], pts[minI])) minI = i;
            if (Less<T>(pts[maxI], pts[i])) maxI = i;
        }
        std::vector<std::size_t> upper, lower;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i == minI || i == maxI) continue;
            const T d = Cross<T>(pts[minI], pts[maxI], pts[i]);
            if (d > T(0))      upper.push_back(i);
            else if (d < T(0)) lower.push_back(i);
        }
        std::vector<Pt<T>> out;
        QuickHullRec<T>(pts, minI, maxI, upper, out);   // minI..(upper chain)
        QuickHullRec<T>(pts, maxI, minI, lower, out);   // maxI..(lower chain)
        return out;
    }
} // namespace Arcane::Geometry::detail
