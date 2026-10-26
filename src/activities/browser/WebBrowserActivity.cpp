#include "WebBrowserActivity.h"

#include <Arduino.h>
#include <FreeInkApp.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
bool startsWithIgnoreCase(const char* text, const char* prefix) {
  while (*prefix && *text) {
    if (std::tolower(static_cast<unsigned char>(*text)) !=
        std::tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
    ++text;
    ++prefix;
  }
  return *prefix == '\0';
}

bool equalsIgnoreCase(const char* left, const char* right) {
  while (*left && *right) {
    if (std::tolower(static_cast<unsigned char>(*left)) !=
        std::tolower(static_cast<unsigned char>(*right))) {
      return false;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

void copyText(char* destination, size_t capacity, const char* source) {
  if (capacity == 0) return;
  size_t length = source ? std::strlen(source) : 0;
  if (length >= capacity) length = capacity - 1;
  if (length > 0) std::memcpy(destination, source, length);
  destination[length] = '\0';
}

bool isBlockTag(const char* name) {
  return equalsIgnoreCase(name, "p") || equalsIgnoreCase(name, "div") ||
         equalsIgnoreCase(name, "li") || equalsIgnoreCase(name, "ul") ||
         equalsIgnoreCase(name, "ol") || equalsIgnoreCase(name, "dl") ||
         equalsIgnoreCase(name, "dt") || equalsIgnoreCase(name, "dd") ||
         equalsIgnoreCase(name, "h1") || equalsIgnoreCase(name, "h2") ||
         equalsIgnoreCase(name, "h3") || equalsIgnoreCase(name, "h4") ||
         equalsIgnoreCase(name, "h5") || equalsIgnoreCase(name, "h6") ||
         equalsIgnoreCase(name, "tr") || equalsIgnoreCase(name, "table") ||
         equalsIgnoreCase(name, "section") || equalsIgnoreCase(name, "article") ||
         equalsIgnoreCase(name, "main") || equalsIgnoreCase(name, "header") ||
         equalsIgnoreCase(name, "footer") || equalsIgnoreCase(name, "blockquote") ||
         equalsIgnoreCase(name, "pre") || equalsIgnoreCase(name, "br") ||
         equalsIgnoreCase(name, "hr");
}

void extractTagName(char* tag, char* name, size_t nameCapacity, bool& closing) {
  char* cursor = tag;
  while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  closing = *cursor == '/';
  if (closing) ++cursor;
  size_t length = 0;
  while (*cursor && !std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != '/' && *cursor != '>') {
    if (length + 1 < nameCapacity) {
      name[length++] = *cursor;
    }
    ++cursor;
  }
  name[length] = '\0';
}

void extractAttribute(const char* tag, const char* wantedName, char* value, size_t valueCapacity) {
  if (valueCapacity == 0) return;
  value[0] = '\0';
  const size_t wantedLength = std::strlen(wantedName);
  const char* cursor = tag;
  while (*cursor) {
    while (*cursor && (std::isspace(static_cast<unsigned char>(*cursor)) || *cursor == '/' || *cursor == '<')) ++cursor;
    if (!*cursor || *cursor == '>') break;
    const char* keyStart = cursor;
    while (*cursor && (std::isalnum(static_cast<unsigned char>(*cursor)) || *cursor == '-' || *cursor == '_' || *cursor == ':')) ++cursor;
    const size_t keyLength = static_cast<size_t>(cursor - keyStart);
    const char* keyEnd = cursor;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (*cursor != '=') {
      cursor = keyEnd;
      while (*cursor && !std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != '>') ++cursor;
      continue;
    }
    ++cursor;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    char quote = '\0';
    if (*cursor == '\'' || *cursor == '"') quote = *cursor++;
    const char* valueStart = cursor;
    while (*cursor && (quote ? *cursor != quote :
                       (!std::isspace(static_cast<unsigned char>(*cursor)) && *cursor != '>'))) {
      ++cursor;
    }
    const size_t valueLength = static_cast<size_t>(cursor - valueStart);
    if (quote && *cursor == quote) ++cursor;
    bool matches = keyLength == wantedLength;
    for (size_t i = 0; matches && i < wantedLength; ++i) {
      matches = std::tolower(static_cast<unsigned char>(keyStart[i])) ==
                std::tolower(static_cast<unsigned char>(wantedName[i]));
    }
    if (matches) {
      const size_t length = std::min(valueLength, valueCapacity - 1);
      std::memcpy(value, valueStart, length);
      value[length] = '\0';
      return;
    }
  }
}

void extractHref(const char* tag, char* href, size_t hrefCapacity) {
  extractAttribute(tag, "href", href, hrefCapacity);
}

bool decodeEntity(const char* entity, size_t length, char& output) {
  if (length == 3 && std::memcmp(entity, "amp", 3) == 0) output = '&';
  else if (length == 2 && std::memcmp(entity, "lt", 2) == 0) output = '<';
  else if (length == 2 && std::memcmp(entity, "gt", 2) == 0) output = '>';
  else if (length == 4 && std::memcmp(entity, "quot", 4) == 0) output = '"';
  else if (length == 4 && std::memcmp(entity, "nbsp", 4) == 0) output = ' ';
  else if (length == 4 && std::memcmp(entity, "apos", 4) == 0) output = '\'';
  else if (length == 3 && std::memcmp(entity, "#39", 3) == 0) output = '\'';
  else return false;
  return true;
}

std::string urlEncode(const std::string& input) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(std::min<size_t>(input.size() * 3, 720));
  for (const unsigned char ch : input) {
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      output.push_back(static_cast<char>(ch));
    } else {
      output.push_back('%');
      output.push_back(HEX_DIGITS[ch >> 4]);
      output.push_back(HEX_DIGITS[ch & 0x0F]);
    }
    if (output.size() >= 720) break;
  }
  return output;
}

bool isConnectedToNetwork() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}
}  // namespace

