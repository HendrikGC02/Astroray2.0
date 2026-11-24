// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// 0006 C.16-C.17: the 4-entry medium stack's push, remove and depth flag, overflow, Schmidt and
// Budge priorities with ties by most recent entry, true and false interfaces entering and leaving,
// and the IOR ratio n(highest after) / n(highest before).
// Ref: docs/decisions/0006-volumes-and-astrophysics.md C.16-C.17; Schmidt and Budge 2002, JGT 7(2).
#include <doctest.h>

#include <cstdint>

#include "medium/medium_stack.h"

namespace {

using ar::U32x4;

struct S {
  U32x4 st = {};
  uint32_t flags = 0;
  bool push(uint32_t id, uint32_t pr) {
    const ar::StackUpdate u = ar::stack_push(st, flags, id, pr);
    st = u.stack, flags = u.flags;
    return u.ok;
  }
  bool remove(uint32_t id) {
    const ar::StackUpdate u = ar::stack_remove(st, flags, id);
    st = u.stack, flags = u.flags;
    return u.ok;
  }
  bool is_true(uint32_t id, uint32_t pr, bool entering) const {
    return ar::interface_is_true(st, flags, id, pr, entering);
  }
  uint32_t top() const { return ar::stack_highest_id(st, flags); }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("medium_stack/push remove and depth flag round trip") {
  S s;
  s.flags = 0xFFFFFFFFu & ~ar::MEDIUM_DEPTH_MASK;  // other flag bits must survive
  const uint32_t other = s.flags;
  CHECK(ar::medium_depth(s.flags) == 0u);
  CHECK(s.push(5, 2));
  CHECK(s.push(9, 7));
  CHECK(ar::medium_depth(s.flags) == 2u);
  CHECK(s.st.v[0] == ((5u << 8) | 2u));
  CHECK(s.st.v[1] == ((9u << 8) | 7u));
  CHECK(s.st.v[2] == 0u);
  CHECK((s.flags & ~ar::MEDIUM_DEPTH_MASK) == other);
  CHECK(s.push(5, 2));  // the same material nested in itself
  CHECK(s.remove(5));   // removes the most recent entry of that id
  CHECK(s.st.v[0] == ((5u << 8) | 2u));
  CHECK(s.st.v[1] == ((9u << 8) | 7u));
  CHECK(s.st.v[2] == 0u);
  CHECK(s.remove(5));  // the gap closes
  CHECK(s.st.v[0] == ((9u << 8) | 7u));
  CHECK(ar::medium_depth(s.flags) == 1u);
  CHECK(s.remove(9));
  CHECK(ar::medium_depth(s.flags) == 0u);
  CHECK(s.st.v[0] == 0u);
  CHECK((s.flags & ~ar::MEDIUM_DEPTH_MASK) == other);
  CHECK(ar::set_medium_depth(0u, 7u) == ar::MEDIUM_DEPTH_MASK);
}

TEST_CASE("medium_stack/remove of an absent id is refused") {
  S s;
  s.push(1, 0);
  const S before = s;
  CHECK_FALSE(s.remove(2));
  CHECK(s.flags == before.flags);
  for (int i = 0; i < 4; ++i) CHECK(s.st.v[i] == before.st.v[i]);
  // A back face whose id is not on the stack (a path that started inside the medium) is a true
  // interface; the caller reports it (lead ruling on 180).
  CHECK(s.is_true(2, 0, false));
  CHECK(S{}.is_true(2, 4, false));
}

TEST_CASE("medium_stack/leaving checks the most recent entry of a duplicated id") {
  // Id 1 (priority 2) is entered twice with an id 2 (priority 2) between: the most recent id-1
  // entry is the highest (latest of the ties), the older one is not.
  S s;
  s.push(1, 2);
  s.push(2, 2);
  s.push(1, 2);
  CHECK(s.is_true(1, 2, false));
  s.remove(1);  // takes the later id-1 entry; id 2 is now the highest
  CHECK_FALSE(s.is_true(1, 2, false));
  CHECK(s.is_true(2, 2, false));
}

TEST_CASE("medium_stack/overflow leaves stack unchanged") {
  S s;
  for (uint32_t i = 1; i <= 4; ++i) CHECK(s.push(i, i));
  CHECK(ar::medium_depth(s.flags) == 4u);
  const S before = s;
  CHECK_FALSE(s.push(5, 9));
  CHECK(s.flags == before.flags);
  for (int i = 0; i < 4; ++i) CHECK(s.st.v[i] == before.st.v[i]);
  CHECK(s.remove(2));  // room again
  CHECK(s.push(5, 9));
}

TEST_CASE("medium_stack/priority and ties") {
  // A high-priority inner object in a low-priority outer one: a true interface both ways.
  S s;
  s.push(1, 1);  // outer, low
  CHECK(s.is_true(2, 5, true));
  s.push(2, 5);  // inner, high
  CHECK(s.is_true(2, 5, false));
  CHECK(s.top() == 2u);
  // The reverse: a low-priority inner object in a high-priority outer one is a false interface.
  S r;
  r.push(2, 5);
  CHECK_FALSE(r.is_true(1, 1, true));
  r.push(1, 1);
  CHECK_FALSE(r.is_true(1, 1, false));
  CHECK(r.top() == 2u);  // the highest is unchanged: the outer one
  CHECK(ar::stack_active_priority(r.st, r.flags) == 5u);
  // Ties: the most recently entered is highest.
  S t;
  t.push(1, 3);
  CHECK(t.is_true(2, 3, true));  // the hit would be the most recent
  t.push(2, 3);
  CHECK(t.top() == 2u);
  CHECK(t.is_true(2, 3, false));        // leaving the most recent one
  CHECK_FALSE(t.is_true(1, 3, false));  // leaving the earlier one while the later one is highest
  // Empty stack: any priority enters true; active priority 0.
  S e;
  CHECK(e.is_true(1, 0, true));
  CHECK(ar::stack_active_priority(e.st, e.flags) == 0u);
  CHECK(e.top() == 0u);
}

TEST_CASE("medium_stack/enter and exit true and false") {
  // Blender's all-zero default: every crossing is true.
  S s;
  CHECK(s.is_true(1, 0, true));
  s.push(1, 0);
  CHECK(s.is_true(2, 0, true));
  s.push(2, 0);
  CHECK(s.is_true(2, 0, false));
  s.remove(2);
  CHECK(s.is_true(1, 0, false));
  s.remove(1);
  CHECK(ar::medium_depth(s.flags) == 0u);
}

TEST_CASE("medium_stack/ior ratio cases") {
  const ar::Real n_air = 1, n_glass = ar::Real(1.5), n_water = ar::Real(1.33);
  // Air to glass.
  CHECK(ar::interface_ior_ratio(n_air, n_glass) == doctest::Approx(1.5));
  // Glass (id 1, priority 2) containing water (id 2, priority 3): the nested crossing is
  // n(water) / n(glass); leaving water is the inverse; leaving glass reaches air.
  S s;
  const ar::Real n[3] = {n_air, n_glass, n_water};
  auto n_of = [&](const S& x) { return n[x.top()]; };
  const ar::Real before_glass = n_of(s);
  s.push(1, 2);
  CHECK(ar::interface_ior_ratio(before_glass, n_of(s)) == doctest::Approx(1.5));
  CHECK(s.is_true(2, 3, true));
  const ar::Real before_water = n_of(s);
  s.push(2, 3);
  CHECK(ar::interface_ior_ratio(before_water, n_of(s)) == doctest::Approx(1.33 / 1.5));
  CHECK(s.is_true(2, 3, false));
  const ar::Real leaving_water = n_of(s);
  s.remove(2);
  CHECK(ar::interface_ior_ratio(leaving_water, n_of(s)) == doctest::Approx(1.5 / 1.33));
  const ar::Real leaving_glass = n_of(s);
  s.remove(1);
  CHECK(ar::interface_ior_ratio(leaving_glass, n_of(s)) == doctest::Approx(1.0 / 1.5));
}

}  // TEST_SUITE
