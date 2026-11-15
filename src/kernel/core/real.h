// SPDX-License-Identifier: Apache-2.0
// The scalar type and the namespace that keeps float and double builds apart.
// Every kernel header wraps its declarations in AR_NS_BEGIN / AR_NS_END, so the
// f32 and f64 copies of one header are different symbols. Without the inline
// namespace the linker would silently merge them (one-definition rule).
// Ref: docs/decisions/0002-kernel-architecture.md D3.
#pragma once

#if defined(AR_REAL_DOUBLE)
#define AR_REAL_NS f64
#define AR_RT_SUFFIX " f64"
#else
#define AR_REAL_NS f32
#define AR_RT_SUFFIX ""
#endif

#define AR_NS_BEGIN namespace ar { inline namespace AR_REAL_NS {
#define AR_NS_END } }

AR_NS_BEGIN
#if defined(AR_REAL_DOUBLE)
using Real = double;
#else
using Real = float;
#endif
AR_NS_END

// A doctest case named `name`, or `name f64` in the double copy of a test file.
#define AR_RT_CASE(name) TEST_CASE(name AR_RT_SUFFIX)
