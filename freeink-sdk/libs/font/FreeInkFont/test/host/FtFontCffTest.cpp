#include <FtFont.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <vector>

using freeink::font::FtFont;

namespace {
unsigned long readMemory(void* context, unsigned long offset, unsigned char* buffer, unsigned long count) {
  const auto& bytes = *static_cast<const std::vector<uint8_t>*>(context);
  if (offset > bytes.size() || count > bytes.size() - offset) return 0;
  if (count) std::memcpy(buffer, bytes.data() + offset, count);
  return count;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) return 2;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "OTTO", 4) != 0) return 2;

  FtFont::FaceInfo info;
  char family[128];
  if (FtFont::inspectStream(readMemory, &bytes, bytes.size(), info, family, sizeof(family)) !=
          FtFont::InspectResult::Ok ||
      !family[0]) {
    std::fprintf(stderr, "Cannot inspect CFF face: %s\n", argv[1]);
    return 1;
  }

  FtFont font;
  if (!font.init(bytes.data(), static_cast<uint32_t>(bytes.size()), 16) || !font.hasGlyph('A') ||
      !font.rasterize('A', 16)) {
    std::fprintf(stderr, "Cannot render CFF face: %s (init stage=%u, FT error=0x%X)\n", argv[1],
                 unsigned(font.lastInitFailure()), unsigned(font.lastInitError()));
    return 1;
  }
  FtFont streamed;
  if (!streamed.initStream(readMemory, &bytes, bytes.size(), 16) || !streamed.rasterize('A', 16)) {
    std::fprintf(stderr, "Cannot stream CFF face: %s\n", argv[1]);
    return 1;
  }
  for (const auto mode : {FtFont::HintingMode::None, FtFont::HintingMode::Auto, FtFont::HintingMode::Light,
                          FtFont::HintingMode::Native}) {
    FtFont::RenderOptions options;
    options.hinting = mode;
    if (!font.setRenderOptions(options) || !font.rasterize('A', 16)) {
      std::fprintf(stderr, "Cannot render CFF face with hinting mode %u: %s\n", unsigned(mode), argv[1]);
      return 1;
    }
    if (font.hasGlyph(0x00E9) && !font.rasterize(0x00E9, 16)) {
      std::fprintf(stderr, "Cannot render accented CFF glyph with hinting mode %u: %s\n", unsigned(mode), argv[1]);
      return 1;
    }
  }
  std::printf("CFF face OK: %s (%s, weight=%u, italic=%u)\n", argv[1], family, unsigned(info.weight),
              unsigned(info.italic));
  return 0;
}
