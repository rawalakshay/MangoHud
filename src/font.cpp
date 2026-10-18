#include <cstdint>
#include "overlay.h"
#include "file_utils.h"
#include "font_default.h"
#include "IconsForkAwesome.h"
#include "forkawesome.h"
#include "font_plex.h"
#include "pacing.h"

void create_fonts(ImFontAtlas* font_atlas, const overlay_params& params, swapchain_stats& sw)
{
   ImFont*& small_font = sw.font_small;
   ImFont*& text_font = sw.font_text;
   ImFont*& secondary_font = sw.font_secondary;
   auto& io = ImGui::GetIO();
   if (!font_atlas)
        font_atlas = io.Fonts;
   font_atlas->Clear();

   ImGui::GetIO().FontGlobalScale = params.font_scale; // set here too so ImGui::CalcTextSize is correct
   float font_size = params.font_size;
   if (font_size < FLT_EPSILON)
      font_size = 24;

   float font_size_text = params.font_size_text;
   if (font_size_text < FLT_EPSILON)
      font_size_text = font_size;

   float font_size_secondary = params.font_size_secondary;
   if (font_size_secondary > font_size || font_size_secondary < FLT_EPSILON)
      font_size_secondary = font_size;

   static const ImWchar default_range[] =
   {
      0x0020, 0x00FF, // Basic Latin + Latin Supplement
      0x2018, 0x201F, // Bunch of quotation marks
      //0x0100, 0x017F, // Latin Extended-A
      //0x2103, 0x2103, // Degree Celsius
      //0x2109, 0x2109, // Degree Fahrenheit
      0,
   };
   // Load Icon file and merge to exisitng font
    ImFontConfig config;
    config.MergeMode = true;
    // ImGui changed OversampleH default to 2, but it appears to sometimes cause
    // crashing issues in 32bit applications.
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    static const ImWchar icon_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 };

   ImVector<ImWchar> glyph_ranges;
   ImFontGlyphRangesBuilder builder;
   builder.AddRanges(font_atlas->GetGlyphRangesDefault());
   if (params.font_glyph_ranges & FG_KOREAN)
      builder.AddRanges(font_atlas->GetGlyphRangesKorean());
   if (params.font_glyph_ranges & FG_CHINESE_FULL)
      builder.AddRanges(font_atlas->GetGlyphRangesChineseFull());
   if (params.font_glyph_ranges & FG_CHINESE_SIMPLIFIED)
      builder.AddRanges(font_atlas->GetGlyphRangesChineseSimplifiedCommon());
   if (params.font_glyph_ranges & FG_JAPANESE)
      builder.AddRanges(font_atlas->GetGlyphRangesJapanese()); // Not exactly Shift JIS compatible?
   if (params.font_glyph_ranges & FG_CYRILLIC)
      builder.AddRanges(font_atlas->GetGlyphRangesCyrillic());
   if (params.font_glyph_ranges & FG_THAI)
      builder.AddRanges(font_atlas->GetGlyphRangesThai());
   if (params.font_glyph_ranges & FG_VIETNAMESE)
      builder.AddRanges(font_atlas->GetGlyphRangesVietnamese());
   if (params.font_glyph_ranges & FG_LATIN_EXT_A) {
      constexpr ImWchar latin_ext_a[] { 0x0100, 0x017F, 0 };
      builder.AddRanges(latin_ext_a);
   }
   if (params.font_glyph_ranges & FG_LATIN_EXT_B) {
      constexpr ImWchar latin_ext_b[] { 0x0180, 0x024F, 0 };
      builder.AddRanges(latin_ext_b);
   }
   builder.BuildRanges(&glyph_ranges);

   bool same_font = (params.font_file == params.font_file_text || params.font_file_text.empty());
   bool text_same_size = (font_size == font_size_text);
   bool secondary_same_size = (font_size == font_size_secondary);

   // ImGui takes ownership of the data, no need to free it
   if (!params.font_file.empty() && file_exists(params.font_file)) {
      font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size, nullptr, same_font && text_same_size ? glyph_ranges.Data : default_range);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size * 0.55f, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size_secondary, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   } else {
      const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
      font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, nullptr, default_range);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55f, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size_secondary, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   }

   auto font_file_text = params.font_file_text;
   if (font_file_text.empty())
      font_file_text = params.font_file;

   if ((!same_font || !text_same_size) && file_exists(font_file_text))
      text_font = font_atlas->AddFontFromFileTTF(font_file_text.c_str(), font_size_text, nullptr, glyph_ranges.Data);
   else
      text_font = font_atlas->Fonts[0];

   if (params.enabled[OVERLAY_PARAM_ENABLED_pacing_panel]) {
      // Panel fonts are baked at the design size times the screen scale; render_pacing_panel
      // zeroes font_params_hash when the scale changes so this runs again.
      static const ImWchar plex_range[] = { 0x0020, 0x00FF, 0x2013, 0x2013 /* en dash */, 0x2026, 0x2026 /* ellipsis */, 0x2264, 0x2265 /* <= >= */, 0 };
      static const char* const plex_faces[] = { plex_mono_semibold_compressed_data_base85, plex_mono_medium_compressed_data_base85,
                                               plex_sans_cond_semibold_compressed_data_base85, plex_sans_cond_medium_compressed_data_base85 };
      ImFontConfig plex_cfg;
      plex_cfg.OversampleH = 1;
      plex_cfg.OversampleV = 1;
      float k = sw.pacing_k_wanted > 0 ? sw.pacing_k_wanted : 1.f;
      sw.pacing_k = k;
      for (int i = 0; i < PF_COUNT; i++)
         sw.pacing_fonts[i] = font_atlas->AddFontFromMemoryCompressedBase85TTF(plex_faces[pacing_font_specs[i].face], pacing_font_specs[i].px * k, &plex_cfg, plex_range);
   } else {
      for (auto& f : sw.pacing_fonts) f = nullptr;
   }

   font_atlas->Build();
}