WebBrowserActivity::HtmlStreamParser::HtmlStreamParser(WebBrowserActivity& activity) : owner(activity) {
  owner.parserTagBuffer.fill('\0');
}

void WebBrowserActivity::HtmlStreamParser::emit(char value) {
  if (value == '\r' || value == '\0') return;
  if (value == '\t') value = ' ';
  if (value == ' ' &&
      (owner.htmlLength == 0 || owner.html[owner.htmlLength - 1] == ' ' || owner.html[owner.htmlLength - 1] == '\n')) {
    return;
  }
  if (value == '\n') {
    while (owner.htmlLength > 0 && owner.html[owner.htmlLength - 1] == ' ') {
      owner.html[--owner.htmlLength] = '\0';
    }
    if (owner.htmlLength == 0 || owner.html[owner.htmlLength - 1] == '\n') return;
  }
  if (owner.htmlLength >= MAX_HTML_BYTES) {
    owner.fetchTruncated = true;
    textLimitReached = true;
    return;
  }

  owner.html[owner.htmlLength++] = value;
  owner.html[owner.htmlLength] = '\0';
  if (activeLink >= 0 && value != '\n') {
    auto& label = owner.links[static_cast<size_t>(activeLink)].label;
    const size_t length = std::strlen(label.data());
    if (length + 1 < label.size()) {
      label[length] = value;
      label[length + 1] = '\0';
    }
  }
  // Stop at a clear, fixed memory ceiling. The response callback treats this
  // intentional abort as a truncated page, rather than a transport failure.
  if (owner.htmlLength >= MAX_HTML_BYTES) {
    owner.fetchTruncated = true;
    textLimitReached = true;
  }
}

void WebBrowserActivity::HtmlStreamParser::flushEntity() {
  if (!inEntity) return;
  acceptDecodedText('&');
  for (size_t i = 0; i < entityLength; ++i) acceptDecodedText(entity[i]);
  entity.fill('\0');
  entityLength = 0;
  inEntity = false;
}

