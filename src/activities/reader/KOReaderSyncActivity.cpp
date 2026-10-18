#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "HalClock.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace {
constexpr int RESULT_NON_TOUCH_ACTION_HEIGHT = 40;
constexpr int RESULT_NON_TOUCH_ACTION_GAP = 8;
constexpr int RESULT_CARD_PADDING = 12;
constexpr int RESULT_CARD_ROW_GAP = 6;
constexpr int RESULT_CARD_BAR_HEIGHT = 10;
constexpr int RESULT_BADGE_PADDING_X = 6;

// Progress in tenths of a percent, matching the card's "%.1f%%" label so the "Ahead" badge never
// disagrees with the numbers shown on screen.
long displayedTenths(const float percentage) { return std::lround(percentage * 1000.0f); }

// Apply/Upload buttons are pinned above the button hints (or the bottom edge on touch devices) so the
// progress cards can use the space above them. Rendering and hit testing share this layout.
TouchActionButtons::Layout resultActionLayout(const Rect& screen, const ThemeMetrics& metrics, const bool hasTouch) {
  constexpr uint8_t buttonCount = 2;
  const int buttonHeight = hasTouch ? TouchActionButtons::kDefaultHeight : RESULT_NON_TOUCH_ACTION_HEIGHT;
  const int buttonGap = hasTouch ? TouchActionButtons::kDefaultGap : RESULT_NON_TOUCH_ACTION_GAP;
  const int reservedBottom = hasTouch ? metrics.verticalSpacing : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int totalHeight = buttonHeight * buttonCount + buttonGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding, screen.y + screen.height - reservedBottom - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount, buttonHeight, buttonGap);
}

struct ProgressCard {
  const char* title;
  const char* source;  // Optional, e.g. the remote device name.
  const char* pageText;
  const char* chapter;
  float percentage;  // 0.0 - 1.0
  bool ahead;
};

int progressCardHeight(const GfxRenderer& renderer) {
  return RESULT_CARD_PADDING * 2 + renderer.getLineHeight(UI_10_FONT_ID) + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP + RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_10_FONT_ID);
}

// Draws text so its baseline lines up with a larger font drawn at rowY.
void drawTextOnBaseline(const GfxRenderer& renderer, const int fontId, const int x, const int rowY, const int rowFontId,
                        const char* text, const bool black = true,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int y = rowY + renderer.getFontAscenderSize(rowFontId) - renderer.getFontAscenderSize(fontId);
  renderer.drawText(fontId, x, y, text, black, style);
}

// Card layout:
//   Title  source                  [Ahead]
//   42.5%                        Page 3/12
//   [=========                          ]
//   Chapter name...
void drawProgressCard(const GfxRenderer& renderer, const Rect& card, const ProgressCard& info) {
  renderer.drawRect(card.x, card.y, card.width, card.height, true);

  const int x = card.x + RESULT_CARD_PADDING;
  const int innerWidth = std::max(1, card.width - RESULT_CARD_PADDING * 2);
  const int right = x + innerWidth;
  const int smallGap = RESULT_CARD_ROW_GAP * 2;
  int y = card.y + RESULT_CARD_PADDING;

  // Row 1: title, optional source, optional "Ahead" badge.
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int titleRight = right;
  if (info.ahead) {
    const char* badge = tr(STR_SYNC_AHEAD);
    const int badgeWidth = renderer.getTextWidth(SMALL_FONT_ID, badge) + RESULT_BADGE_PADDING_X * 2;
    const int badgeX = right - badgeWidth;
    renderer.fillRect(badgeX, y, badgeWidth, titleLineHeight, true);
    const int badgeTextY = y + (titleLineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, badgeX + RESULT_BADGE_PADDING_X, badgeTextY, badge, false);
    titleRight = badgeX - smallGap;
  }
  const std::string title =
      renderer.truncatedText(UI_10_FONT_ID, info.title, std::max(1, titleRight - x), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (info.source != nullptr && info.source[0] != '\0') {
    const int sourceX = x + renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD) + smallGap;
    if (titleRight - sourceX > 0) {
      const std::string source = renderer.truncatedText(SMALL_FONT_ID, info.source, titleRight - sourceX);
      drawTextOnBaseline(renderer, SMALL_FONT_ID, sourceX, y, UI_10_FONT_ID, source.c_str());
    }
  }
  y += titleLineHeight + RESULT_CARD_ROW_GAP;

  // Row 2: large percentage with the page position right-aligned on the same baseline.
  char percentStr[16];
  snprintf(percentStr, sizeof(percentStr), "%.1f%%", info.percentage * 100);
  renderer.drawText(UI_12_FONT_ID, x, y, percentStr, true, EpdFontFamily::BOLD);
  const int percentRight = x + renderer.getTextWidth(UI_12_FONT_ID, percentStr, EpdFontFamily::BOLD) + smallGap;
  if (right - percentRight > 0) {
    const std::string page = renderer.truncatedText(UI_10_FONT_ID, info.pageText, right - percentRight);
    const int pageX = right - renderer.getTextWidth(UI_10_FONT_ID, page.c_str());
    drawTextOnBaseline(renderer, UI_10_FONT_ID, pageX, y, UI_12_FONT_ID, page.c_str());
  }
  y += renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP;

  // Row 3: progress bar.
  renderer.drawRect(x, y, innerWidth, RESULT_CARD_BAR_HEIGHT, true);
  const float clamped = std::min(1.0f, std::max(0.0f, info.percentage));
  const int fillWidth = static_cast<int>((innerWidth - 4) * clamped);
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, RESULT_CARD_BAR_HEIGHT - 4, true);
  }
  y += RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP;

  // Row 4: chapter name.
  const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, info.chapter, innerWidth);
  renderer.drawText(UI_10_FONT_ID, x, y, chapter.c_str());
}

