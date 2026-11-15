// SPDX-License-Identifier: Apache-2.0
#pragma once
AR_NS_BEGIN
constexpr double kA = 3.14;
constexpr double kB = double(3.14);
Real f(Real x) { return x * 0.5 + Real(0.25) + 1e-3 + .5 + 2. + 1.0f + float(2.5) + 7; }
Real g(Real x) { return Real(ar::sqrt(2.0)) + Real(3.0 * 1e5); }
AR_NS_END
