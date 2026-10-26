#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"

/**
 * A small, text-first web browser for Ink-boy.
 *
 * The activity owns all page text, link metadata, and history. No browser
 * state is static or global, so ActivityManager can release it when the user
 * leaves this screen. HTML is capped to keep peak heap use bounded.
 */
class WebBrowserActivity final : public Activity {
 public:
  explicit WebBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WebBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t { START, LOADING, VIEWING, LINKS, ERROR };

  // Stores extracted readable text, not the raw HTML response.
  static constexpr size_t MAX_HTML_BYTES = 8192;
  // Bound network parsing without allocating a response-sized buffer.
  static constexpr size_t MAX_HTML_INPUT_BYTES = 64 * 1024;
  static constexpr size_t TAG_BUFFER_SIZE = 160;
  static constexpr size_t MAX_LINKS = 16;
  static constexpr size_t MAX_LINK_URL = 192;
  static constexpr size_t MAX_LINK_LABEL = 64;
  static constexpr size_t HISTORY_CAPACITY = 6;
  static constexpr size_t VIEW_LINE_WIDTH = 76;
  static constexpr size_t VIEW_LINES = 15;

  struct Link {
    std::array<char, MAX_LINK_URL> href{};
    std::array<char, MAX_LINK_LABEL> label{};
  };

  // Incremental parser: HTML is consumed in HTTP chunks and only readable text
  // plus a small fixed set of links is retained in RAM.
  struct HtmlStreamParser {
    explicit HtmlStreamParser(WebBrowserActivity& owner);
    bool feed(const uint8_t* data, size_t length);
    void finish();

   private:
    void processTextChar(char value);
    void processTag();
    void emit(char value);
    void acceptDecodedText(char value);
    void finishEncodedMarkupCandidate(bool completeTag);
    void flushEntity();

    WebBrowserActivity& owner;
    size_t inputBytes = 0;
    size_t tagLength = 0;
    size_t entityLength = 0;
    size_t commentDashCount = 0;
    size_t encodedMarkupLength = 0;
    bool inTag = false;
    bool inComment = false;
    bool inEntity = false;
    bool inEncodedMarkupCandidate = false;
    bool skipEncodedMarkupUntilClose = false;
    bool textLimitReached = false;
    bool inputLimitReached = false;
    char tagQuote = '\0';
    // 1 = script, 2 = style, 3 = noscript, 4 = template, 5 = svg, 6 = iframe.
    int skippedTag = 0;
    int activeLink = -1;
    std::array<char, 10> entity{};
    std::array<char, TAG_BUFFER_SIZE> encodedMarkupCandidate{};
  };

  State state = State::START;
  std::array<char, MAX_HTML_BYTES + 1> html{};
  std::array<char, TAG_BUFFER_SIZE> parserTagBuffer{};
  std::array<Link, MAX_LINKS> links{};
  std::array<std::string, HISTORY_CAPACITY> history{};
  size_t htmlLength = 0;
  size_t linkCount = 0;
  size_t historyCount = 0;
  size_t scrollLine = 0;
  size_t selectedLink = 0;
  size_t firstVisibleLink = 0;
  std::string url;
  std::string pendingUrl;
  std::string statusMessage;
  bool pageLoaded = false;
  bool ownsWifi = false;
  bool fetchTruncated = false;

  void openUrlInput();
  void launchWifiSelection();
  void openTarget(const std::string& input, bool addHistory);
  void loadPage(const std::string& target);
  void goBack();
  void activateSelectedLink();
  void clearPage();
  void pushHistory(const std::string& previousUrl);
  size_t visualLineCount() const;
  size_t nextVisualLine(size_t start, size_t& end) const;
  std::string normalizeInput(const std::string& input) const;
  std::string resolveLink(const std::string& baseUrl, const std::string& href) const;
};
