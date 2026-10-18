#pragma once

#include <Epub/Page.h>

#include <array>
#include <memory>

#include "FootnoteLinkTargets.h"
#include "activities/Activity.h"

// Select a footnote reference on the page itself. The page is owned by this
// activity so the source reader can remain paused until a jump or cancellation.
class EpubReaderFootnoteSelectActivity final : public Activity {
 public:
  EpubReaderFootnoteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                                   FootnoteLinkTargets targets, int fontId, int marginLeft, int marginTop)
      : Activity("EpubReaderFootnoteSelect", renderer, mappedInput),
        page(std::move(page)),
        targets(targets),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  static constexpr unsigned long REPEAT_START_MS = 500;
  static constexpr unsigned long REPEAT_INTERVAL_MS = 500;

  void cancel();
  void performJump();
  bool drawHighlightWithSnapshot();
  void drawChrome() const;

  std::unique_ptr<Page> page;
  FootnoteLinkTargets targets;
  const int fontId;
  const int marginLeft;
  const int marginTop;
  std::array<uint8_t, EPUB_MAX_FOOTNOTES_PER_PAGE> selectable{};
  uint8_t selectableCount = 0;
  uint8_t selected = 0;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
  unsigned long lastMoveTime = 0;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;
};
