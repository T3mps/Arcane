#pragma once

// Arcane::Geometry public entry: ConvexHull<Policy,T>(points). Each algorithm is a
// stateless policy tag whose Build returns an ordered boundary cycle; the wrapper
// owns the shared contract (dedup, degenerate cases, canonical CCW/pivot form), so
// all policies return byte-identical canonical output for the same input.

#include <span>
#include <vector>

#include <Arcane/Geometry/detail/Predicates.hpp>
#include <Arcane/Geometry/detail/MonotoneChain.hpp>
#include <Arcane/Geometry/detail/GrahamScan.hpp>
#include <Arcane/Geometry/detail/JarvisMarch.hpp>
#include <Arcane/Geometry/detail/QuickHull.hpp>
#include <Arcane/Geometry/detail/Chan.hpp>

namespace Arcane::Geometry
{
    struct MonotoneChain
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::MonotoneChainBuild<T>(p);
        }
    };

    struct GrahamScan
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::GrahamScanBuild<T>(p);
        }
    };

    struct JarvisMarch
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::JarvisMarchBuild<T>(p);
        }
    };

    struct QuickHull
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::QuickHullBuild<T>(p);
        }
    };

    struct Chan
    {
        template <class T>
        static std::vector<Pt<T>> Build(std::span<const Pt<T>> p)
        {
            return detail::ChanBuild<T>(p);
        }
    };

    template <class Policy, class T = float>
    std::vector<Pt<T>> ConvexHull(std::span<const Pt<T>> points)
    {
        std::vector<Pt<T>> pts = detail::Dedup<T>(points);
        const std::size_t n = pts.size();
        if (n < 3) return pts;   // 0/1/2 points: already lexicographically ordered.

        bool collinear = true;
        for (std::size_t i = 2; i < n && collinear; ++i)
        {
            if (detail::Cross<T>(pts[0], pts[1], pts[i]) != T(0))
                collinear = false;
        }
        if (collinear)
            return { pts.front(), pts.back() };   // two extreme endpoints.

        std::vector<Pt<T>> hull = Policy::template Build<T>(std::span<const Pt<T>>(pts));
        return detail::Canonicalize<T>(std::move(hull));
    }
} // namespace Arcane::Geometry