void WebBrowserActivity::HtmlStreamParser::acceptDecodedText(const char value) {
  if (skipEncodedMarkupUntilClose) {
    if (value == '>') skipEncodedMarkupUntilClose = false;
    return;
  }
  if (!inEncodedMarkupCandidate) {
    if (value == '<') {
      inEncodedMarkupCandidate = true;
      encodedMarkupLength = 1;
      encodedMarkupCandidate.fill('\0');
      encodedMarkupCandidate[0] = '<';
      return;
    }
    emit(value);
    return;
  }

  // Some lightweight search pages expose escaped snippets such as
  // &lt;meta name="description" content="..."&gt;. Buffer that text so it
  // doesn't appear as raw markup in the reader. Ordinary comparisons like
  // "x < 3" are emitted unchanged when the candidate stops looking tag-like.
  if (value == '\n' || value == '\r') {
    finishEncodedMarkupCandidate(false);
    acceptDecodedText(value);
    return;
  }
  if (encodedMarkupLength + 1 >= encodedMarkupCandidate.size()) {
    // The prefix is already tag-shaped; discard the rest through its closing
    // angle bracket instead of leaking a long encoded tag into page text.
    inEncodedMarkupCandidate = false;
    encodedMarkupLength = 0;
    encodedMarkupCandidate.fill('\0');
    skipEncodedMarkupUntilClose = true;
    return;
  }
  if (encodedMarkupLength == 1 &&
      !(std::isalpha(static_cast<unsigned char>(value)) || value == '/' || value == '!' || value == '?')) {
    finishEncodedMarkupCandidate(false);
    acceptDecodedText(value);
    return;
  }

  encodedMarkupCandidate[encodedMarkupLength++] = value;
  encodedMarkupCandidate[encodedMarkupLength] = '\0';
  if (value == '>') finishEncodedMarkupCandidate(true);
}

void WebBrowserActivity::HtmlStreamParser::finishEncodedMarkupCandidate(const bool completeTag) {
  if (!inEncodedMarkupCandidate) return;
  inEncodedMarkupCandidate = false;

  if (completeTag && encodedMarkupLength >= 7) {
    char tagName[24]{};
    bool closing = false;
    extractTagName(encodedMarkupCandidate.data() + 1, tagName, sizeof(tagName), closing);
    // Keep a meta description as readable prose while removing the literal
    // encoded tag around it. Other entity-encoded markup is suppressed.
    if (!closing && equalsIgnoreCase(tagName, "meta")) {
      char content[TAG_BUFFER_SIZE]{};
      extractAttribute(encodedMarkupCandidate.data() + 1, "content", content, sizeof(content));
      if (content[0] != '\0') {
        emit('\n');
        for (const char* cursor = content; *cursor; ++cursor) emit(*cursor);
        emit('\n');
      }
    }
  } else if (!completeTag) {
    for (size_t i = 0; i < encodedMarkupLength; ++i) emit(encodedMarkupCandidate[i]);
  }

  encodedMarkupCandidate.fill('\0');
  encodedMarkupLength = 0;
}

void WebBrowserActivity::HtmlStreamParser::processTextChar(char value) {
  if (inEntity) {
    if (value == ';') {
      char decoded = '\0';
      if (decodeEntity(entity.data(), entityLength, decoded)) {
        acceptDecodedText(decoded);
      } else {
        acceptDecodedText('&');
        for (size_t i = 0; i < entityLength; ++i) acceptDecodedText(entity[i]);
        acceptDecodedText(';');
      }
      entity.fill('\0');
      entityLength = 0;
      inEntity = false;
      return;
    }
    if (value == '<' || std::isspace(static_cast<unsigned char>(value)) || entityLength + 1 >= entity.size()) {
      flushEntity();
      processTextChar(value);
      return;
    }
    entity[entityLength++] = value;
    return;
  }

  if (value == '&') {
    inEntity = true;
    entityLength = 0;
    entity.fill('\0');
    return;
  }
  if (value == '<') {
    inTag = true;
    tagLength = 0;
    tagQuote = '\0';
    owner.parserTagBuffer.fill('\0');
    return;
  }
  acceptDecodedText(value);
}

