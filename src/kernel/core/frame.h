// SPDX-License-Identifier: Apache-2.0
// Orthonormal shading frame, right-handed with s x t = n.
// Ref: Duff, Burgess, Christensen, Hery, Kensler, Liani, Villemin 2017,
// "Building an Orthonormal Basis, Revisited", JCGT 6(1); pbrt-v4
// util/vecmath.h Frame::FromZ (Apache-2.0).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/vec.h"
#include "portability.h"

AR_NS_BEGIN

struct Frame {
  Real3 s, t, n;
};

// n must be unit length. The branch-free construction of Duff et al., with the
// sign chosen by n.z so it is stable at n = (0, 0, -1).
AR_FN Frame frame_from_normal(Real3 n) {
  const Real sign = n.z < Real(0) ? Real(-1) : Real(1);
  const Real a = Real(-1) / (sign + n.z);
  const Real b = n.x * n.y * a;
  Frame f;
  f.n = n;
  f.s = Real3{Real(1) + sign * n.x * n.x * a, sign * b, -sign * n.x};
  f.t = Real3{b, sign + n.y * n.y * a, -n.y};
  return f;
}

// n must be unit length; tangent is any vector not parallel to n. Gram-Schmidt
// keeps the tangent direction, which anisotropic lobes need. The projection is
// applied twice ("twice is enough", Giraud et al. 2005) so a tangent that is
// nearly parallel to n still gives a tangent perpendicular to n in float.
AR_FN Frame frame_from_normal_tangent(Real3 n, Real3 tangent) {
  Frame f;
  f.n = n;
  Real3 s = tangent - n * dot(n, tangent);
  s = normalize(s);
  f.s = normalize(s - n * dot(n, s));
  f.t = cross(n, f.s);
  return f;
}

AR_FN Real3 to_local(const Frame& f, Real3 v) { return Real3{dot(v, f.s), dot(v, f.t), dot(v, f.n)}; }
AR_FN Real3 to_world(const Frame& f, Real3 v) { return f.s * v.x + f.t * v.y + f.n * v.z; }

AR_NS_END
