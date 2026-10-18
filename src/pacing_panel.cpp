// The frame pacing panel: a port of the capture-fps-app "Panel" overlay
// (src/core/overlay.js) drawn with ImDrawList. Layout is in design px at 1080p,
// scaled by k = screen height / 1080; fonts are baked at that scale by create_fonts.
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <mesa/util/os_time.h>
#include "overlay.h"
#include "hud_elements.h"
#include "fps_limiter.h"
#include "pacing.h"

PacingRing pacing;

namespace {

constexpr float PW = 380, PAD = 16, IW = PW - 2 * PAD, PH = 476, CHART_H = 72;
constexpr float TITLE_BASE = 31.7f, PILL_TOP = 16.5f, PILL_H = 20.6f, PILL_BASE = 31.3f;
constexpr float EYEBROW_BASE = 58, GRID_LABEL_BASE = 76.2f, GRID_VALUE_BASE = 109.25f;
constexpr float HERO_BASE = 111, HERO_FPS_BASE = 94, LAST_BASE = 146, RUN_LABEL_BASE = 168, RUN_VALUE_BASE = 190;
constexpr float FPS_LABEL_BASE = 216, FPS_TOP = 226, FT_LABEL_BASE = 322, FT_TOP = 332;
constexpr float PACING_BASE = 428, PACING_Y = 436, PACING_H = 14, FOOT_BASE = 466;
constexpr float COL1_R = PW - PAD - 62.4f - 18, COL2_R = PW - PAD;
constexpr int FT_SHOW = 89, MAX_OVERFLOW_LABELS = 4;
constexpr float DESCENT = 0.275f; // libass anchors text by ascent + descent; the design's baselines assume Plex's 0.275 em descent

constexpr ImU32 rgb(unsigned c, int a = 255) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, a); }
// shortcut: palette is sRGB, HDR (transfer_function) surfaces will look off; upgrade by hoisting convert_colors' lambda
constexpr ImU32 PANEL = rgb(0x0A0C0F, 219), PANEL_SOLID = rgb(0x0A0C0F), LINE = rgb(0x3A434C);
constexpr ImU32 TEXT = rgb(0xEEF2F5), LABEL = rgb(0xC9D0D6), DIM = rgb(0xA7B0B8), FOOT = rgb(0x8E98A1);
constexpr ImU32 BLUE = rgb(0x5AA9E6), AMBER = rgb(0xF5B13D), RED = rgb(0xF2545B);
constexpr ImU32 GRID_STRONG = rgb(0xEEF2F5, 115), GRID_FAINT = rgb(0xEEF2F5, 56);
constexpr ImU32 with_alpha(ImU32 c, float a) { return (c & ~IM_COL32_A_MASK) | (ImU32(a * 255 + 0.5f) << IM_COL32_A_SHIFT); }

ImU32 level_color(float fps, float target) { return fps >= 0.95f * target ? TEXT : fps >= 0.8f * target ? AMBER : RED; }
ImU32 bar_color(float fps, float target) { return fps >= 0.95f * target ? BLUE : fps >= 0.8f * target ? AMBER : RED; }
ImU32 ft_bar_color(float ms, float t) { return ms <= t * 1.2f ? BLUE : ms <= t * 2.05f ? AMBER : RED; }
ImU32 ft_text_color(float ms, float t) { return ms <= t * 1.2f ? TEXT : ms <= t * 2.05f ? AMBER : RED; }

const char* const DASH = "\xE2\x80\x93"; // –

const char* fmt1(char* b, size_t n, float v) { if (std::isnan(v)) return DASH; snprintf(b, n, "%.1f", v); return b; }
const char* fmt0(char* b, size_t n, float v) { if (std::isnan(v)) return DASH; snprintf(b, n, "%.0f", v); return b; }
/** Whole number when it is one, else one decimal: 16.7, 33.3, 50, 100. */
const char* fmt_ms(char* b, size_t n, float v) { snprintf(b, n, std::fabs(v - std::round(v)) < 0.05f ? "%.0f" : "%.1f", v); return b; }