void WebBrowserActivity::HtmlStreamParser::processTag() {
  auto& tagBuffer = owner.parserTagBuffer;
  tagBuffer[std::min(tagLength, tagBuffer.size() - 1)] = '\0';
  char tagName[24]{};
  bool closing = false;
  extractTagName(tagBuffer.data(), tagName, sizeof(tagName), closing);

  if (skippedTag != 0) {
    const bool closesSkippedTag =
        (skippedTag == 1 && equalsIgnoreCase(tagName, "script")) ||
        (skippedTag == 2 && equalsIgnoreCase(tagName, "style")) ||
        (skippedTag == 3 && equalsIgnoreCase(tagName, "noscript")) ||
        (skippedTag == 4 && equalsIgnoreCase(tagName, "template")) ||
        (skippedTag == 5 && equalsIgnoreCase(tagName, "svg")) ||
        (skippedTag == 6 && equalsIgnoreCase(tagName, "iframe"));
    if (closing && closesSkippedTag) skippedTag = 0;
    return;
  }

  if (!closing && (equalsIgnoreCase(tagName, "script") || equalsIgnoreCase(tagName, "style") ||
                   equalsIgnoreCase(tagName, "noscript") || equalsIgnoreCase(tagName, "template") ||
                   equalsIgnoreCase(tagName, "svg") || equalsIgnoreCase(tagName, "iframe"))) {
    if (equalsIgnoreCase(tagName, "script")) skippedTag = 1;
    else if (equalsIgnoreCase(tagName, "style")) skippedTag = 2;
    else if (equalsIgnoreCase(tagName, "noscript")) skippedTag = 3;
    else if (equalsIgnoreCase(tagName, "template")) skippedTag = 4;
    else if (equalsIgnoreCase(tagName, "svg")) skippedTag = 5;
    else skippedTag = 6;
    activeLink = -1;
    return;
  }

  if (!closing && equalsIgnoreCase(tagName, "a")) {
    if (owner.linkCount < MAX_LINKS) {
      char href[MAX_LINK_URL]{};
      extractHref(tagBuffer.data(), href, sizeof(href));
      if (href[0] != '\0') {
        activeLink = static_cast<int>(owner.linkCount++);
        auto& link = owner.links[static_cast<size_t>(activeLink)];
        copyText(link.href.data(), link.href.size(), href);
      } else {
        activeLink = -1;
      }
    } else {
      activeLink = -1;
    }
  } else if (closing && equalsIgnoreCase(tagName, "a")) {
    if (activeLink >= 0) {
      auto& label = owner.links[static_cast<size_t>(activeLink)].label;
      if (label[0] == '\0') copyText(label.data(), label.size(), owner.links[static_cast<size_t>(activeLink)].href.data());
    }
    activeLink = -1;
  }

  if (isBlockTag(tagName)) emit('\n');
}

bool WebBrowserActivity::HtmlStreamParser::feed(const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (textLimitReached) return false;
    if (inputBytes >= MAX_HTML_INPUT_BYTES) {
      inputLimitReached = true;
      owner.fetchTruncated = true;
      return false;
    }
    ++inputBytes;
    const char value = static_cast<char>(data[i]);

    if (inComment) {
      if (value == '-') {
        commentDashCount = std::min<size_t>(commentDashCount + 1, 2);
      } else if (value == '>' && commentDashCount >= 2) {
        inComment = false;
        commentDashCount = 0;
      } else {
        commentDashCount = 0;
      }
      continue;
    }

    if (inTag) {
      if (tagLength + 1 < owner.parserTagBuffer.size()) {
        owner.parserTagBuffer[tagLength++] = value;
        owner.parserTagBuffer[tagLength] = '\0';
      }
      if (tagLength == 3 && owner.parserTagBuffer[0] == '!' && owner.parserTagBuffer[1] == '-' &&
          owner.parserTagBuffer[2] == '-') {
        inTag = false;
        inComment = true;
        tagLength = 0;
        commentDashCount = 0;
        continue;
      }
      if (value == '\'' || value == '"') {
        if (tagQuote == '\0') tagQuote = value;
        else if (tagQuote == value) tagQuote = '\0';
      } else if (value == '>' && tagQuote == '\0') {
        inTag = false;
        processTag();
      }
      continue;
    }

    if (skippedTag != 0) {
      if (value == '<') {
        inTag = true;
        tagLength = 0;
        tagQuote = '\0';
        owner.parserTagBuffer.fill('\0');
      }
      continue;
    }

    processTextChar(value);
  }
  return !textLimitReached && !inputLimitReached;
}

