// Self-check of the pacing panel's numbers (src/pacing.h). Plain asserts, no framework.
#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/pacing.h"

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

int main() {
   const uint64_t S = 1000000000ull, base = 1000 * S; // base is a multiple of 250 ms
   const double ft = 1e9 / 60.0;
   PacingRing r;
   for (int i = 0; i < 660; i++) r.push(base + uint64_t(std::llround(i * ft)), float(ft / 1e6));
   uint64_t now = base + uint64_t(std::llround(659 * ft)) + 1000;

   // A game locked at 60 fps reads flat.
   PacingNumbers n = pacing_numbers(r, now, 60, 60);
   for (int j = 0; j < PacingNumbers::BARS; j++) { assert(!std::isnan(n.bars[j])); assert(near(n.bars[j], 60, 0.1f)); }
   assert(near(n.hero, 60, 0.1f) && near(n.avg, 60, 0.1f) && near(n.low, 60, 0.1f));
   assert(near(n.mn, 60, 0.1f) && near(n.mx, 60, 0.1f));
   assert(n.on == n.n10 && n.late == 0 && n.stall == 0);
   assert(near(n.run_avg, 60, 0.1f) && near(n.run1, 60, 0.2f) && near(n.run01, 60, 0.2f));
   assert(n.missed == 0);

   // One 100 ms frame: a stall, a missed frame, and the 0.1% low drops to it.
   now += uint64_t(100 * 1e6);
   r.push(now, 100.f);
   n = pacing_numbers(r, now + 1000, 60, 60);
   assert(n.stall == 1 && n.late == 0);
   assert(near(n.missed, 1.f / 661, 1e-4f));
   assert(near(n.run01, 10, 0.1f));
   assert(near(n.run1, 60, 0.2f));
   assert(n.hero < 60 && n.hero > 40);

   // A frame straddling a slice boundary counts partly in each slice.
   PacingRing s;
   s.push(base + 10 * 1000000, 20.f);
   double a = pacing_weighted(s, 1, base - 250 * 1000000, base, base + 10 * 1000000, 0);
   double b = pacing_weighted(s, 1, base, base + 250 * 1000000, base + 10 * 1000000, 0);
   assert(near(float(a), 0.5f, 1e-6f) && near(float(b), 0.5f, 1e-6f));

   // Above 819 fps the ring holds less than 10 s: the walk must stop at the ring's edge and
   // the windows shrink to what it holds, so 2000 fps still reads 2000.
   PacingRing h;
   const double ft2k = 1e9 / 2000.0;
   for (int i = 0; i < 20000; i++) h.push(base + uint64_t(std::llround(i * ft2k)), float(ft2k / 1e6));
   now = base + uint64_t(std::llround(19999 * ft2k)) + 1000;
   n = pacing_numbers(h, now, 60, 60);
   assert(near(n.hero, 2000, 2.f) && near(n.avg, 2000, 2.f) && near(n.low, 2000, 2.f));
   int valid = 0;
   for (int j = 0; j < PacingNumbers::BARS; j++) if (!std::isnan(n.bars[j])) { valid++; assert(near(n.bars[j], 2000, 2.f)); }
   assert(valid > 10 && valid < PacingNumbers::BARS); // 4 s of ring, not 10
   assert(n.n10 == int(PacingRing::N));

   // Nothing yet: everything is unknown, nothing crashes.
   PacingRing e;
   n = pacing_numbers(e, now, 60, 0);
   assert(std::isnan(n.hero) && std::isnan(n.avg) && std::isnan(n.low) && n.n10 == 0);

   // Reset forgets the stall and the run; the next frames start a fresh run.
   r.reset();
   n = pacing_numbers(r, now, 60, 60);
   assert(std::isnan(n.run_avg) && std::isnan(n.missed) && n.n10 == 0);
   for (int i = 0; i < 60; i++) r.push(now + uint64_t(std::llround((i + 1) * ft)), float(ft / 1e6));
   n = pacing_numbers(r, now + uint64_t(std::llround(60 * ft)) + 1000, 60, 60);
   assert(n.n10 == 60 && n.missed == 0 && near(n.run_avg, 60, 0.1f) && near(n.run1, 60, 0.2f));

   // Temperature history: starts at the first real reading, fills skipped slices with NaN, keeps the last N.
   TempHistory t;
   t.push(10, 0);                       // nothing read yet: not stored
   assert(t.n == 0);
   t.push(11, 60); t.push(11, 61);      // one slot per slice
   assert(t.n == 1 && t.v[0] == 60);
   t.push(14, 64);                      // slices 12 and 13 were never drawn
   assert(t.n == 4 && std::isnan(t.v[1]) && std::isnan(t.v[2]) && t.v[3] == 64);
   t.push(15, 0);                       // sensor lost its reading
   assert(t.n == 5 && std::isnan(t.v[4]));
   t.push(15 + 1000, 70);               // a very long gap: only the newest N slots survive
   assert(t.n == TempHistory::N && t.v[TempHistory::N - 1] == 70 && std::isnan(t.v[0]));

   puts("pacing ok");
   return 0;
}