struct Run { ImFont* f; float size; ImU32 col; const char* s; };

struct Painter {
   ImDrawList* dl;
   ImVec2 o;
   float k;
   ImFont** F;

   ImVec2 P(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
   float width(ImFont* f, float size, const char* s) const { return f->CalcTextSizeA(size * k, FLT_MAX, 0.f, s).x / k; }

   /** One text run with its baseline at (x, base) in design px; returns its width in design px. */
   float text(float x, float base, ImFont* f, float size, ImU32 col, const char* s, char align = 'l', float clip_w = 0) const {
      float w = width(f, size, s);
      if (align == 'r') x -= w; else if (align == 'c') x -= w / 2;
      float sz = size * k, top = base * k - f->Ascent * (sz / f->FontSize);
      ImVec2 pos(o.x + x * k, o.y + top);
      if (clip_w > 0) {
         ImVec4 clip(pos.x, pos.y, o.x + (x + clip_w) * k, pos.y + sz * 1.5f);
         dl->AddText(f, sz, pos, col, s, nullptr, 0.f, &clip);
      } else {
         dl->AddText(f, sz, pos, col, s);
      }
      return w;
   }

   /** Several runs on one baseline, aligned as a whole. */
   void runs(float x, float base, std::initializer_list<Run> rs, char align = 'l') const {
      if (align != 'l') {
         float total = 0;
         for (auto& r : rs) total += width(r.f, r.size, r.s);
         x -= align == 'r' ? total : total / 2;
      }
      for (auto& r : rs) x += text(x, base, r.f, r.size, r.col, r.s);
   }

   void rect(float x, float y, float w, float h, ImU32 col, float rounding = 0) const {
      dl->AddRectFilled(P(x, y), P(x + w, y + h), col, rounding * k);
   }
   void outline(float x, float y, float w, float h, ImU32 col, float rounding = 0) const {
      dl->AddRect(P(x, y), P(x + w, y + h), col, rounding * k, 0, 1.f * k);
   }

   /** Hollow pill with mono text, right edge at right_x; returns its width. */
   float pill(float right_x, const char* s) const {
      float w = width(F[PF_MONO_MED_13], 13, s) + 16;
      rect(right_x - w + 0.5f, PILL_TOP, w - 1, PILL_H, PANEL_SOLID, 4);
      outline(right_x - w + 0.5f, PILL_TOP, w - 1, PILL_H, LINE, 4);
      text(right_x - w / 2, PILL_BASE, F[PF_MONO_MED_13], 13, DIM, s, 'c');
      return w;
   }

   /** Small mono label in a panel-coloured box, left-aligned at left/top. */
   void chart_label_left(float left, float top, const char* s) const {
      const float size = 13, w = width(F[PF_MONO_MED_13], size, s) + 8, h = size * 1.3f;
      rect(left, top, w, h, PANEL, 2);
      text(left + 4, top + h - DESCENT * size, F[PF_MONO_MED_13], size, LABEL, s);
   }
};

struct Model { float target, target_ms, refresh_hz, refresh_ms, stall_ms, y_max; int k_ref; };

Model model() {
   float target = 60;
   if (fps_limiter && fps_limiter->active && fps_limiter->current_limit() > 0)
      target = fps_limiter->current_limit();
   else if (HUDElements.refresh > 0)
      target = HUDElements.refresh;
   Model m;
   m.target = target;
   m.target_ms = 1000.f / target;
   m.refresh_hz = HUDElements.refresh > 0 ? HUDElements.refresh : 0.f;
   m.refresh_ms = m.refresh_hz > 0 ? 1000.f / m.refresh_hz : m.target_ms;
   m.k_ref = std::max(1, int(std::lround(m.target_ms / m.refresh_ms)));
   m.stall_ms = std::max(100.f, 3 * m.target_ms);
   m.y_max = std::max(target, std::min(1.25f * target, m.refresh_hz > 0 ? m.refresh_hz : 1.25f * target));
   return m;
}

} // namespace