void WebBrowserActivity::HtmlStreamParser::finish() {
  if (!textLimitReached && !inputLimitReached) flushEntity();
  if (inEncodedMarkupCandidate) finishEncodedMarkupCandidate(false);
  while (owner.htmlLength > 0 &&
         (owner.html[owner.htmlLength - 1] == ' ' || owner.html[owner.htmlLength - 1] == '\n')) {
    owner.html[--owner.htmlLength] = '\0';
  }
  for (size_t i = 0; i < owner.linkCount; ++i) {
    if (owner.links[i].label[0] == '\0') {
      copyText(owner.links[i].label.data(), owner.links[i].label.size(), owner.links[i].href.data());
    }
  }
}

void WebBrowserActivity::onEnter() {
  Activity::onEnter();
  state = State::START;
  url.clear();
  pendingUrl.clear();
  statusMessage.clear();
  historyCount = 0;
  pageLoaded = false;
  ownsWifi = false;
  fetchTruncated = false;
  clearPage();
  requestUpdate();
}

void WebBrowserActivity::onExit() {
  Activity::onExit();
  clearPage();
  for (auto& entry : history) std::string().swap(entry);
  historyCount = 0;
  std::string().swap(url);
  std::string().swap(pendingUrl);
  std::string().swap(statusMessage);
#ifndef SIMULATOR
  if (ownsWifi) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
#endif
}

bool WebBrowserActivity::preventAutoSleep() { return state == State::LOADING; }

void WebBrowserActivity::loop() {
  if (state == State::START) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      openUrlInput();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state == State::LOADING) return;

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!url.empty()) openTarget(url, false);
      else openUrlInput();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      openUrlInput();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == State::LINKS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (selectedLink > 0) --selectedLink;
      if (selectedLink < firstVisibleLink) firstVisibleLink = selectedLink;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (selectedLink + 1 < linkCount) ++selectedLink;
      if (selectedLink >= firstVisibleLink + VIEW_LINES) firstVisibleLink = selectedLink - VIEW_LINES + 1;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelectedLink();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      openUrlInput();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
               mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      state = State::VIEWING;
      requestUpdate();
    }
    return;
  }

  if (state == State::VIEWING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (scrollLine > 0) --scrollLine;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      const size_t total = visualLineCount();
      const size_t maxScroll = total > VIEW_LINES ? total - VIEW_LINES : 0;
      if (scrollLine < maxScroll) ++scrollLine;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (linkCount > 0) {
        state = State::LINKS;
        selectedLink = 0;
        firstVisibleLink = 0;
        requestUpdate();
      } else {
        openUrlInput();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (linkCount > 0) {
        state = State::LINKS;
        selectedLink = 0;
        firstVisibleLink = 0;
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      openUrlInput();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      goBack();
    }
  }
}

void WebBrowserActivity::openUrlInput() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BROWSER_URL), url, 240,
                                                           InputType::Url, 1);
  if (!keyboard) {
    statusMessage = tr(STR_MEMORY_ERROR);
    state = pageLoaded ? State::VIEWING : State::START;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto* entered = std::get_if<KeyboardResult>(&result.data);
    if (!entered || entered->text.empty()) {
      requestUpdate();
      return;
    }
    openTarget(entered->text, pageLoaded);
  });
}

void WebBrowserActivity::launchWifiSelection() {
  ownsWifi = ownsWifi || WiFi.getMode() == WIFI_MODE_NULL;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    state = pageLoaded ? State::VIEWING : State::ERROR;
    statusMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled || pendingUrl.empty()) {
      pendingUrl.clear();
      state = pageLoaded ? State::VIEWING : State::START;
      statusMessage.clear();
      requestUpdate();
      return;
    }

    // The Wi-Fi activity can report association before DHCP finishes. Give
    // the station a short window to obtain an address before starting HTTP.
    const uint32_t startMs = millis();
    while (!isConnectedToNetwork() && millis() - startMs < 10000UL) {
      delay(100);
    }
    if (isConnectedToNetwork()) {
      const std::string target = pendingUrl;
      pendingUrl.clear();
      loadPage(target);
      return;
    }

    pendingUrl.clear();
    state = State::ERROR;
    statusMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  });
}

