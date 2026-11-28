// SPDX-License-Identifier: Apache-2.0
// The final denoise in a CPU session (OIDN's CPU device): after convergence the session denoises once
// and only then reports CONVERGED; Combined is the denoised image with DENOISED set, Noisy Image is
// the film's and equals bit for bit the Combined of the same render without a denoiser, and the
// denoised image is closer to a 1024-spp reference than the noisy one (V11's criterion at test size).
// A denoiser that cannot be created leaves the noisy image, one DENOISER_UNAVAILABLE entry with the
// backend's text and the state CONVERGED; a cancel while the render waits for the denoiser returns
// within B-21 and leaves the noisy image with no entry, as does a cancel during the OIDN call; a
// poisoned context ends the wait as a denoiser failure; an edit after convergence discards the denoised
// image and denoises again; a target smaller than the render rectangle fails the session; sessions
// destroyed while the denoiser loads free everything. The scene is common/cornell_rig.h. The C ABI path is
// tests/abi/test_session_passes.py; CUDA is tests/gpu/test_session_denoise_gpu.cpp.
// Ref: docs/decisions/0008-film-outputs-denoise.md d10, d13a, d16 (ruling 87's order), d17, V9, V11;
// 0007 d27; 0009 B-21.
#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/cornell_rig.h"
#include "common/off_reference.h"
#include "host/film/fits_writer.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr unsigned kWorkers = 2;  // hosted ci has 2-4 cores
constexpr double kB21Ms = 5.0;    // 0009 B-21

bool has(const std::vector<std::string>& v, const char* name) { return std::find(v.begin(), v.end(), name) != v.end(); }

// Combined of a CPU render of the box without a denoiser.
std::vector<float> render_without_denoiser() {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_NONE));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  const std::vector<std::string> names = cornell::pass_names(s);
  for (const char* p : {"Noisy Image", "Denoising Albedo", "Denoising Normal"}) CHECK_FALSE(has(names, p));
  CHECK_FALSE(s.denoised());
  return cornell::read_mean(s, "Combined");
}

