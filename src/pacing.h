#pragma once
// Frame history and the numbers the pacing panel shows. Header-only, no ImGui,
// so tests/test_pacing.cpp can include it. Mirrors capture-fps-app overlay.js:
// frames are counted by the share of their on-screen time inside a window, so a
// locked frame rate reads flat.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

struct PacingRing {
   struct Frame { uint64_t end_ns; float ms; };      // start = end - ms*1e6
   static constexpr uint32_t N = 8192;                 // 10 s of history up to 819 fps
   static constexpr uint32_t HIST = 10000;             // 0.1 ms bins up to 999.9 ms

   std::array<Frame, N> f {};
   std::atomic<uint32_t> n {0};                        // frames pushed so far
   std::array<uint32_t, HIST> hist {};                 // run-wide frame times for percentiles and MISSED
   double sum_ms = 0;
   uint64_t first_start_ns = 0;

   void push(uint64_t now_ns, float ms) {
      uint32_t i = n.load(std::memory_order_relaxed);
      if (!i) first_start_ns = now_ns - uint64_t(ms * 1e6);
      f[i & (N - 1)] = { now_ns, ms };
      hist[std::min<uint32_t>(HIST - 1, uint32_t(ms * 10.f))]++;
      sum_ms += ms;
      n.store(i + 1, std::memory_order_release);
   }

   const Frame& at(uint32_t i) const { return f[i & (N - 1)]; } // i counts from the first frame
   /** Index of the oldest frame still in the ring. */
   static uint32_t oldest(uint32_t n) { return n > N ? n - N : 0; }
   /** Start time of the oldest frame still in the ring. */
   uint64_t ring_start_ns(uint32_t n) const {
      if (n <= N) return first_start_ns;
      const Frame& o = at(n - N);
      return o.end_ns - std::min(o.end_ns, uint64_t(o.ms * 1e6));
   }
};

struct PacingNumbers {
   static constexpr int BARS = 40;
   static constexpr float BUCKET_S = 0.25f;
   float hero, avg, low, run_avg, run1, run01, missed;   // NaN = not yet known ("–")
   int on, late, stall, n10;                             // pacing shares over 10 s
   float bars[BARS];                                     // fps per 0.25 s slice, oldest first; NaN = before the run started
   float mn, mx;
};

// Fonts the panel bakes into the atlas (face: 0 Mono SemiBold, 1 Mono Medium, 2 Sans Cond SemiBold, 3 Sans Cond Medium).
enum PacingFont { PF_MONO_SEMI_72, PF_MONO_SEMI_26, PF_MONO_SEMI_20, PF_MONO_SEMI_16, PF_MONO_SEMI_13,
                  PF_MONO_MED_13, PF_SANS_SEMI_16, PF_SANS_SEMI_14, PF_SANS_MED_13, PF_COUNT };
struct PacingFontSpec { int face; float px; };
constexpr PacingFontSpec pacing_font_specs[PF_COUNT] = {
   {0, 72}, {0, 26}, {0, 20}, {0, 16}, {0, 13}, {1, 13}, {2, 16}, {2, 14}, {3, 13},
};

/** Linear-interpolated percentile of an ascending vector (overlay analysis.js percentile). */
inline float pacing_percentile(const std::vector<float>& s, float p) {
   if (s.empty()) return NAN;
   float k = (s.size() - 1) * p;
   size_t lo = size_t(k);
   if (lo + 1 >= s.size()) return s.back();
   return s[lo] + (s[lo + 1] - s[lo]) * (k - lo);
}

/**
 * Frames in [t0, t1), each weighted by the share of its on-screen time inside
 * the window. The frame still on screen counts for the time it has been there,
 * never less than one target frame (overlay.js weightedFrames).
 */
inline double pacing_weighted(const PacingRing& r, uint32_t n, uint64_t t0, uint64_t t1, uint64_t now, uint64_t floor_ns) {
   double total = 0;
   auto add = [&](uint64_t st, uint64_t en, double dur) {
      if (st >= t1 || en <= t0) return;
      double ov = double(std::min(en, t1) - std::max(st, t0));
      if (ov > 0 && dur > 0) total += ov / dur;
   };
   if (n) { // live frame
      uint64_t st = r.at(n - 1).end_ns;
      if (now > st) add(st, now, double(std::max(now - st, floor_ns)));
   }
   for (uint32_t i = n; i-- > PacingRing::oldest(n);) {
      const auto& fr = r.at(i);
      if (fr.end_ns <= t0) break;
      uint64_t d = uint64_t(fr.ms * 1e6);
      add(fr.end_ns - std::min(d, fr.end_ns), fr.end_ns, double(d));
   }
   return total;
}

