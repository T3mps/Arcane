#include <Arcane/Assets/ImageCompare.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    double BlendWithWhite(double c, double a) noexcept
    {
        return 255.0 + (c - 255.0) * a;
    }

    int Rgb2Gray(int r, int g, int b) noexcept
    {
        return (77 * r + 150 * g + 29 * b + 128) >> 8;
    }

    void Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept
    {
        auto toLinear = [](double v) noexcept
        {
            v /= 255.0;
            return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
        };
        const double r = toLinear(rgb[0]);
        const double g = toLinear(rgb[1]);
        const double b = toLinear(rgb[2]);

        xyz[0] = r * 0.4124 + g * 0.3576 + b * 0.1805;
        xyz[1] = r * 0.2126 + g * 0.7152 + b * 0.0722;
        xyz[2] = r * 0.0193 + g * 0.1192 + b * 0.9505;
    }

    void Xyz2Lab(const double xyz[3], double lab[3]) noexcept
    {
        // sigma = 6/29; the piecewise split keeps the curve finite-sloped at 0.
        constexpr double kSigmaPow2 = 6.0 * 6.0 / 29.0 / 29.0;
        constexpr double kSigmaPow3 = 6.0 * 6.0 * 6.0 / 29.0 / 29.0 / 29.0;

        const double x = xyz[0] / 0.950489;
        const double y = xyz[1];
        const double z = xyz[2] / 1.088840;

        // Upstream is `x ** (1/3)`; std::pow(v, 1.0/3.0) is the literal port of
        // that call, not std::cbrt. Bit-parity binds every arithmetic choice
        // here, and pow(v, 1/3) and a correctly-rounded cbrt are NOT the same
        // function -- JS's exponent is the inexact double 0.3333333333333333,
        // so the two diverge by several ULPs in the shadows, which is exactly
        // where Lab spends its precision and this function feeds every dE94.
        // It is the `v > kSigmaPow3` guard above -- not the function choice --
        // that excludes the negative branch where cbrt and pow(., 1/3) would
        // otherwise disagree (pow rejects a negative base with a fractional
        // exponent; cbrt does not).
        auto f = [](double v) noexcept
        {
            return v > kSigmaPow3 ? std::pow(v, 1.0 / 3.0) : v / 3.0 / kSigmaPow2 + 4.0 / 29.0;
        };
        const double fx = f(x), fy = f(y), fz = f(z);

        lab[0] = 116.0 * fy - 16.0;
        lab[1] = 500.0 * (fx - fy);
        lab[2] = 200.0 * (fy - fz);
    }

    double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept
    {
        double xyz1[3] = {}, xyz2[3] = {}, lab1[3] = {}, lab2[3] = {};
        Srgb2Xyz(rgb1, xyz1);
        Srgb2Xyz(rgb2, xyz2);
        Xyz2Lab(xyz1, lab1);
        Xyz2Lab(xyz2, lab2);

        const double deltaL = lab1[0] - lab2[0];
        const double deltaA = lab1[1] - lab2[1];
        const double deltaB = lab1[2] - lab2[2];

        const double c1 = std::sqrt(lab1[1] * lab1[1] + lab1[2] * lab1[2]);
        const double c2 = std::sqrt(lab2[1] * lab2[1] + lab2[2] * lab2[2]);
        const double deltaC = c1 - c2;

        double deltaH = deltaA * deltaA + deltaB * deltaB - deltaC * deltaC;
        deltaH = deltaH < 0.0 ? 0.0 : std::sqrt(deltaH);

        // "Graphic arts" weights. ASYMMETRIC: sC/sH use c1 (the EXPECTED
        // image's chroma) only -- upstream does the same, deliberately.
        constexpr double k1 = 0.045, k2 = 0.015;
        constexpr double kL = 1.0, kC = 1.0, kH = 1.0;
        const double sL = 1.0;
        const double sC = 1.0 + k1 * c1;
        const double sH = 1.0 + k2 * c1;

        const double tL = deltaL / sL / kL;
        const double tC = deltaC / sC / kC;
        const double tH = deltaH / sH / kH;
        return std::sqrt(tL * tL + tC * tC + tH * tH);
    }
}