void WebBrowserActivity::openTarget(const std::string& input, const bool addHistory) {
  const std::string target = normalizeInput(input);
  if (target.empty()) {
    state = pageLoaded ? State::VIEWING : State::ERROR;
    statusMessage = tr(STR_BROWSER_FETCH_FAILED);
    requestUpdate();
    return;
  }
  if (addHistory && pageLoaded && !url.empty() && target != url) pushHistory(url);
#ifndef SIMULATOR
  if (!isConnectedToNetwork()) {
    pendingUrl = target;
    state = State::LOADING;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdateAndWait();
    launchWifiSelection();
    return;
  }
#endif
  loadPage(target);
}

void WebBrowserActivity::loadPage(const std::string& target) {
  url = target;
  state = State::LOADING;
  statusMessage = tr(STR_BROWSER_LOADING);
  requestUpdateAndWait();
  clearPage();

#ifdef SIMULATOR
  const char* demo = "Ink-boy Web Browser\n\nThis is a text-first browser preview. Connect the X4 Pro to Wi-Fi to load real pages.\n\nUse URL to enter a web address or search term.";
  const size_t demoLength = std::min(std::strlen(demo), MAX_HTML_BYTES);
  std::memcpy(html.data(), demo, demoLength);
  htmlLength = demoLength;
  pageLoaded = true;
  state = State::VIEWING;
  statusMessage.clear();
  requestUpdate();
  return;
#else
  HtmlStreamParser parser(*this);
  HttpDownloader::DownloadOptions downloadOptions;
  // Ink-boy's OPDS browser uses wolfSSL explicitly; use the same transport
  // here rather than HttpDownloader::fetchUrl(), which defaults to ESP_HTTP.
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  const auto fetchResult = HttpDownloader::streamUrl(
      url,
      [&parser](const uint8_t* data, const size_t length) { return parser.feed(data, length); }, nullptr, "", "",
      std::move(downloadOptions));
  parser.finish();
  const bool fetchOk = fetchResult == HttpDownloader::OK;

  if (!fetchOk && !fetchTruncated) {
    LOG_ERR("WEB", "HTTP fetch failed: result=%d", static_cast<int>(fetchResult));
    state = State::ERROR;
    statusMessage = tr(STR_BROWSER_FETCH_FAILED);
    pageLoaded = false;
    requestUpdate();
    return;
  }
  if (htmlLength == 0) {
    state = State::ERROR;
    statusMessage = tr(STR_BROWSER_NO_TEXT);
    pageLoaded = false;
    requestUpdate();
    return;
  }

  state = State::VIEWING;
  pageLoaded = true;
  scrollLine = 0;
  statusMessage = fetchTruncated ? tr(STR_BROWSER_TRUNCATED) : "";
  requestUpdate();
#endif
}

void WebBrowserActivity::goBack() {
  if (state == State::LINKS) {
    state = State::VIEWING;
    requestUpdate();
    return;
  }
  if (historyCount > 0) {
    std::string previous = history[--historyCount];
    history[historyCount].clear();
    loadPage(previous);
    return;
  }
  finishAfterBackPress();
}

void WebBrowserActivity::activateSelectedLink() {
  if (selectedLink >= linkCount) return;
  const std::string href = links[selectedLink].href.data();
  const std::string target = resolveLink(url, href);
  if (target.empty()) {
    statusMessage = tr(STR_BROWSER_FETCH_FAILED);
    requestUpdate();
    return;
  }
  openTarget(target, true);
}

void WebBrowserActivity::pushHistory(const std::string& previousUrl) {
  if (historyCount == HISTORY_CAPACITY) {
    for (size_t i = 1; i < HISTORY_CAPACITY; ++i) history[i - 1] = std::move(history[i]);
    history[HISTORY_CAPACITY - 1].clear();
    historyCount = HISTORY_CAPACITY - 1;
  }
  history[historyCount++] = previousUrl;
}

