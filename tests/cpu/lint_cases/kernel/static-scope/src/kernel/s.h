// SPDX-License-Identifier: Apache-2.0
#pragma once
AR_NS_BEGIN
static int g1 = 0;
static Real g2;
static const int ok1 = 1;
static constexpr Real ok2 = Real(1);
static Real fn(Real x);
static Real fn2(Real x) { return x; }
struct S { static int member; };
void h() { static int local = 0; (void)local; }
namespace inner { static float g3 = Real(0); }
AR_NS_END