// The value of the FITS card `key` in `path`'s primary header ("" when absent).
std::string fits_card(const std::filesystem::path& path, const char* key) {
  std::ifstream in(path, std::ios::binary);
  std::string block(2880, '\0');
  while (in.read(block.data(), std::streamsize(block.size()))) {
    for (size_t at = 0; at < block.size(); at += 80) {
      const std::string card = block.substr(at, 80);
      if (card.compare(0, 8, std::string(key) + std::string(8 - std::strlen(key), ' ')) == 0) {
        const size_t v = card.find_first_not_of(' ', 10);
        return v == std::string::npos ? "" : card.substr(v, 1);
      }
      if (card.compare(0, 3, "END") == 0) return "";
    }
  }
  return "";
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("session_denoise/noisy image is the film") {
  const std::vector<float> without = render_without_denoiser();
  const std::vector<float> ref = cornell::reference();

  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  {
    Session s(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
    s.set_view(cornell::view());
    s.start();
    cornell::settle(s, rig);
    const std::vector<std::string> names = cornell::pass_names(s);
    for (const char* p : {"Combined", "Denoising Albedo", "Denoising Normal", "Noisy Image"}) CHECK(has(names, p));
    REQUIRE(s.denoised());
    const std::vector<float> combined = cornell::read_mean(s, "Combined");
    const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
    REQUIRE(noisy.size() == without.size());
    // d13a, d17: Noisy Image is the film's, so it is the render a session without a denoiser makes.
    CHECK(std::memcmp(noisy.data(), without.data(), noisy.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) != 0);
    cornell::check_reference(ref, noisy, cornell::read_mean(s, "Noisy Image", AR_QUANTITY_VARIANCE), "cpu");
    const double rel_noisy = cornell::rel_mse(noisy, ref), rel_denoised = cornell::rel_mse(combined, ref);
    const SessionStats st = s.stats();
    std::printf("session_denoise cpu: relMSE noisy %.5f denoised %.5f (ratio %.4f); denoise %.1f ms, prepare and copy %.1f ms\n",
                rel_noisy, rel_denoised, rel_denoised / rel_noisy, st.denoise_ms, st.denoise_copy_ms);
    CHECK(rel_denoised < rel_noisy);
    CHECK(st.denoises == 1);
    cornell::dump("reference", ref);
    cornell::dump("cpu_noisy", noisy);
    cornell::dump("cpu_denoised", combined);
    // A denoised image has no variance: the film's is Noisy Image's.
    CHECK_THROWS_WITH_AS(cornell::read_mean(s, "Combined", AR_QUANTITY_VARIANCE), doctest::Contains("Noisy Image"),
                         std::invalid_argument);
    CHECK(cornell::read_mean(s, "Noisy Image", AR_QUANTITY_VARIANCE).size() == noisy.size());
    // The guides read raw: the albedo of the lit walls is above 0 somewhere.
    const std::vector<float> albedo = cornell::read_mean(s, "Denoising Albedo");
    CHECK(*std::max_element(albedo.begin(), albedo.end()) > 0.5f);
    CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());
    // The target planes are in the ledger under DENOISER while the session lives (OIDN's CPU device
    // records none of its own, 176).
    CHECK(dev->ledger().category_total(LedgerCategory::DENOISER) > 0);
    // V9: the film dump of a denoised session still says DENOISED = F (a session film has no band
    // channel yet, so the dump is the session's whole film).
    const std::filesystem::path fits = cornell::repo_root() / "out" / "tmp" / "197" / "session_denoise_cpu.fits";
    std::filesystem::create_directories(fits.parent_path());
    s.read_film([&](const FilmReadContext& c) { write_fits_film_dump(fits.string(), FilmView(c.device, c.film), c.provenance); });
    CHECK(fits_card(fits, "DENOISED") == "F");
  }
  CHECK(dev->ledger().category_total(LedgerCategory::DENOISER) == 0);
}

TEST_CASE("session_denoise/failure leaves noisy and one entry") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  cornell::EnvGuard hook("ASTRORAY_TEST_DENOISER_FAIL", "library");
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
  REQUIRE(has(s.active_test_hooks(), "DENOISER_FAIL=library"));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  // d16: not a session failure; the noisy image stays with denoised = false.
  CHECK(s.progress().state == AR_STATE_CONVERGED);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  const std::string error = s.denoiser_error();
  std::printf("session_denoise cpu forced failure: %s\n", error.c_str());
  CHECK(error.find("no library folder given") != std::string::npos);
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE);
  REQUIRE(e.size() == 1);
  CHECK(e[0].detail == error.substr(0, 127));
  CHECK(s.stats().denoises == 0);
  // No target planes for a denoiser that does not work (OIDN's CPU device records none of its own).
  CHECK(dev->ledger().category_total(LedgerCategory::DENOISER) == 0);
}

TEST_CASE("session_denoise/cancel during denoise") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  o.test_hold_denoiser = true;
  Session s(rig.scene, *dev, o);
  s.set_view(cornell::view());
  s.start();
  // Every pass resolved and the session still RENDERING: the render thread is in the final denoise's
  // wait for the state (ruling 87: RENDERING until the denoise ends).
  REQUIRE(slab::until([&] {
    const SessionProgress p = s.progress();
    return p.samples_per_pixel == cornell::kSpp && p.active_generation == rig.generation;
  }));
  CHECK_FALSE(s.wait(100));  // held: the wait does not return
  CHECK(s.progress().state == AR_STATE_RENDERING);
  const auto t0 = std::chrono::steady_clock::now();
  s.cancel();
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE(s.wait(2000));
  const auto t2 = std::chrono::steady_clock::now();
  const double cancel_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double paused_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
  std::printf("session_denoise cpu cancel during the denoiser wait: cancel %.3f ms, PAUSED after %.3f ms (B-21 %.0f ms)\n",
              cancel_ms, paused_ms, kB21Ms);
  CHECK(astroray::test::timing_ok("B-21 session_denoise cancel", cancel_ms, kB21Ms));
  CHECK(astroray::test::timing_ok("B-21 session_denoise PAUSED", paused_ms, kB21Ms));
  CHECK(s.progress().state == AR_STATE_PAUSED);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());  // a cancel is not a failure
}

