// Spike-B native module: threaded test-pattern framebuffer + scene copy-in. Throwaway.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cmath>
#include "sb.h"

#define EXPORT extern "C" __declspec(dllexport)

static double g_qpf = 0;
static int64_t qpc_ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void init_qpf() { if (g_qpf == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart; } }
// Same definition CPython uses for time.perf_counter_ns() on Windows (ticks * 1e9 / freq).
EXPORT double sb_now_ns() { init_qpf(); return (double)qpc_ticks() * 1e9 / g_qpf; }
static double ticks_to_ms(int64_t d) { init_qpf(); return (double)d * 1e3 / g_qpf; }

EXPORT double sb_qpc_ns_to_check() { return sb_now_ns(); }

// ---------------------------------------------------------------- framebuffer
static uint16_t f2h(float f) {  // round-to-nearest-even float -> half, enough for a test pattern
  uint32_t x; memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000; int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
  if (e <= 0) return (uint16_t)sign;
  if (e >= 31) return (uint16_t)(sign | 0x7c00);
  uint16_t h = (uint16_t)(sign | (e << 10) | (m >> 13));
  if ((m & 0x1fff) > 0x1000 || ((m & 0x1fff) == 0x1000 && (h & 1))) h++;
  return h;
}

static const int RING = 8192;
struct FB {
  int w, h, fmt, nbuf; size_t bpp;      // fmt 0 = RGBA32F, 1 = RGBA16F, 2 = RGBA8
  uint8_t* buf[4] = {};
  std::atomic<int> front{0};
  std::atomic<uint64_t> counter{0};
  std::atomic<double> last_ns{0};
  std::atomic<double> fps{60};
  std::atomic<bool> stop{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> resync{false};
  std::atomic<double> write_ms_sum{0}, write_ms_max{0};
  std::atomic<double> gap_ms_max{0};   // longest interval between two swaps while pacing
  double ring[RING];                   // swap timestamps (ns), index = counter % RING
  std::thread th;
  std::vector<uint8_t> tmpl;           // 2*w pixels of pattern, rows are shifted windows into it
};

static void fill_template(FB* f, uint64_t frame) {
  int w = f->w;
  float phase = (float)(frame % 600) / 600.0f;
  for (int x = 0; x < 2 * w; x++) {
    float u = (float)(x % w) / (float)w;
    float r = u * 1.5f, g = 0.5f + 0.5f * sinf(6.2831853f * (u * 4.0f + phase)), b = 1.0f - u + 0.25f * phase, a = 1.0f;
    if (((x % w) / 64 + (int)(frame / 8)) % 2 == 0) { r *= 0.5f; g *= 0.5f; b *= 0.5f; }
    if (f->fmt == 3) {  // packed R11G11B10F, one uint32 per pixel (truncating half->11/10-bit float)
      auto pos = [](float v) { return v < 0 ? 0.0f : v; };
      uint32_t hr = f2h(pos(r)), hg = f2h(pos(g)), hb = f2h(pos(b));
      ((uint32_t*)f->tmpl.data())[x] = ((hr >> 4) & 0x7ff) | (((hg >> 4) & 0x7ff) << 11) | (((hb >> 5) & 0x3ff) << 22);
    }
    else if (f->fmt == 0) { float* p = (float*)f->tmpl.data() + 4 * x; p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    else if (f->fmt == 1) { uint16_t* p = (uint16_t*)f->tmpl.data() + 4 * x; p[0] = f2h(r); p[1] = f2h(g); p[2] = f2h(b); p[3] = f2h(a); }
    else { uint8_t* p = f->tmpl.data() + 4 * x; auto q = [](float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (uint8_t)(v * 255.0f + 0.5f); }; p[0] = q(r); p[1] = q(g); p[2] = q(b); p[3] = 255; }
  }
}

static void write_frame(FB* f, int b, uint64_t frame) {
  fill_template(f, frame);
  size_t rowb = (size_t)f->w * f->bpp;
  for (int y = 0; y < f->h; y++) {
    size_t shift = ((size_t)(frame * 6) + (size_t)(y >> 4) * 37) % (size_t)f->w;
    memcpy(f->buf[b] + (size_t)y * rowb, f->tmpl.data() + shift * f->bpp, rowb);
  }
}

static void worker(FB* f) {
  init_qpf();
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  int64_t next = qpc_ticks(); int64_t prev_swap = 0;
  while (!f->stop.load(std::memory_order_relaxed)) {
    int b = (f->front.load() + 1) % f->nbuf;
    uint64_t frame = f->counter.load();
    int64_t t0 = qpc_ticks();
    write_frame(f, b, frame);
    int64_t t1 = qpc_ticks();
    f->front.store(b, std::memory_order_release);
    double now_ns = sb_now_ns();
    f->last_ns.store(now_ns);
    f->ring[(frame + 1) % RING] = now_ns;
    f->counter.fetch_add(1, std::memory_order_release);
    double wms = ticks_to_ms(t1 - t0);
    f->write_ms_sum.store(f->write_ms_sum.load() + wms);
    if (wms > f->write_ms_max.load()) f->write_ms_max.store(wms);
    if (prev_swap) { double gap = ticks_to_ms(t1 - prev_swap); if (gap > f->gap_ms_max.load()) f->gap_ms_max.store(gap); }
    prev_swap = t1;
    double fps = f->fps.load();
    if (fps > 0) {
      next += (int64_t)(g_qpf / fps);
      int64_t now = qpc_ticks();
      if (next < now - (int64_t)(g_qpf * 0.1)) next = now;  // fell far behind: resync, do not burst
      for (;;) {
        now = qpc_ticks();
        if (now >= next || f->stop.load()) break;
        if (f->resync.exchange(false)) { next = now; break; }
        int64_t rem = next - now;
        double rem_ms = ticks_to_ms(rem);
        if (timer && rem_ms > 2.5) {
          double w_ms = rem_ms - 1.0; if (w_ms > 10.0) w_ms = 10.0;
          LARGE_INTEGER due; due.QuadPart = -(int64_t)(w_ms * 10000.0);
          SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, 50);
        } else YieldProcessor();
      }
    }
    while (f->paused.load() && !f->stop.load()) { Sleep(1); next = qpc_ticks(); prev_swap = 0; }
  }
  if (timer) CloseHandle(timer);
}

EXPORT void* sb_fb_create(int w, int h, int fmt, double fps, int nbuf) {
  init_qpf();
  FB* f = new FB();
  f->w = w; f->h = h; f->fmt = fmt; f->nbuf = nbuf < 2 ? 2 : (nbuf > 4 ? 4 : nbuf);
  f->bpp = fmt == 0 ? 16 : (fmt == 1 ? 8 : 4);  // fmt 3 = packed R11G11B10F, 4 bytes/pixel
  f->fps.store(fps);
  for (int i = 0; i < f->nbuf; i++) { f->buf[i] = (uint8_t*)_aligned_malloc((size_t)w * h * f->bpp, 64); memset(f->buf[i], 0, (size_t)w * h * f->bpp); }
  f->tmpl.resize((size_t)w * 2 * f->bpp);
  memset(f->ring, 0, sizeof(f->ring));
  f->th = std::thread(worker, f);
  return f;
}
EXPORT void sb_fb_destroy(void* p) {
  FB* f = (FB*)p; f->stop.store(true); f->th.join();
  for (int i = 0; i < f->nbuf; i++) _aligned_free(f->buf[i]);
  delete f;
}
EXPORT void* sb_fb_front(void* p) { FB* f = (FB*)p; return f->buf[f->front.load(std::memory_order_acquire)]; }
EXPORT uint64_t sb_fb_counter(void* p) { return ((FB*)p)->counter.load(std::memory_order_acquire); }
EXPORT double sb_fb_last_ns(void* p) { return ((FB*)p)->last_ns.load(); }
EXPORT void sb_fb_set_fps(void* p, double fps) { FB* f = (FB*)p; f->fps.store(fps); f->resync.store(true); }
EXPORT void sb_fb_pause(void* p, int on) { FB* f = (FB*)p; f->paused.store(on != 0); }
EXPORT int64_t sb_fb_bytes(void* p) { FB* f = (FB*)p; return (int64_t)f->w * f->h * (int64_t)f->bpp; }
// stats: [frames, write_ms_sum, write_ms_max, gap_ms_max]; reset=1 clears the accumulators
EXPORT void sb_fb_stats(void* p, double* out, int reset) {
  FB* f = (FB*)p;
  out[0] = (double)f->counter.load(); out[1] = f->write_ms_sum.load(); out[2] = f->write_ms_max.load(); out[3] = f->gap_ms_max.load();
  if (reset) { f->write_ms_sum.store(0); f->write_ms_max.store(0); f->gap_ms_max.store(0); }
}
// Swap timestamps (ns) for frames [first, first+n); caller must keep n < RING.
EXPORT int sb_fb_ring(void* p, uint64_t first, int n, double* out) {
  FB* f = (FB*)p; for (int i = 0; i < n; i++) out[i] = f->ring[(first + i) % RING]; return RING;
}

// ---------------------------------------------------------------- scene copy-in
static void* cpy(const void* src, size_t bytes) {
  if (!src || !bytes) return nullptr;
  void* d = malloc(bytes); memcpy(d, src, bytes); return d;
}
static void free_mesh(SbMesh& m) { free(m.pos); free(m.tri); free(m.nrm); free(m.uv); free(m.tloops); free(m.tmat); m = SbMesh(); }

EXPORT void* sb_scene_create() { init_qpf(); return new SbScene(); }
EXPORT void sb_scene_destroy(void* p) {
  SbScene* s = (SbScene*)p; for (auto& m : s->meshes) free_mesh(m); free(s->obj_mesh); free(s->obj_xf); delete s;
}
// Copies one mesh into native storage. Returns copy time in ms (QueryPerformanceCounter around the memcpys + mallocs).
EXPORT double sb_scene_set_mesh(void* p, int id, const float* pos, int64_t nv, const int32_t* tri, int64_t nt,
                                const float* nrm, int64_t nn, const float* uv, int64_t nuv,
                                const int32_t* tloops, const int32_t* tmat) {
  SbScene* s = (SbScene*)p;
  int64_t t0 = qpc_ticks();
  if ((int)s->meshes.size() <= id) s->meshes.resize(id + 1);
  SbMesh& m = s->meshes[id]; free_mesh(m);
  m.nv = nv; m.nt = nt; m.nn = nn; m.nuv = nuv;
  m.pos = (float*)cpy(pos, nv * 12); m.tri = (int32_t*)cpy(tri, nt * 12); m.nrm = (float*)cpy(nrm, nn * 12);
  m.uv = (float*)cpy(uv, nuv * 8); m.tloops = (int32_t*)cpy(tloops, tloops ? nt * 12 : 0); m.tmat = (int32_t*)cpy(tmat, tmat ? nt * 4 : 0);
  return ticks_to_ms(qpc_ticks() - t0);
}

// Polygon-mesh copy-in with native fan triangulation (what a fast Python read path would feed: raw corner_vert + face start offsets).
// fs has nf+1 entries (last = nl). fmat may be null. Returns copy+triangulate time in ms.
EXPORT double sb_scene_set_mesh_poly(void* p, int id, const float* pos, int64_t nv, const int32_t* cv, int64_t nl,
                                     const int32_t* fs, int64_t nf, const float* nrm, const float* uv, const int32_t* fmat) {
  SbScene* s = (SbScene*)p;
  int64_t t0 = qpc_ticks();
  if ((int)s->meshes.size() <= id) s->meshes.resize(id + 1);
  SbMesh& m = s->meshes[id]; free_mesh(m);
  int64_t nt = 0; for (int64_t f = 0; f < nf; f++) nt += (fs[f + 1] - fs[f]) - 2;
  m.nv = nv; m.nt = nt; m.nn = nrm ? nl : 0; m.nuv = uv ? nl : 0;
  m.pos = (float*)cpy(pos, nv * 12); m.nrm = (float*)cpy(nrm, nrm ? nl * 12 : 0); m.uv = (float*)cpy(uv, uv ? nl * 8 : 0);
  m.tri = (int32_t*)malloc(nt * 12); m.tloops = (int32_t*)malloc(nt * 12); m.tmat = (int32_t*)malloc(nt * 4);
  int64_t k = 0;
  for (int64_t f = 0; f < nf; f++) {
    int32_t a = fs[f], n = fs[f + 1] - a;
    for (int32_t j = 1; j + 1 < n; j++) {
      m.tloops[k * 3] = a; m.tloops[k * 3 + 1] = a + j; m.tloops[k * 3 + 2] = a + j + 1;
      m.tri[k * 3] = cv[a]; m.tri[k * 3 + 1] = cv[a + j]; m.tri[k * 3 + 2] = cv[a + j + 1];
      m.tmat[k] = fmat ? fmat[f] : 0; k++;
    }
  }
  return ticks_to_ms(qpc_ticks() - t0);
}
EXPORT double sb_scene_set_objects(void* p, int64_t n, const int32_t* mesh_ids, const float* xf) {
  SbScene* s = (SbScene*)p; int64_t t0 = qpc_ticks();
  free(s->obj_mesh); free(s->obj_xf);
  s->obj_mesh = (int32_t*)cpy(mesh_ids, n * 4); s->obj_xf = (float*)cpy(xf, n * 64); s->nobj = n;
  return ticks_to_ms(qpc_ticks() - t0);
}
EXPORT double sb_scene_set_xform(void* p, int64_t idx, const float* xf16) {
  SbScene* s = (SbScene*)p; int64_t t0 = qpc_ticks();
  if (idx < s->nobj) memcpy(s->obj_xf + idx * 16, xf16, 64);
  return ticks_to_ms(qpc_ticks() - t0);
}
EXPORT double sb_scene_set_material(void* p, int id, const float* params, int n) {
  SbScene* s = (SbScene*)p; int64_t t0 = qpc_ticks();
  if ((int)s->materials.size() <= id) s->materials.resize(id + 1);
  s->materials[id].assign(params, params + n);
  return ticks_to_ms(qpc_ticks() - t0);
}
EXPORT double sb_scene_edit_vertex(void* p, int mesh_id, int64_t vi, float x, float y, float z) {
  SbScene* s = (SbScene*)p; int64_t t0 = qpc_ticks();
  SbMesh& m = s->meshes[mesh_id]; if (vi < m.nv) { m.pos[vi * 3] = x; m.pos[vi * 3 + 1] = y; m.pos[vi * 3 + 2] = z; }
  return ticks_to_ms(qpc_ticks() - t0);
}
// Appends one object (instance of mesh_id) to the object list.
EXPORT double sb_scene_add_object(void* p, int mesh_id, const float* xf16) {
  SbScene* s = (SbScene*)p; int64_t t0 = qpc_ticks();
  s->obj_mesh = (int32_t*)realloc(s->obj_mesh, (s->nobj + 1) * 4); s->obj_xf = (float*)realloc(s->obj_xf, (s->nobj + 1) * 64);
  s->obj_mesh[s->nobj] = mesh_id; memcpy(s->obj_xf + s->nobj * 16, xf16, 64); s->nobj++;
  return ticks_to_ms(qpc_ticks() - t0);
}
EXPORT void sb_scene_totals(void* p, int64_t* out) {
  SbScene* s = (SbScene*)p; int64_t meshes = 0, tris = 0, inst_tris = 0;
  for (auto& m : s->meshes) if (m.pos) { meshes++; tris += m.nt; }
  for (int64_t i = 0; i < s->nobj; i++) inst_tris += s->meshes[s->obj_mesh[i]].nt;
  out[0] = meshes; out[1] = tris; out[2] = s->nobj; out[3] = inst_tris;
}
