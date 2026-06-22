#pragma once

// Arcane::Geometry shared predicates + the canonical-form post-processing every
// convex-hull policy runs through. Presentation-free: glm + std only (compiles
// /MD and static-CRT). Header-only templates; scalar-generic on T (float|double).

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <glm/vec2.hpp>

namespace Arcane::Geometry
{
    template <class T> using Pt = glm::vec<2, T>;

    namespace detail
    {
        // (b-o) is left of (a-o): >0 CCW/left turn, <0 CW/right, 0 collinear.
        template <class T>
        constexpr T Cross(const Pt<T>& o, const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        // Lexicographic order: x then y.
        template <class T>
        constexpr bool Less(const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return a.x < b.x || (a.x == b.x && a.y < b.y);
        }

        template <class T>
        constexpr bool Equal(const Pt<T>& a, const Pt<T>& b) noexcept
        {
            return a.x == b.x && a.y == b.y;
        }

        // Copy, lexicographically sort, drop exact duplicates.
        template <class T>
        std::vector<Pt<T>> Dedup(std::span<const Pt<T>> pts)
        {
            std::vector<Pt<T>> v(pts.begin(), pts.end());
            std::sort(v.begin(), v.end(),
                      [](const Pt<T>& a, const Pt<T>& b) { return Less<T>(a, b); });
            v.erase(std::unique(v.begin(), v.end(),
                                [](const Pt<T>& a, const Pt<T>& b) { return Equal<T>(a, b); }),
                    v.end());
            return v;
        }

        // 2x signed area of an ordered closed polygon (>0 => CCW in math orientation).
        template <class T>
        T SignedArea2(const std::vector<Pt<T>>& poly)
        {
            T s = T(0);
            const std::size_t n = poly.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const Pt<T>& a = poly[i];
                const Pt<T>& b = poly[(i + 1) % n];
                s += a.x * b.y - b.x * a.y;
            }
            return s;
        }

        // Drop vertices collinear with their neighbours (and seam duplicates, whose
        // neighbour cross is also 0) from an ordered closed polygon.
        template <class T>
        std::vector<Pt<T>> StripCollinear(const std::vector<Pt<T>>& poly)
        {
            const std::size_t n = poly.size();
            if (n < 3) return poly;
            std::vector<Pt<T>> out;
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const Pt<T>& prev = poly[(i + n - 1) % n];
                const Pt<T>& cur  = poly[i];
                const Pt<T>& next = poly[(i + 1) % n];
                if (Cross<T>(prev, cur, next) != T(0))
                    out.push_back(cur);
            }
            return out;
        }

        // Canonical form of a policy's ordered boundary cycle WITHOUT re-deriving the
        // hull: strip collinear, normalise winding to CCW by signed-area SIGN (reverse
        // if negative -- NOT an angular re-sort, so a policy ordering bug surfaces as a
        // wrong result), rotate to start at the lexicographically smallest vertex.
        template <class T>
        std::vector<Pt<T>> Canonicalize(std::vector<Pt<T>> hull)
        {
            hull = StripCollinear<T>(hull);
            if (hull.size() < 3) return hull;
            if (SignedArea2<T>(hull) < T(0))
                std::reverse(hull.begin(), hull.end());
            std::size_t start = 0;
            for (std::size_t i = 1; i < hull.size(); ++i)
                if (Less<T>(hull[i], hull[start])) start = i;
            std::rotate(hull.begin(), hull.begin() + static_cast<std::ptrdiff_t>(start),
                        hull.end());
            return hull;
        }

        // Deterministic median-of-medians (groups of 5) selection: returns the k-th
        // (0-based) smallest element of `a` under `less`. O(n) worst case. By value.
        template <class U, class Cmp>
        U DeterministicSelect(std::vector<U> a, std::size_t k, Cmp less)
        {
            for (;;)
            {
                const std::size_t n = a.size();
                if (n <= 5)
                {
                    std::sort(a.begin(), a.end(), less);
                    return a[k];
                }
                std::vector<U> medians;
                medians.reserve((n + 4) / 5);
                for (std::size_t i = 0; i < n; i += 5)
                {
                    const std::size_t e = std::min(i + 5, n);
                    std::sort(a.begin() + static_cast<std::ptrdiff_t>(i),
                              a.begin() + static_cast<std::ptrdiff_t>(e), less);
                    medians.push_back(a[i + (e - i) / 2]);
                }
                U pivot = DeterministicSelect<U>(medians, medians.size() / 2, less);
                std::vector<U> lo, hi;
                std::size_t eq = 0;
                for (const U& x : a)
                {
                    if (less(x, pivot))      lo.push_back(x);
                    else if (less(pivot, x)) hi.push_back(x);
                    else                     ++eq;
                }
                if (k < lo.size())            { a = std::move(lo); }
                else if (k < lo.size() + eq)  { return pivot; }
                else { k -= lo.size() + eq;     a = std::move(hi); }
            }
        }
    } // namespace detail
} // namespace Arcane::Geometry