TEST_CASE("session_denoise/create and destroy while the denoiser loads") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    auto s = std::make_unique<Session>(rig.scene, *dev, cornell::options(AR_DENOISER_OIDN));
    if (i % 2) s->start();  // half of them with the render thread running too
    s.reset();
    REQUIRE(dev->ledger().category_total(LedgerCategory::DENOISER) == 0);
  }
  std::printf("session_denoise cpu: 20 sessions created and destroyed while loading in %.0f ms\n",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  CHECK(dev->ledger().total() == 0);
}

TEST_CASE("session_denoise/refusals and unknown denoising passes") {
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp, {"Combined", "Noisy Image", "Denoising Albedo"});
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions viewport = cornell::options(AR_DENOISER_OIDN);
  viewport.mode = AR_SESSION_MODE_VIEWPORT;
  CHECK_THROWS_WITH_AS(Session(rig.scene, *dev, viewport), doctest::Contains("viewport OIDN is M2"), SessionUnsupported);
  CHECK_THROWS_AS(Session(rig.scene, *dev, cornell::options(AR_DENOISER_DLSS_RR)), SessionUnsupported);
  // Without a denoiser the denoising passes do not exist: one PASS_UNSUPPORTED entry per name.
  Session s(rig.scene, *dev, cornell::options(AR_DENOISER_NONE));
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_PASS_UNSUPPORTED);
  CHECK(e.size() == 2);
  for (const char* p : {"Noisy Image", "Denoising Albedo"})
    CHECK(std::count_if(e.begin(), e.end(), [&](const cornell::Entry& x) { return x.detail == p; }) == 1);
}

TEST_CASE("session_denoise/poisoned context ends the denoiser wait") {
  // A denoiser worker that faults never reports (denoiser.h); the context is poisoned instead. The
  // render thread's wait must end as a denoiser failure: noisy image, one entry naming the poison, and
  // the session parked in CONVERGED rather than coming back to the wait on every loop.
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  o.test_hold_denoiser = true;
  Session s(rig.scene, *dev, o);
  s.set_view(cornell::view());
  s.start();
  REQUIRE(slab::until([&] { return s.progress().samples_per_pixel == cornell::kSpp; }));
  rig.ctx.poison_message = "forced by the test";
  rig.ctx.poisoned.store(true, std::memory_order_release);
  REQUIRE(slab::until([&] { return !cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty(); }, 10000));
  CHECK(s.progress().state == AR_STATE_FAILED);  // a poisoned context reads FAILED (0001 D2.5)
  // The test's own poison is lifted so the session's own state shows: it parked in CONVERGED.
  rig.ctx.poisoned.store(false, std::memory_order_release);
  REQUIRE(slab::until([&] { return s.progress().state == AR_STATE_CONVERGED; }, 10000));
  const std::vector<cornell::Entry> e = cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE);
  REQUIRE(e.size() == 1);
  CHECK(e[0].detail.find("context poisoned: forced by the test") != std::string::npos);
  CHECK_FALSE(s.denoised());
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
}

TEST_CASE("session_denoise/cancel during the denoise execution") {
  // Ruling 87: a cancel while OIDN runs leaves the noisy output (the filter itself cannot be stopped;
  // its result is dropped). The hook cancels on the render thread right after the OIDN call returned.
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  Session* self = nullptr;
  o.test_after_denoise = [&self] { self->cancel(); };
  Session s(rig.scene, *dev, o);
  self = &s;
  s.set_view(cornell::view());
  s.start();
  REQUIRE(slab::until([&] { return s.progress().state == AR_STATE_PAUSED; }));
  CHECK_FALSE(s.denoised());
  CHECK(s.stats().denoises == 0);
  const std::vector<float> combined = cornell::read_mean(s, "Combined");
  const std::vector<float> noisy = cornell::read_mean(s, "Noisy Image");
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  CHECK(cornell::entries(s, AR_REPORT_DENOISER_UNAVAILABLE).empty());  // a cancel is not a failure
}

