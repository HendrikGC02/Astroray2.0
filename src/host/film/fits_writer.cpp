// SPDX-License-Identifier: Apache-2.0
#include "host/film/fits_writer.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "host/build_info.h"

namespace astroray::host {

namespace {

constexpr size_t kBlock = 2880;  // FITS 4.0 section 3.1
constexpr size_t kCard = 80;

class Hdu {
 public:
  void logical(const char* key, bool v) { card(key, std::string(19, ' ') + (v ? 'T' : 'F')); }
  void integer(const char* key, long long v) {
    char b[32];
    std::snprintf(b, sizeof b, "%20lld", v);
    card(key, b);
  }
  // FITS 4.0 section 4.2.1: quote, text with quotes doubled, padded to 8 characters, quote.
  void string(const char* key, const std::string& v) {
    std::string q;
    for (char c : v) {
      if (c < 0x20 || c > 0x7E) throw std::invalid_argument(std::string("fits: ") + key + " is not printable ASCII");
      q += c;
      if (c == '\'') q += '\'';
    }
    if (v.empty()) throw std::invalid_argument(std::string("fits: ") + key + " is empty");
    if (q.size() > 68) throw std::invalid_argument(std::string("fits: ") + key + " is longer than 68 characters");
    if (q.size() < 8) q.append(8 - q.size(), ' ');
    card(key, "'" + q + "'");
  }
  // END, then spaces to a block boundary.
  std::string finish() {
    std::string s = cards_ + pad("END", kCard);
    s.resize((s.size() + kBlock - 1) / kBlock * kBlock, ' ');
    return s;
  }

 private:
  static std::string pad(std::string s, size_t n) {
    s.resize(n, ' ');
    return s;
  }
  // Fixed format: keyword in 1-8, "= " in 9-10, value from column 11 (numbers and logicals
  // right-justified to column 30).
  void card(const char* key, const std::string& value) {
    std::string k = key;
    if (k.empty() || k.size() > 8) throw std::logic_error("fits: bad keyword " + k);
    cards_ += pad(pad(k, 8) + "= " + value, kCard);
  }
  std::string cards_;
};

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
void put64(std::vector<uint8_t>& out, uint64_t v) {
  for (int s = 56; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

void append(std::vector<uint8_t>& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

void pad_block(std::vector<uint8_t>& out) { out.resize((out.size() + kBlock - 1) / kBlock * kBlock, 0); }

struct Plane {
  std::string extname;
  std::string channel;
  uint32_t component;
  const char* kind;  // MEAN, VAR, NSAMP, COUNT
  std::vector<double> real;
  std::vector<uint32_t> integer;  // used when real is empty
};

void write_image(std::vector<uint8_t>& out, const Plane& p, uint32_t w, uint32_t h, FitsPrecision precision) {
  const bool is_int = p.real.empty();
  Hdu hdu;
  hdu.string("XTENSION", "IMAGE");
  hdu.integer("BITPIX", is_int ? 32 : (precision == FitsPrecision::F32 ? -32 : -64));
  hdu.integer("NAXIS", 2);
  hdu.integer("NAXIS1", w);
  hdu.integer("NAXIS2", h);
  hdu.integer("PCOUNT", 0);
  hdu.integer("GCOUNT", 1);
  hdu.string("EXTNAME", p.extname);
  hdu.string("CHANNEL", p.channel);
  hdu.integer("COMP", p.component);
  hdu.string("PLANE", p.kind);
  append(out, hdu.finish());
  const size_t n = size_t(w) * h;
  for (size_t i = 0; i < n; ++i) {
    if (is_int) {
      if (p.integer[i] > uint32_t(INT32_MAX))
        throw std::length_error("fits: " + p.extname + " holds a counter above INT32_MAX");
      put32(out, p.integer[i]);
    } else if (precision == FitsPrecision::F32) {
      const float f = float(p.real[i]);
      uint32_t bits;
      std::memcpy(&bits, &f, 4);
      put32(out, bits);
    } else {
      uint64_t bits;
      std::memcpy(&bits, &p.real[i], 8);
      put64(out, bits);
    }
  }
  pad_block(out);
}

std::string component_name(const std::string& channel, uint32_t components, uint32_t c) {
  return components == 1 ? channel : channel + "." + std::to_string(c);
}

}  // namespace

std::vector<uint8_t> fits_film_dump_bytes(const FilmView& film, const FitsProvenance& pv, FitsPrecision precision) {
  Hdu primary;
  primary.logical("SIMPLE", true);
  primary.integer("BITPIX", 8);
  primary.integer("NAXIS", 0);
  primary.logical("EXTEND", true);
  primary.string("ENGVER", astroray::kBuildInfo.version);
  primary.string("COMMIT", std::string(astroray::kBuildInfo.git_sha) + (astroray::kBuildInfo.dirty ? "-dirty" : ""));
  primary.integer("SEED", pv.seed);
  primary.integer("SPP", pv.spp);
  primary.string("SAMPLER", pv.sampler_mode);
  primary.string("OBSHASH", pv.observer_hash);
  primary.string("WORKSPC", pv.working_space);
  primary.string("FILTER", pv.filter);
  primary.string("WLPDF", pv.wavelength_pdf);
  primary.string("DEVICE", pv.device);
  primary.logical("DENOISED", false);

  std::vector<uint8_t> out;
  append(out, primary.finish());

  const uint32_t w = film.width(), h = film.height();
  for (const FilmChannel& ch : film.layout().channels()) {
    // Splat has no film plane readback yet; skipping it would be silent, so refuse.
    if (ch.kind != ChannelKind::Moment && ch.kind != ChannelKind::Add && ch.kind != ChannelKind::Count)
      throw std::invalid_argument("fits: channel '" + ch.name + "' has a kind the film dump cannot write yet");
    for (uint32_t c = 0; c < ch.components; ++c) {
      const std::string name = component_name(ch.name, ch.components, c);
      if (ch.kind == ChannelKind::Count) {
        write_image(out, {name, ch.name, c, "COUNT", {}, film.counts(ch.name, c)}, w, h, precision);
        continue;
      }
      write_image(out, {name, ch.name, c, "MEAN", film.mean(ch.name, c), {}}, w, h, precision);
      if (ch.kind != ChannelKind::Moment) continue;
      write_image(out, {name + ".VAR", ch.name, c, "VAR", film.variance_of_mean(ch.name, c), {}}, w, h, precision);
      write_image(out, {name + ".NSAMP", ch.name, c, "NSAMP", {}, film.sample_counts()}, w, h, precision);
    }
  }
  return out;
}

void write_fits_film_dump(const std::string& path, const FilmView& film, const FitsProvenance& pv,
                          FitsPrecision precision) {
  const std::vector<uint8_t> bytes = fits_film_dump_bytes(film, pv, precision);
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  f.close();
  if (!f) throw std::runtime_error("fits: cannot write '" + path + "'");
}

}  // namespace astroray::host