TouchActionButtons::Layout noRemoteProgressActionLayout(const Rect& screen, const ThemeMetrics& metrics) {
  constexpr uint8_t buttonCount = 2;
  constexpr int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

void syncTimeWithNTP() {
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("KOSync", "NTP sync unavailable, using fallback");
  }
#endif
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
  }
}

bool KOReaderSyncActivity::ensureLocalProgressLoaded() {
  if (!localProgressDeferred) return localProgress.valid;

  ensureEpubLoaded();
  if (!epub) return false;

  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "KOSync")) {
    currentSpineIndex = progress.spineIndex;
    currentPage = progress.pageNumber;
    if (progress.hasPageCount) totalPagesInSpine = std::max(1, progress.pageCount);
  }

  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) currentSpineIndex = 0;
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  if (progress.hasVisibleTextOffset) {
    localPos.visibleTextOffset = progress.visibleTextOffset;
    localPos.hasVisibleTextOffset = true;
  }
  const PositionCoordinateSpace coordinateSpace = primaryMatchMethod == DocumentMatchMethod::FILENAME
                                                      ? PositionCoordinateSpace::SourceDocument
                                                      : PositionCoordinateSpace::CurrentDocument;
  localProgress = ProgressMapper::toKOReader(epub, localPos, coordinateSpace);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterName = tocIdx >= 0 ? epub->getTocItem(tocIdx).title : "";
  localProgressDeferred = false;
  return localProgress.valid;
}

void KOReaderSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("KOSync", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  const std::optional<uint32_t> visibleTextOffset =
      position.hasVisibleTextOffset ? std::optional<uint32_t>(position.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount, visibleTextOffset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, position.spineIndex, position.pageNumber, pageCount);
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath, false, false, true); }

