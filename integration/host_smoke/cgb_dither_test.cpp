/*
 * CgbDither.h must produce exactly the framebuffer the per-pixel Floyd-Steinberg
 * it replaced produced. That original function (drawCgbFloydSteinberg from
 * GameBoyActivity.cpp as of the commit before the speedups) is kept below,
 * verbatim, as the reference. Random and extreme frames, both orientations, all
 * three scales, on a framebuffer pre-filled with garbage so that any byte the
 * new code touches but the old did not (or vice versa) shows up.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace reference {
bool drawCgbFloydSteinberg(const uint8_t* source, uint8_t* framebuffer, const int gameX, const int gameY,
                           const int scale, const int panelBytes, const int panelHeight, const bool portrait) {
  constexpr int kSourceWidth = 160;
  constexpr int kSourceHeight = 144;
  constexpr int kMaxOutputWidth = kSourceWidth * 3;
  if (!source || !framebuffer || scale < 1 || scale > 3 || panelBytes <= 0 || panelHeight <= 0) return false;

  const int outputWidth = kSourceWidth * scale;
  const int outputHeight = kSourceHeight * scale;
  int errorA[kMaxOutputWidth + 2] = {};
  int errorB[kMaxOutputWidth + 2] = {};
  int* current = errorA;
  int* next = errorB;

  for (int py = 0; py < outputHeight; ++py) {
    const bool leftToRight = (py & 1) == 0;
    for (int i = 0; i < outputWidth; ++i) {
      const int px = leftToRight ? i : outputWidth - 1 - i;
      const uint8_t luminance = source[(py / scale) * kSourceWidth + (px / scale)];
      int adjusted = static_cast<int>(luminance) * 16 + current[px + 1];
      adjusted = std::clamp(adjusted, 0, 255 * 16);
      const bool white = adjusted >= 128 * 16;
      const int error = adjusted - (white ? 255 * 16 : 0);

      const int panelRow = portrait ? panelHeight - 1 - (gameX + px) : gameY + py;
      const int panelColumn = portrait ? gameY + py : gameX + px;
      uint8_t* destination = framebuffer + static_cast<size_t>(panelRow) * panelBytes + panelColumn / 8;
      const uint8_t mask = static_cast<uint8_t>(0x80U >> (panelColumn & 7));
      if (white) {
        *destination |= mask;
      } else {
        *destination &= static_cast<uint8_t>(~mask);
      }

      // Serpentine Floyd-Steinberg error distribution, in fixed-point units.
      if (leftToRight) {
        if (px + 1 < outputWidth) current[px + 2] += (error * 7) / 16;
        if (px > 0) next[px] += (error * 3) / 16;
        next[px + 1] += (error * 5) / 16;
        if (px + 1 < outputWidth) next[px + 2] += error / 16;
      } else {
        if (px > 0) current[px] += (error * 7) / 16;
        if (px + 1 < outputWidth) next[px + 2] += (error * 3) / 16;
        next[px + 1] += (error * 5) / 16;
        if (px > 0) next[px] += error / 16;
      }
    }
    std::swap(current, next);
    std::fill(next, next + outputWidth + 2, 0);
  }
  return true;
}
}  // namespace reference

#include "activities/gameboy/CgbDither.h"

static std::mt19937 rng(12345);

static void fill(std::vector<uint8_t>& f, int mode) {
  for (size_t i = 0; i < f.size(); ++i) {
    switch (mode) {
      case 0: f[i] = rng() & 255; break;
      case 1: f[i] = 0; break;
      case 2: f[i] = 255; break;
      case 3: f[i] = (i / 160 + i % 160) & 1 ? 255 : 0; break;
      case 4: f[i] = (i % 160) * 255 / 159; break;            // horizontal ramp
      case 5: f[i] = 127 + (rng() & 3); break;                // right at threshold
      case 6: f[i] = (rng() & 1) ? 255 : 0; break;            // worst-case error swings
      default: f[i] = ((i / 160) * 255 / 143); break;         // vertical ramp
    }
  }
}

int main() {
  int cases = 0, bad = 0;
  for (int portrait = 0; portrait < 2; ++portrait)
    for (int scale = 1; scale <= 3; ++scale)
      for (int mode = 0; mode < 8; ++mode)
        for (int rep = 0; rep < (mode == 0 ? 6 : 1); ++rep) {
          std::vector<uint8_t> src(160 * 144);
          fill(src, mode);
          const int gw = 160 * scale;
          // Panel 480x800 as on the X4 Pro: stride 60 bytes, 800 rows.
          const int panelBytes = 60, panelRows = 800;
          int gameX, gameY;
          if (portrait) { gameX = ((480 - gw) / 2); gameY = 40; if (gameX < 0) gameX = 0; }
          else { gameX = 160; gameY = 24; if (gameX + gw > 800) gameX = 0; }
          if (!portrait) gameX &= ~7;
          // Landscape logical space is 800 wide: stride must hold gameX+gw bits.
          const int strideBytes = portrait ? panelBytes : 100;
          const int rows = portrait ? panelRows : 480;
          std::vector<uint8_t> a(strideBytes * rows), b;
          for (auto& v : a) v = rng() & 255;   // garbage: untouched bytes must stay identical
          b = a;
          bool ok1 = reference::drawCgbFloydSteinberg(src.data(), a.data(), gameX, gameY, scale, strideBytes, rows, portrait != 0);
          bool ok2 = cgbdither::cgbDitherFrame(src.data(), b.data(), gameX, gameY, scale, strideBytes, rows, portrait != 0);
          ++cases;
          if (ok1 != ok2 || a != b) {
            ++bad;
            size_t first = 0; while (first < a.size() && a[first] == b[first]) ++first;
            printf("MISMATCH portrait=%d scale=%d mode=%d: first diff at byte %zu\n", portrait, scale, mode, first);
          }
        }
  // Rejected arguments must be rejected by both (new one is stricter on alignment only).
  std::vector<uint8_t> s(160 * 144), fb(60 * 800);
  if (cgbdither::cgbDitherFrame(nullptr, fb.data(), 0, 0, 1, 60, 800, true) ||
      cgbdither::cgbDitherFrame(s.data(), fb.data(), 0, 0, 4, 60, 800, true) ||
      cgbdither::cgbDitherFrame(s.data(), fb.data(), 0, 3, 1, 60, 800, true)) { puts("bad rejection"); ++bad; }
  printf("%d cases, %d mismatches\n", cases, bad);
  return bad != 0;
}