void render_pacing_panel(swapchain_stats& data, ImVec2& window_size)
{
   auto& io = ImGui::GetIO();
   float k = io.DisplaySize.y > 0 ? io.DisplaySize.y / 1080.f : 1.f;
   window_size = ImVec2(PW * k, PH * k);
   data.pacing_k_wanted = k;
   if (!data.pacing_fonts[0] || data.pacing_k != k) {
      data.font_params_hash = 0; // the host rebuilds the atlas when the hashes differ
      return;
   }

   auto& params = *HUDElements.params;
   const Model m = model();
   const uint64_t now = os_time_get_nano();
   const uint32_t n = pacing.n.load(std::memory_order_acquire);

   // Numbers tick every 250 ms; the frame-time chart, the stall counter and the timecode every frame.
   static PacingNumbers num {};
   static uint64_t num_bucket = ~0ull;
   static float num_target = 0, num_refresh = 0;
   const uint64_t bucket = now / 250000000ull;
   if (bucket != num_bucket || m.target != num_target || m.refresh_hz != num_refresh) {
      num = pacing_numbers(pacing, now, m.target, m.refresh_hz);
      num_bucket = bucket; num_target = m.target; num_refresh = m.refresh_hz;
   }

   ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
   ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
   Painter p { ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), k, data.pacing_fonts };
   ImFont* MONO72 = p.F[PF_MONO_SEMI_72]; ImFont* MONO26 = p.F[PF_MONO_SEMI_26]; ImFont* MONO20 = p.F[PF_MONO_SEMI_20];
   ImFont* MONO16 = p.F[PF_MONO_SEMI_16]; ImFont* MONO13 = p.F[PF_MONO_SEMI_13]; ImFont* MONOM13 = p.F[PF_MONO_MED_13];
   ImFont* SANS16 = p.F[PF_SANS_SEMI_16]; ImFont* SANS14 = p.F[PF_SANS_SEMI_14]; ImFont* SANSM13 = p.F[PF_SANS_MED_13];
   char b1[64], b2[64], b3[64], b4[64];

   // ---- chrome ----
   p.rect(0, 0, PW, PH, PANEL, 10);
   float right = PW - PAD;
   snprintf(b1, sizeof b1, "TARGET %d", int(std::lround(m.target)));
   right -= p.pill(right, b1) + 6;
   if (m.refresh_hz > 0) snprintf(b1, sizeof b1, "%dP%d", int(io.DisplaySize.y), int(std::lround(m.refresh_hz)));
   else snprintf(b1, sizeof b1, "%dP", int(io.DisplaySize.y));
   right -= p.pill(right, b1) + 6;
   std::string title = params.custom_text_center.empty() ? engine_name(data) : params.custom_text_center;
   for (auto& c : title) c = toupper(static_cast<unsigned char>(c));
   p.text(PAD, TITLE_BASE, SANS14, 14, DIM, title.c_str(), 'l', right + 6 - PAD - 8);

   p.text(COL2_R, EYEBROW_BASE, SANSM13, 13, FOOT, "LAST 10 S", 'r');
   p.text(COL1_R, GRID_LABEL_BASE, SANS16, 16, LABEL, "AVG", 'r');
   p.text(COL2_R, GRID_LABEL_BASE, SANS16, 16, LABEL, "1% LOW", 'r');
   const char* run_labels[] = { "RUN AVG", "1% LOW", "0.1% LOW", "MISSED" };
   for (int i = 0; i < 4; i++) p.text(PAD + IW * (i + 1) / 4, RUN_LABEL_BASE, SANSM13, 13, FOOT, run_labels[i], 'r');

   p.rect(PAD, FPS_TOP + CHART_H, IW, 1, LINE);
   p.rect(PAD, FT_TOP + CHART_H, IW, 1, LINE);
   const float ty = FPS_TOP + CHART_H - CHART_H * (m.target / m.y_max);
   p.rect(PAD, ty, IW, 1, GRID_STRONG);
   snprintf(b1, sizeof b1, "%d", int(std::lround(m.target)));
   p.chart_label_left(PAD, ty + 3, b1);
   const float g1 = CHART_H / 3, g2 = CHART_H * 2 / 3;
   p.rect(PAD, FT_TOP + CHART_H - g1, IW, 1, GRID_STRONG);
   p.rect(PAD, FT_TOP + CHART_H - g2, IW, 1, GRID_FAINT);
   snprintf(b1, sizeof b1, "%.1f ms", m.target_ms);
   p.chart_label_left(PAD, FT_TOP + CHART_H - g1 + 3, b1);
   snprintf(b1, sizeof b1, "%.1f", 2 * m.target_ms);
   p.chart_label_left(PAD, FT_TOP + CHART_H - g2 + 3, b1);
   p.text(PAD, PACING_BASE, SANS16, 16, LABEL, "PACING \xC2\xB7 10 S");
   const char* limit = fps_limiter && fps_limiter->active ? (fps_limiter->use_early ? "limit early" : "limit late") : "no limit";
   snprintf(b1, sizeof b1, "%s \xC2\xB7 %s", engine_name(data), limit);
   p.text(PAD, FOOT_BASE, SANSM13, 13, FOOT, b1);

   // ---- numbers (250 ms) ----
   const char* hero = fmt0(b1, sizeof b1, num.hero);
   float hero_w = p.text(PAD, HERO_BASE, MONO72, 72, std::isnan(num.hero) ? TEXT : level_color(num.hero, m.target), hero);
   p.text(PAD + hero_w + 8, HERO_FPS_BASE, SANS16, 16, LABEL, "FPS");
   p.text(PAD + hero_w + 8, HERO_BASE, SANSM13, 13, FOOT, "0.5 S");
   p.text(COL1_R, GRID_VALUE_BASE, MONO26, 26, std::isnan(num.avg) ? TEXT : level_color(num.avg, m.target), fmt1(b1, sizeof b1, num.avg), 'r');
   p.text(COL2_R, GRID_VALUE_BASE, MONO26, 26, std::isnan(num.low) ? TEXT : level_color(num.low, m.target), fmt1(b1, sizeof b1, num.low), 'r');
   if (std::isnan(num.missed)) snprintf(b4, sizeof b4, "%s", DASH); else snprintf(b4, sizeof b4, "%d%%", int(std::lround(num.missed * 100)));
   const char* run_vals[] = { fmt1(b1, sizeof b1, num.run_avg), fmt1(b2, sizeof b2, num.run1), fmt1(b3, sizeof b3, num.run01), b4 };
   for (int i = 0; i < 4; i++) p.text(PAD + IW * (i + 1) / 4, RUN_VALUE_BASE, MONO20, 20, TEXT, run_vals[i], 'r');

   const float bw = (IW - (PacingNumbers::BARS - 1) * 2) / PacingNumbers::BARS;
   for (int j = 0; j < PacingNumbers::BARS; j++) {
      float fps = num.bars[j];
      if (std::isnan(fps)) continue;
      float h = std::max(2.f, std::min(1.f, fps / m.y_max) * CHART_H);
      p.rect(PAD + j * (bw + 2), FPS_TOP + CHART_H - h, bw, h, bar_color(fps, m.target));
   }
   p.runs(PAD, FPS_LABEL_BASE, { {SANS16, 16, LABEL, "FPS \xC2\xB7 LAST 10 S"}, {SANSM13, 13, FOOT, " \xC2\xB7 \xC2\xBC S SLICES"} });
   p.runs(PW - PAD, FPS_LABEL_BASE, { {SANS16, 16, LABEL, "MIN "}, {MONO16, 16, TEXT, fmt0(b1, sizeof b1, num.mn)},
                                      {SANS16, 16, LABEL, " \xC2\xB7 MAX "}, {MONO16, 16, TEXT, fmt0(b2, sizeof b2, num.mx)} }, 'r');

   if (num.n10 > 0) {
      auto ms = [&](char* b, size_t sz, int c) { return fmt_ms(b, sz, c * m.refresh_ms); };
      char lab_on[32], lab_late[48], lab_stall[32], t1[16], t2[16];
      if (m.k_ref == 1) snprintf(lab_on, sizeof lab_on, "%s ms", ms(t1, sizeof t1, 1));
      else snprintf(lab_on, sizeof lab_on, "\xE2\x89\xA4%s ms", ms(t1, sizeof t1, m.k_ref));
      if (m.k_ref == 1) snprintf(lab_late, sizeof lab_late, "%s ms", ms(t1, sizeof t1, 2));
      else snprintf(lab_late, sizeof lab_late, "%s\xE2\x80\x93%s ms", ms(t1, sizeof t1, m.k_ref + 1), ms(t2, sizeof t2, 2 * m.k_ref));
      snprintf(lab_stall, sizeof lab_stall, "\xE2\x89\xA5%s ms", ms(t1, sizeof t1, 2 * m.k_ref + 1));
      const float total = float(num.n10);
      snprintf(b1, sizeof b1, "%s ", lab_stall);
      snprintf(b2, sizeof b2, "%d%%", int(std::lround(num.stall / total * 100)));
      p.runs(PW - PAD, PACING_BASE, { {SANSM13, 13, FOOT, b1}, {MONO13, 13, num.stall ? RED : TEXT, b2} }, 'r');
      // Stacked share bar with labels inside the segments that can hold them.
      struct Seg { float share; ImU32 col; const char* label; } segs[] = {
         { num.on / total, BLUE, lab_on }, { num.late / total, AMBER, lab_late }, { num.stall / total, RED, lab_stall } };
      int live = 0;
      for (auto& s : segs) if (s.share > 0) live++;
      float avail = IW - (live - 1) * 2, x = PAD;
      for (auto& s : segs) {
         if (s.share <= 0) continue;
         float w = s.share * avail;
         p.rect(x, PACING_Y, w, PACING_H, s.col, 2);
         snprintf(b1, sizeof b1, "%d%%", int(std::lround(s.share * 100)));
         snprintf(b2, sizeof b2, "%s  %s", s.label, b1);
         const char* s_lab = w >= p.width(MONO13, 13, b2) + 10 ? b2 : w >= p.width(MONO13, 13, b1) + 8 ? b1 : nullptr;
         if (s_lab) p.text(x + w / 2, PACING_Y + PACING_H - 3.5f, MONO13, 13, PANEL_SOLID, s_lab, 'c');
         x += w + 2;
      }
   } else {
      p.text(PW - PAD, PACING_BASE, MONOM13, 13, FOOT, DASH, 'r');
   }

   // ---- frame (every tick) ----
   const float last_ms = n ? pacing.at(n - 1).ms : NAN;
   const float elapsed = n && now > data.last_present_time ? (now - data.last_present_time) / 1e6f : 0.f;
   const bool stalled = n && elapsed >= m.stall_ms;
   p.runs(PAD, LAST_BASE, { {SANS16, 16, LABEL, "LAST FRAME  "},
                            {MONO20, 20, std::isnan(last_ms) ? TEXT : ft_text_color(last_ms, m.target_ms), fmt1(b1, sizeof b1, last_ms)},
                            {MONO13, 13, DIM, " ms"} });
   if (stalled) {
      snprintf(b1, sizeof b1, "%.0f ms", elapsed);
      p.runs(PW - PAD, LAST_BASE, { {SANS16, 16, RED, "STALL  "}, {MONO20, 20, RED, b1} }, 'r');
   }

   // Frame-time chart: fixed columns filled from the right, the frame on screen in the last column.
   const int slots = FT_SHOW + 1, shown = int(std::min<uint32_t>(FT_SHOW, std::min(n, PacingRing::N - 1)));
   const float fbw = (IW - (slots - 1)) / slots, scale = 3 * m.target_ms;
   const int first = slots - 1 - shown;
   float ft_max = NAN, span_s = 0;
   struct Overflow { float x, ms; } overflow[FT_SHOW]; int n_overflow = 0;
   for (int j = 0; j < shown; j++) {
      const auto& fr = pacing.at(n - shown + j);
      float bh = std::max(2.f, std::min(1.f, fr.ms / scale) * CHART_H);
      float bx = PAD + (first + j) * (fbw + 1);
      p.rect(bx, FT_TOP + CHART_H - bh, fbw, bh, ft_bar_color(fr.ms, m.target_ms));
      if (fr.ms > scale * 1.001f) overflow[n_overflow++] = { bx, fr.ms };
      ft_max = std::isnan(ft_max) ? fr.ms : std::max(ft_max, fr.ms);
   }
   if (shown) {
      const auto& oldest = pacing.at(n - shown);
      span_s = (pacing.at(n - 1).end_ns - (oldest.end_ns - uint64_t(oldest.ms * 1e6))) / 1e9f;
   }
   for (int i = std::max(0, n_overflow - MAX_OVERFLOW_LABELS); i < n_overflow; i++) {
      snprintf(b1, sizeof b1, "%.0f", overflow[i].ms);
      const float size = 13, w = p.width(MONO13, size, b1) + 8, h = 17, top = FT_TOP + 2;
      float lx = overflow[i].x + fbw / 2 > PAD + IW / 2 ? overflow[i].x - 2 - w : overflow[i].x + fbw + 2;
      p.rect(lx, top, w, h, PANEL, 2);
      p.text(lx + w - 4, top + h - DESCENT * size, MONO13, size, RED, b1, 'r');
   }
   {  // the frame still on screen: outlined bar, inset 1 px so the border stays in its column
      const float x = PAD + (slots - 1) * (fbw + 1), h = std::max(2.f, std::min(1.f, elapsed / scale) * CHART_H);
      const ImU32 col = stalled ? RED : LABEL;
      const float w = std::max(0.5f, fbw - 2), hh = std::max(0.5f, h - 2);
      p.rect(x + 1, FT_TOP + CHART_H - h + 1, w, hh, with_alpha(col, stalled ? 0.55f : 0.3f));
      p.outline(x + 1, FT_TOP + CHART_H - h + 1, w, hh, col);
   }
   snprintf(b1, sizeof b1, " \xC2\xB7 %d FRAME%s \xC2\xB7 %.1f S", shown + 1, shown + 1 == 1 ? "" : "S", span_s);
   p.runs(PAD, FT_LABEL_BASE, { {SANS16, 16, LABEL, "FRAME TIME"}, {SANSM13, 13, FOOT, b1} });
   p.runs(PW - PAD, FT_LABEL_BASE, { {SANS16, 16, LABEL, "MAX "},
                                     {MONO16, 16, std::isnan(ft_max) ? TEXT : ft_text_color(ft_max, m.target_ms), fmt1(b2, sizeof b2, ft_max)},
                                     {MONO13, 13, DIM, " ms"} }, 'r');

   // Footer timecode m:ss.ff since the overlay started.
   auto cs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - HUDElements.overlay_start).count() / 10;
   snprintf(b1, sizeof b1, "%lld:%02lld.%02lld", (long long)(cs / 6000), (long long)(cs % 6000 / 100), (long long)(cs % 100));
   p.text(PW - PAD, FOOT_BASE, MONOM13, 13, LABEL, b1, 'r');

   ImGui::End();
}