TEST_CASE("session_denoise/an edit after convergence denoises again") {
  // An epoch reset discards the denoised image (0008 d16): right after it Combined is the film's, and
  // the next convergence denoises again. Passes are slowed so the reset can be seen before it ends.
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, cornell::kSpp);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  o.test_sleep_after_resolve_ms = 20;
  Session s(rig.scene, *dev, o);
  s.set_view(cornell::view());
  s.start();
  cornell::settle(s, rig);
  REQUIRE(s.denoised());
  REQUIRE(s.stats().denoises == 1);
  const uint64_t epoch = s.progress().epoch;
  rig.commit([](Edit&) {});
  REQUIRE(slab::until([&] {
    const SessionProgress p = s.progress();
    return p.epoch > epoch && p.film_epoch == p.epoch && p.samples_per_pixel < cornell::kSpp;
  }));
  CHECK_FALSE(s.denoised());
  // Both reads under one render lock, so no pass lands between them.
  std::vector<float> combined, noisy;
  s.read_film([&](const FilmReadContext& c) {
    CHECK(c.denoised == nullptr);
    const std::vector<PassDesc> passes = list_passes(c.film.layout(), c.denoiser);
    FilmView view(c.device, c.film);
    for (const PassDesc& p : passes) {
      if (p.name != "Combined" && p.name != "Noisy Image") continue;
      std::vector<float>& out = p.name == "Combined" ? combined : noisy;
      out.resize(pass_float_count(p, AR_QUANTITY_MEAN, view.width(), view.height()));
      read_pass(view, p, AR_QUANTITY_MEAN, uint32_t(out.size()), out.data(), c.denoised);
    }
  });
  REQUIRE(combined.size() == noisy.size());
  CHECK(std::memcmp(combined.data(), noisy.data(), noisy.size() * sizeof(float)) == 0);
  cornell::settle(s, rig);
  CHECK(s.denoised());
  CHECK(s.stats().denoises == 2);
}

}  // TEST_SUITE

TEST_SUITE("fast") {

TEST_CASE("session_denoise/the target must hold the render rectangle") {
  // film_prepare_denoise checks its target only in debug builds: the session refuses a render
  // rectangle larger than the target, loudly. The hook makes the target look one pixel short (the
  // buffer stays full size, so nothing is written out of bounds either way).
  cornell::CornellRig rig(SceneConfig{2, {}, {}}, 1);
  std::shared_ptr<Device> dev = make_cpu_session_device(rig.ctx, kWorkers);
  SessionOptions o = cornell::options(AR_DENOISER_OIDN);
  o.test_denoise_capacity_px = uint64_t(cornell::kSize) * cornell::kSize - 1;
  Session s(rig.scene, *dev, o);
  s.set_view(cornell::view());
  s.start();
  REQUIRE(slab::until([&] {
    const uint32_t st = s.progress().state;
    return st == AR_STATE_FAILED || st == AR_STATE_CONVERGED;
  }));
  CHECK(s.progress().state == AR_STATE_FAILED);
  CHECK(s.failure().find("does not fit the denoise target") != std::string::npos);
}

}  // TEST_SUITE

TEST_SUITE("fast") {

// The machine rule of a timing CHECK (tests/common/off_reference.h): an overrun fails on the reference machine and is
// printed, not failed, on another.
TEST_CASE("session_denoise/a timing check over its limit follows the machine standing") {
  using astroray::test::timing_ok_for;
  CHECK(timing_ok_for(nullptr, "B-21 x", 4.0, 5.0));
  CHECK_FALSE(timing_ok_for(nullptr, "B-21 x", 6.0, 5.0));
  CHECK_FALSE(timing_ok_for("", "B-21 x", 6.0, 5.0));
  CHECK(timing_ok_for("laptop-6c12t", "B-21 x", 6.0, 5.0));
}

}  // TEST_SUITE