void WebBrowserActivity::clearPage() {
  html.fill('\0');
  for (auto& link : links) link = Link{};
  htmlLength = 0;
  linkCount = 0;
  scrollLine = 0;
  selectedLink = 0;
  firstVisibleLink = 0;
  fetchTruncated = false;
}

std::string WebBrowserActivity::normalizeInput(const std::string& rawInput) const {
  size_t first = 0;
  size_t last = rawInput.size();
  while (first < last && std::isspace(static_cast<unsigned char>(rawInput[first]))) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(rawInput[last - 1]))) --last;
  if (first == last) return {};
  std::string input = rawInput.substr(first, last - first);
  const bool hasHttpScheme = startsWithIgnoreCase(input.c_str(), "http://");
  const bool hasHttpsScheme = startsWithIgnoreCase(input.c_str(), "https://");
  const bool hasScheme = hasHttpScheme || hasHttpsScheme;
  if (!hasScheme && input.find("://") != std::string::npos) return {};
  const bool looksLikeHost = input.find('.') != std::string::npos && input.find(' ') == std::string::npos;
  if (!hasScheme && !looksLikeHost) {
    return "https://lite.duckduckgo.com/lite/?q=" + urlEncode(input);
  }
  if (!hasScheme) input.insert(0, "https://");
  if (startsWithIgnoreCase(input.c_str(), "http://")) {
    input[0] = 'h'; input[1] = 't'; input[2] = 't'; input[3] = 'p'; input[4] = ':';
  } else if (startsWithIgnoreCase(input.c_str(), "https://")) {
    input[0] = 'h'; input[1] = 't'; input[2] = 't'; input[3] = 'p'; input[4] = 's'; input[5] = ':';
  } else {
    return {};
  }
  return input;
}

std::string WebBrowserActivity::resolveLink(const std::string& baseUrl, const std::string& rawHref) const {
  if (rawHref.empty() || rawHref[0] == '#') return {};
  std::string href = rawHref;
  const size_t fragment = href.find('#');
  if (fragment != std::string::npos) href.resize(fragment);
  if (href.empty()) return {};
  if (startsWithIgnoreCase(href.c_str(), "http://") || startsWithIgnoreCase(href.c_str(), "https://")) {
    return href;
  }
  if (href.compare(0, 2, "//") == 0) {
    const size_t scheme = baseUrl.find("://");
    return scheme == std::string::npos ? std::string() : baseUrl.substr(0, scheme + 1) + href;
  }
  if (href.find(":") != std::string::npos) return {};  // Disallow mailto:, javascript:, data:, etc.

  const size_t scheme = baseUrl.find("://");
  if (scheme == std::string::npos) return {};
  const size_t pathStart = baseUrl.find('/', scheme + 3);
  const std::string origin = pathStart == std::string::npos ? baseUrl : baseUrl.substr(0, pathStart);
  if (href[0] == '/') return origin + href;
  const size_t lastSlash = baseUrl.find_last_of('/');
  const std::string directory = lastSlash == std::string::npos || lastSlash < scheme + 2
                                    ? origin + "/"
                                    : baseUrl.substr(0, lastSlash + 1);
  return directory + href;
}

size_t WebBrowserActivity::nextVisualLine(const size_t start, size_t& end) const {
  if (start >= htmlLength) {
    end = htmlLength;
    return htmlLength;
  }
  size_t hardEnd = std::min(htmlLength, start + VIEW_LINE_WIDTH);
  size_t newline = start;
  while (newline < hardEnd && html[newline] != '\n') ++newline;
  if (newline < hardEnd && html[newline] == '\n') {
    end = newline;
    return newline + 1;
  }
  if (hardEnd < htmlLength) {
    size_t space = hardEnd;
    while (space > start && html[space] != ' ' && html[space] != '\n') --space;
    if (space > start) hardEnd = space;
  }
  end = hardEnd;
  size_t next = hardEnd;
  while (next < htmlLength && html[next] == ' ') ++next;
  if (next < htmlLength && html[next] == '\n') ++next;
  return next;
}

