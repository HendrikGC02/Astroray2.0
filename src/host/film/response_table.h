// SPDX-License-Identifier: Apache-2.0
// Host-side response channels (0008 d7): tabulated curves plus a unit, and the display
// group (xbar, ybar, zbar from observer.bin, then M_ws) packed into the one table the
// kernel reads through ar::ResponseTableView (kernel/film/response.h).
#pragma once
#include <cstdint>
#include <vector>

#include "host/data_resource.h"
#include "kernel/film/response.h"

namespace astroray::host {

// Values equal ar::ResponseUnit's (checked in response_table.cpp).
enum class ResponseUnit : uint32_t { Energy = 0, Photon = 1 };

// R(lambda) in nm (vacuum), piecewise linear; 0 outside [nm.front(), nm.back()].
struct ResponseCurve {
  std::vector<float> nm;
  std::vector<float> value;
  ResponseUnit unit = ResponseUnit::Energy;
};

struct ResponseTable {
  std::vector<float> knot_nm;
  std::vector<float> knot_value;
  std::vector<ar::ResponseChannel> channels;
  uint32_t display_first = 0;
  double photon_scale = 0;  // 1e-9 / (h c)
  double y_w = 0;
  double m_ws[9] = {};
};

// Channels 0, 1, 2 are the display group (xbar, ybar, zbar at 360 + i nm), Y_w = Y_n.
ResponseTable make_response_table(const ObserverTables& obs);

// Appends a channel and returns its index. Throws std::invalid_argument for fewer than two
// knots, differing array sizes, non-finite or not strictly ascending nm, and values that are
// non-finite or negative. Invalidates views of the table.
uint32_t add_response_channel(ResponseTable& table, const ResponseCurve& curve);

// The view the kernel reads; valid until the table is modified or destroyed.
inline ar::ResponseTableView response_view(const ResponseTable& t) {
  ar::ResponseTableView v{};
  v.knot_nm = t.knot_nm.data();
  v.knot_value = t.knot_value.data();
  v.channels = t.channels.data();
  v.channel_count = static_cast<uint32_t>(t.channels.size());
  v.display_first = t.display_first;
  v.photon_scale = t.photon_scale;
  v.y_w = t.y_w;
  for (int i = 0; i < 9; ++i) v.m_ws[i] = t.m_ws[i];
  return v;
}

}  // namespace astroray::host