inline PacingNumbers pacing_numbers(const PacingRing& r, uint64_t now, float target, float refresh_hz) {
   PacingNumbers o {};
   o.hero = o.avg = o.low = o.run_avg = o.run1 = o.run01 = o.missed = o.mn = o.mx = NAN;
   for (auto& b : o.bars) b = NAN;
   uint32_t n = r.n.load(std::memory_order_acquire);
   const float target_ms = 1000.f / target;
   const float refresh_ms = refresh_hz > 0 ? 1000.f / refresh_hz : target_ms;
   const int kRef = std::max(1, int(std::lround(target_ms / refresh_ms)));
   const uint64_t floor_ns = uint64_t(target_ms * 1e6), S = 1000000000ull;
   // Windows never reach past the oldest frame in the ring (above 819 fps the ring holds less than 10 s).
   const uint64_t ring_start = r.ring_start_ns(n);
   if (!n || now <= ring_start) return o;
   const uint64_t since_start = now - ring_start;
   const uint64_t min_span = uint64_t(refresh_ms * 1e6);

   auto rate = [&](uint64_t span) -> float {
      span = std::min(span, since_start);
      if (span < min_span) return NAN;
      return float(pacing_weighted(r, n, now - span, now, now, floor_ns) * S / double(span));
   };
   o.hero = rate(S / 2);
   o.avg = rate(10 * S);

   std::vector<float> w10;
   for (uint32_t i = n; i-- > PacingRing::oldest(n);) {
      const auto& fr = r.at(i);
      if (fr.end_ns + 10 * S <= now) break;
      w10.push_back(fr.ms);
      int k = int(std::lround(fr.ms / refresh_ms));
      if (k <= kRef) o.on++; else if (k <= 2 * kRef) o.late++; else o.stall++;
   }
   o.n10 = int(w10.size());
   if (w10.size() >= 20) {
      std::sort(w10.begin(), w10.end());
      o.low = 1000.f / pacing_percentile(w10, 0.99f);
   }

   // Run so far from the histogram: percentiles and the share over the cap.
   if (r.sum_ms > 0) o.run_avg = float(1000.0 * n / r.sum_ms);
   if (n >= 20) {
      uint64_t seen = 0, want1 = uint64_t(std::ceil(0.99 * n)), want01 = uint64_t(std::ceil(0.999 * n)), missed = 0;
      uint32_t cap = uint32_t((target_ms + 0.5f * refresh_ms) * 10.f);
      for (uint32_t b = 0; b < PacingRing::HIST; b++) {
         uint32_t c = r.hist[b];
         if (!c) continue;
         if (b > cap) missed += c;
         uint64_t before = seen;
         seen += c;
         float ms = (b + 0.5f) / 10.f;
         if (before < want1 && seen >= want1) o.run1 = 1000.f / ms;
         if (before < want01 && seen >= want01) o.run01 = 1000.f / ms;
      }
      o.missed = float(missed) / float(n);
   }

   // 40 slices of 250 ms ending at the last whole slice boundary.
   const uint64_t B = S / 4, bucket_end = now - now % B;
   for (int j = 0; j < PacingNumbers::BARS; j++) {
      uint64_t s = bucket_end - uint64_t(PacingNumbers::BARS - j) * B;
      if (bucket_end < uint64_t(PacingNumbers::BARS - j) * B || s < ring_start) continue;
      float fps = float(pacing_weighted(r, n, s, s + B, now, floor_ns) / PacingNumbers::BUCKET_S);
      o.bars[j] = fps;
      o.mn = std::isnan(o.mn) ? fps : std::min(o.mn, fps);
      o.mx = std::isnan(o.mx) ? fps : std::max(o.mx, fps);
   }
   return o;
}

extern PacingRing pacing;

struct swapchain_stats;
struct ImVec2;
void render_pacing_panel(swapchain_stats& data, ImVec2& window_size);