size_t WebBrowserActivity::visualLineCount() const {
  size_t count = 0;
  size_t position = 0;
  while (position < htmlLength) {
    size_t end = position;
    const size_t next = nextVisualLine(position, end);
    if (next <= position) break;
    position = next;
    ++count;
  }
  return count;
}

void WebBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const Rect header{0, metrics.topPadding, width, metrics.homeTopPadding};
  const char* headerTitle = state == State::LINKS ? tr(STR_BROWSER_LINKS) : tr(STR_WEB_BROWSER);
  GUI.drawHeader(renderer, header, headerTitle);

  MappedInputManager::Labels labels;
  if (state == State::START) {
    labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OPEN), tr(STR_SEARCH), "");
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 - 20, tr(STR_BROWSER_START_HINT), true);
  } else if (state == State::LOADING) {
    labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2, tr(STR_BROWSER_LOADING), true);
  } else if (state == State::ERROR) {
    labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SEARCH), tr(STR_BROWSER_URL), "");
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 - 15,
                              statusMessage.empty() ? tr(STR_BROWSER_FETCH_FAILED) : statusMessage.c_str(), true);
    if (!url.empty()) renderer.drawCenteredText(UI_10_FONT_ID, height / 2 + 18, url.c_str(), true);
  } else if (state == State::LINKS) {
    labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OPEN), tr(STR_BROWSER_URL),
                                   tr(STR_DIR_DOWN));
    const int lineHeight = std::max(18, renderer.getLineHeight(UI_10_FONT_ID));
    int y = header.y + header.height + 12;
    constexpr size_t MAX_VISIBLE_LINKS = 14;
    const size_t end = std::min(linkCount, firstVisibleLink + MAX_VISIBLE_LINKS);
    for (size_t i = firstVisibleLink; i < end; ++i) {
      char row[128]{};
      const char marker = i == selectedLink ? '>' : ' ';
      std::snprintf(row, sizeof(row), "%c %02u. %.96s", marker, static_cast<unsigned>(i + 1),
                    links[i].label.data());
      renderer.drawText(UI_10_FONT_ID, 16, y, row, true);
      y += lineHeight;
    }
    if (linkCount == 0) renderer.drawText(UI_10_FONT_ID, 16, y, tr(STR_BROWSER_NO_LINKS), true);
  } else {
    labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_BROWSER_LINKS),
                                   tr(STR_BROWSER_URL), tr(STR_DIR_DOWN));
    char urlLine[160]{};
    std::snprintf(urlLine, sizeof(urlLine), "%.148s", url.c_str());
    int y = header.y + header.height + 4;
    renderer.drawText(UI_10_FONT_ID, 16, y, urlLine, true);
    y += std::max(18, renderer.getLineHeight(UI_10_FONT_ID)) + 4;
    if (!statusMessage.empty()) {
      renderer.drawText(UI_10_FONT_ID, 16, y, statusMessage.c_str(), true);
      y += std::max(18, renderer.getLineHeight(UI_10_FONT_ID));
    }
    const int lineHeight = std::max(18, renderer.getLineHeight(UI_10_FONT_ID));
    const int available = height - y - metrics.buttonHintsHeight - 8;
    const size_t visibleLines = std::min(VIEW_LINES, static_cast<size_t>(std::max(1, available / lineHeight)));
    size_t position = 0;
    size_t lineIndex = 0;
    while (position < htmlLength && lineIndex < scrollLine) {
      size_t end = position;
      const size_t next = nextVisualLine(position, end);
      if (next <= position) break;
      position = next;
      ++lineIndex;
    }
    size_t drawn = 0;
    while (position < htmlLength && drawn < visibleLines) {
      size_t end = position;
      const size_t next = nextVisualLine(position, end);
      char line[VIEW_LINE_WIDTH + 1]{};
      const size_t lineLength = std::min(end - position, VIEW_LINE_WIDTH);
      if (lineLength > 0) std::memcpy(line, html.data() + position, lineLength);
      line[lineLength] = '\0';
      renderer.drawText(UI_10_FONT_ID, 16, y, line, true);
      y += lineHeight;
      ++drawn;
      if (next <= position) break;
      position = next;
    }
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