bool KOReaderSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::performSync() {
  const DocumentMatchMethod primaryMethod = primaryMatchMethod;
  remoteMatchMethod = primaryMethod;
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Fetch progress screen could not be rendered synchronously; aborting sync");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Fetch remote progress. In smart mode, also probe the alternate document-id
  // method and use the furthest remote state we can find. This avoids a stale
  // local upload when another KOReader device synced the same book with a
  // different document matching method.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          remoteProgress.percentage, remoteProgress.progress.c_str());

  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = altHash;
        remoteProgress = std::move(altProgress);
        remoteMatchMethod = altMethod;
        result = KOReaderSyncClient::OK;
      }
    }
  }

  // A minimal network boot intentionally reaches this point without loading the EPUB.
  // Reconstruct local progress only after all remote TLS probes have completed.
  if (!ensureLocalProgressLoaded()) {
    LOG_ERR("KOSync", "Failed to reconstruct local progress after network boot");
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    }
    requestUpdate(true);
    return;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  hasRemoteProgress = true;

  const PositionCoordinateSpace remoteCoordinateSpace = remoteMatchMethod == DocumentMatchMethod::FILENAME
                                                            ? PositionCoordinateSpace::SourceDocument
                                                            : PositionCoordinateSpace::CurrentDocument;
  bool usedRichPosition = false;
  // The client only accepts rich positions from the official CrossPoint Sync server.
  // Filename matching still needs source-document mapping because optimized books can diverge.
  if (remoteCoordinateSpace == PositionCoordinateSpace::CurrentDocument && remoteProgress.position.has_value()) {
    const auto richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
    if (richMapped.has_value()) {
      remotePosition = *richMapped;
      usedRichPosition = true;
    }
  }
  if (!usedRichPosition) {
    const KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition =
        ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine, remoteCoordinateSpace);
  }
  if (!remotePosition.valid) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    }
    requestUpdate(true);
    return;
  }

  // Refine page using the content-offset LUT first, then structural anchors.
  // A partial cache deliberately returns no page for an offset outside its
  // watermark; preserving that offset lets the reader index through to it.
  if (!usedRichPosition && (remotePosition.hasVisibleTextOffset || remotePosition.hasLiIndex ||
                            remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex)) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasVisibleTextOffset) {
      const auto contentPage = tempSection.getPageForVisibleTextOffset(remotePosition.visibleTextOffset, true);
      if (contentPage.has_value()) {
        LOG_DBG("KOSync", "Visible offset %lu -> page %d (was %d)",
                static_cast<unsigned long>(remotePosition.visibleTextOffset), *contentPage, remotePosition.pageNumber);
        remotePosition.pageNumber = *contentPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Visible offset %lu is beyond the cached section watermark",
                static_cast<unsigned long>(remotePosition.visibleTextOffset));
      }
    }
    if (!refined && remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("KOSync", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("KOSync", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Keep the percentage-derived page inside the paragraph's cached page range.
          // A one-page paragraph should not allow byte-percentage drift to jump to later paragraphs.
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("KOSync", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d",
                remotePosition.paragraphIndex, *paragraphPage, nextParaBuf, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("KOSync", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }

  if (smartSyncEnabled()) {
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: doc=%s local=%.6f remote=%.6f delta=%.6f remoteXpath=%s mapped=%d/%d",
            documentHash.c_str(), localProgress.percentage, remoteProgress.percentage, delta,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }

    if (delta > 0) {
      // Alternate hashes are only probes for newer remote state. Keep uploads
      // on the user's configured matching method so its primary record heals.
      documentHash = primaryHash;
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition);
    return;
  }
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Upload progress screen could not be rendered synchronously; aborting upload");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (epub) {
    epub.reset();
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

  // Rich CrossPoint position for the default CrossPoint sync server (lossless
  // CrossPoint<->CrossPoint sync). The HTTP client also enforces this boundary
  // before serializing the extension.
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  wifiOff();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (smartSyncEnabled()) {
    markAutoReturn();
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // Sync is a reader-originated activity, but its decision prompts are not
  // reader content. Keep their touch actions available even when the reader's
  // tap controls are disabled.
  if (mappedInput.hasTouchHardware()) {
    mappedInput.setReaderTouchscreenOverride(true);
    touchOverrideActive = true;
  }

  // The reader uses this activity as a tiny handoff so ActivityManager can run
  // reader onExit() before rebooting. Network boot uses the other constructor.
  if (restartBeforeNetwork) {
    const bool hasReaderOrientation = readerOrientation < CrossPointSettings::ORIENTATION_COUNT;
    if (hasReaderOrientation) ReaderUtils::applyOrientation(renderer, readerOrientation);
    // Zero means no reader override; valid orientations are encoded one-based.
    const uint32_t orientationPayload = hasReaderOrientation ? static_cast<uint32_t>(readerOrientation) + 1 : 0;
    silentRestartToNetwork(NetworkBootTarget::KOREADER_SYNC, orientationPayload);
    return;
  }

  LOG_INF("KOSync", "network entry free=%u maxAlloc=%u stack=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  uint8_t syncOrientation =
      readerOrientation < CrossPointSettings::ORIENTATION_COUNT ? readerOrientation : SETTINGS.orientation;
  const PendingOverlayResume& resume = APP_STATE.pendingOverlayResume;
  if (resume.origin == PendingOverlayOrigin::Reader && resume.overlay == PendingOverlayType::FrontlightDrawer &&
      resume.preserveReaderOrientation && resume.readerOrientation < CrossPointSettings::ORIENTATION_COUNT) {
    syncOrientation = resume.readerOrientation;
  }
  ReaderUtils::applyOrientation(renderer, syncOrientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (!localProgressDeferred && !localProgress.valid) {
    LOG_ERR("KOSync", "Source position map unavailable; re-optimize the EPUB before filename-based sync");
    state = SYNC_FAILED;
    statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    requestUpdate();
    return;
  }

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_INF("KOSync", "launch WiFi selection free=%u maxAlloc=%u stack=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  if (touchOverrideActive) {
    mappedInput.setReaderTouchscreenOverride(false);
    touchOverrideActive = false;
  }
  Activity::onExit();

  if (wifiActivated) {
    wifiOff();
    silentRestartToReader(true);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_KOREADER_SYNC), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_KOREADER_SYNC));
  }

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SHOWING_RESULT) {
    const bool hasTouch = mappedInput.hasTouchHardware();
    top = screen.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);
    top += renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char remotePageStr[48];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_SYNC_PAGE_FORMAT), remotePosition.pageNumber + 1);
    char localPageStr[48];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_SYNC_PAGE_TOTAL_FORMAT), currentPage + 1, totalPagesInSpine);

    const long remoteTenths = displayedTenths(remoteProgress.percentage);
    const long localTenths = displayedTenths(localProgress.percentage);
    const ProgressCard cards[] = {
        {tr(STR_SYNC_REMOTE_TITLE), remoteProgress.device.c_str(), remotePageStr, remoteChapter.c_str(),
         remoteProgress.percentage, remoteTenths > localTenths},
        {tr(STR_SYNC_LOCAL_TITLE), nullptr, localPageStr, localChapter.c_str(), localProgress.percentage,
         localTenths > remoteTenths},
    };

    // Stack the cards in portrait; place them side by side in landscape where height is scarce.
    const int contentX = screen.x + metrics.contentSidePadding;
    const int contentWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
    const int cardHeight = progressCardHeight(renderer);
    const int cardGap = metrics.verticalSpacing;
    if (screen.width > screen.height) {
      const int cardWidth = std::max(1, (contentWidth - cardGap) / 2);
      drawProgressCard(renderer, Rect{contentX, top, cardWidth, cardHeight}, cards[0]);
      drawProgressCard(renderer, Rect{contentX + contentWidth - cardWidth, top, cardWidth, cardHeight}, cards[1]);
    } else {
      drawProgressCard(renderer, Rect{contentX, top, contentWidth, cardHeight}, cards[0]);
      drawProgressCard(renderer, Rect{contentX, top + cardHeight + cardGap, contentWidth, cardHeight}, cards[1]);
    }

    const auto actions = resultActionLayout(screen, metrics, hasTouch);
    const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL)};
    TouchActionButtons::draw(renderer, actions, actionLabels, selectedOption, selectedOption, UI_10_FONT_ID);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP),
                                              tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    if (mappedInput.hasTouch()) {
      const auto actions = noRemoteProgressActionLayout(screen, metrics);
      const char* actionLabels[] = {tr(STR_UPLOAD), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
    }

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SYNC_FAILED) {
    const Rect textArea{screen.x + metrics.contentSidePadding, screen.y, screen.width - metrics.contentSidePadding * 2,
                        screen.height};
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), 2, true,
                                     EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top + 40, statusMessage.c_str(), 3, true,
                                     EpdFontFamily::REGULAR, 4);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    returnToReader();
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        performUpload();
      }
    };

    {
      const auto actions = resultActionLayout(screen, metrics, mappedInput.hasTouchHardware());
      const Rect& first = actions.buttons[0];
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, first.y, actions.buttons[1].y - first.y, actions.count,
                                              first.x, first.x + first.width, first.height);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        chooseSelected();
        return;
      }
    }

    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.hasTouch()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const auto actions = noRemoteProgressActionLayout(screen, metrics);
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(
          touchedOption, actions.buttons[0].y, TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
          actions.count, actions.buttons[0].x, actions.buttons[0].x + actions.buttons[0].width,
          actions.buttons[0].height);
      if (touch == MappedInputManager::RowTouch::Down) return;
      if (touch == MappedInputManager::RowTouch::Tap) {
        if (touchedOption == 0) {
          if (documentHash.empty()) {
            documentHash = calculateDocumentHashForMethod(epubPath, primaryMatchMethod);
          }
          performUpload();
        } else if (touchedOption == 1) {
          returnToReader();
        }
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        documentHash = calculateDocumentHashForMethod(epubPath, primaryMatchMethod);
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
