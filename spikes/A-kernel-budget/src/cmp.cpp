// Compares two output dumps (float32 arrays). Reports max/mean relative difference and ULP distance per output,
// excluding paths where a discrete choice (lobe / light) differs; those are counted separately.
// Usage: cmp.exe A.bin B.bin   (B is the reference)
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
struct Dump { int n = 0; std::vector<std::string> names; std::vector<std::vector<float>> a; };
static bool load(const char* p, Dump& d) {
  FILE* f = fopen(p, "rb"); if (!f) return false;
  int cnt; fread(&d.n, 4, 1, f); fread(&cnt, 4, 1, f);
  for (int i = 0; i < cnt; ++i) { char nb[32]; fread(nb, 1, 32, f); d.names.push_back(nb); d.a.emplace_back(d.n); fread(d.a.back().data(), 4, d.n, f); }
  fclose(f); return true;
}
static long long ordered(float x) { int i; memcpy(&i, &x, 4); return i < 0 ? (long long)INT_MIN - i : (long long)i; }
static double ulps(float a, float b) {
  if (isnan(a) || isnan(b)) return (isnan(a) && isnan(b)) ? 0 : 4e9;
  if (isinf(a) || isinf(b)) return a == b ? 0 : 4e9;
  if (fabsf(a) < 1e-30f && fabsf(b) < 1e-30f) return 0;
  return (double)llabs(ordered(a) - ordered(b));
}
struct Stat {
  long n = 0; double maxrel = 0, sumrel = 0, maxulp = 0, sumulp = 0; long gt[5] = {0, 0, 0, 0, 0}; // >0, >1, >4, >64, >4096 ulp
  void add(float a, float b) {
    double u = ulps(a, b);
    double den = std::max(fabs((double)a), fabs((double)b));
    double rel = den < 1e-30 ? 0 : fabs((double)a - (double)b) / den;
    if (isnan(rel)) rel = 1e9;
    ++n; sumrel += rel; sumulp += u; maxrel = std::max(maxrel, rel); maxulp = std::max(maxulp, u);
    if (u > 0) ++gt[0]; if (u > 1) ++gt[1]; if (u > 4) ++gt[2]; if (u > 64) ++gt[3]; if (u > 4096) ++gt[4];
  }
};
int main(int argc, char** argv) {
  if (argc < 3) return 1;
  Dump A, B; if (!load(argv[1], A) || !load(argv[2], B)) { fprintf(stderr, "load failed\n"); return 1; }
  int n = A.n;
  auto find = [&](Dump& d, const char* nm) { for (size_t i = 0; i < d.names.size(); ++i) if (d.names[i] == nm) return (int)i; return -1; };
  int lobeIdx = find(A, "sample.lobe"), lightIdx = find(A, "nee.light"), matIdx = find(A, "aux.matclass"), cosIdx = find(A, "aux.coso");
  long lobeFlip = 0, lightFlip = 0;
  std::vector<char> lobeOk(n), lightOk(n);
  for (int i = 0; i < n; ++i) { lobeOk[i] = A.a[lobeIdx][i] == B.a[lobeIdx][i]; lightOk[i] = A.a[lightIdx][i] == B.a[lightIdx][i]; lobeFlip += !lobeOk[i]; lightFlip += !lightOk[i]; }
  printf("DIFF %s vs %s: n=%d, lobe choice differs on %ld (%.4f%%), light choice differs on %ld (%.4f%%)\n", argv[1], argv[2], n, lobeFlip, 100.0 * lobeFlip / n, lightFlip, 100.0 * lightFlip / n);
  printf("%-22s %9s %10s %10s %9s %9s %8s %8s %8s %8s\n", "output", "n", "maxrel", "meanrel", "maxulp", "meanulp", ">1ulp%", ">4ulp%", ">64ulp%", ">4096%");
  std::map<std::string, Stat> st;
  std::map<std::string, std::vector<std::pair<int, int>>> ordering; // not used
  // per-class (material class x output group) for >64 ulp
  std::map<std::string, std::vector<Stat>> byMat, byCos;
  for (size_t k = 0; k < A.names.size(); ++k) {
    const std::string& nm = A.names[k];
    if (nm.rfind("aux.", 0) == 0 || nm == "sample.lobe" || nm == "nee.light") continue;
    bool isSample = nm.rfind("sample.", 0) == 0, isNee = nm.rfind("nee.", 0) == 0;
    if (!byMat.count(nm)) { byMat[nm].resize(8); byCos[nm].resize(3); }
    for (int i = 0; i < n; ++i) {
      if (isSample && !lobeOk[i]) continue;
      if (isNee && !lightOk[i]) continue;
      st[nm].add(A.a[k][i], B.a[k][i]);
      int mc = (int)A.a[matIdx][i] & 7; float c = A.a[cosIdx][i];
      byMat[nm][mc].add(A.a[k][i], B.a[k][i]);
      byCos[nm][c < 0.05f ? 0 : c < 0.25f ? 1 : 2].add(A.a[k][i], B.a[k][i]);
    }
  }
  for (auto& kv : st) {
    const Stat& s = kv.second;
    printf("%-22s %9ld %10.3e %10.3e %9.0f %9.2f %8.4f %8.4f %8.4f %8.4f\n", kv.first.c_str(), s.n, s.maxrel, s.sumrel / s.n, s.maxulp, s.sumulp / s.n,
           100.0 * s.gt[1] / s.n, 100.0 * s.gt[2] / s.n, 100.0 * s.gt[3] / s.n, 100.0 * s.gt[4] / s.n);
  }
  // Vector-level errors: relative L2 error of 4-wavelength values and angular error of directions.
  printf("VECTOR errors (value groups: ||a-b||/||b||; direction groups: angle in radians; float eps = 1.19e-07)\n");
  printf("%-22s %9s %10s %10s %10s %10s\n", "group", "n", "max", "mean", "p99", "p99.9");
  auto gi = [&](const char* nm, int k) { std::string s = nm; int c = 0; for (size_t i = 0; i < A.names.size(); ++i) if (A.names[i] == s) { if (c == k) return (int)i; ++c; } return -1; };
  struct G { const char* label; const char* nm; bool dir; const char* nm2; const char* nm3; int mask; };
  G gs[] = {{"eval.f", "eval.f", false, 0, 0, 0}, {"sample.f", "sample.f", false, 0, 0, 1}, {"sample.throughput", "sample.throughput", false, 0, 0, 1},
            {"nee.contrib", "nee.contrib", false, 0, 0, 2}, {"sample.wi (dir)", "sample.wi.x", true, "sample.wi.y", "sample.wi.z", 1},
            {"sample.nextdir (dir)", "sample.nextdir.x", true, "sample.nextdir.y", "sample.nextdir.z", 1}, {"nee.dir (dir)", "nee.dir.x", true, "nee.dir.y", "nee.dir.z", 2}};
  for (auto& g : gs) {
    std::vector<double> e; e.reserve(n);
    for (int i = 0; i < n; ++i) {
      if (g.mask == 1 && !lobeOk[i]) continue;
      if (g.mask == 2 && !lightOk[i]) continue;
      double v;
      if (g.dir) {
        int ix = find(A, g.nm), iy = find(A, g.nm2), iz = find(A, g.nm3);
        double ax = A.a[ix][i], ay = A.a[iy][i], az = A.a[iz][i], bx = B.a[ix][i], by = B.a[iy][i], bz = B.a[iz][i];
        double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
        v = atan2(sqrt(cx * cx + cy * cy + cz * cz), ax * bx + ay * by + az * bz);
      } else {
        double num = 0, den = 0;
        for (int k = 0; k < 4; ++k) { int ii = gi(g.nm, k); double a = A.a[ii][i], b = B.a[ii][i]; num += (a - b) * (a - b); den += b * b; }
        v = den > 1e-60 ? sqrt(num / den) : (num > 1e-60 ? 1e9 : 0);
      }
      if (isnan(v)) v = 1e9;
      e.push_back(v);
    }
    if (e.empty()) continue;
    double mx = 0, sm = 0; for (double x : e) { mx = std::max(mx, x); sm += x; }
    size_t p99 = (size_t)(0.99 * (e.size() - 1)), p999 = (size_t)(0.999 * (e.size() - 1));
    std::nth_element(e.begin(), e.begin() + p99, e.end()); double v99 = e[p99];
    std::nth_element(e.begin(), e.begin() + p999, e.end()); double v999 = e[p999];
    printf("%-22s %9zu %10.3e %10.3e %10.3e %10.3e\n", g.label, e.size(), mx, sm / e.size(), v99, v999);
  }
  printf("CLASS: percent of values >64 ulp, by material class (0 gold,1 rough metal,2 clear glass,3 rough glass,4 coated,5 fuzz,6 thin film,7 emissive)\n");
  for (auto& kv : byMat) {
    if (kv.first == "sample.nextdir.x" || kv.first == "sample.nextdir.y" || kv.first == "sample.nextdir.z" || kv.first == "sample.wi.x" || kv.first == "sample.wi.y" || kv.first == "nee.dir.x" || kv.first == "nee.dir.y") continue;
    printf("%-22s", kv.first.c_str());
    for (int m = 0; m < 8; ++m) printf(" %7.3f", kv.second[m].n ? 100.0 * kv.second[m].gt[3] / kv.second[m].n : 0.0);
    printf("\n");
  }
  printf("CLASS: percent >64 ulp by cos(theta_o): <0.05 | 0.05-0.25 | >0.25\n");
  for (auto& kv : byCos) {
    if (kv.first.find(".x") != std::string::npos || kv.first.find(".y") != std::string::npos) continue;
    printf("%-22s", kv.first.c_str());
    for (int m = 0; m < 3; ++m) printf(" %7.3f", kv.second[m].n ? 100.0 * kv.second[m].gt[3] / kv.second[m].n : 0.0);
    printf("   (n=%ld/%ld/%ld)\n", kv.second[0].n, kv.second[1].n, kv.second[2].n);
  }
  return 0;
}
