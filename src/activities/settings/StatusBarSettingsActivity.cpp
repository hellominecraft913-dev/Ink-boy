#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
enum BarItem {
  SLOT_LEFT_1,
  SLOT_LEFT_2,
  SLOT_LEFT_3,
  SLOT_CENTER,
  SLOT_RIGHT_1,
  SLOT_RIGHT_2,
  SLOT_RIGHT_3,
  PERCENTAGE_FORMAT,
  PROGRESS_BAR,
  PROGRESS_BAR_THICKNESS,
};

constexpr ReaderStatusBarItem pickerItems[] = {
    ReaderStatusBarItem::Clock,
    ReaderStatusBarItem::Date,
    ReaderStatusBarItem::Battery,
    ReaderStatusBarItem::TimeLeftBook,
    ReaderStatusBarItem::TimeLeftChapter,
    ReaderStatusBarItem::ChapterPageCount,
    ReaderStatusBarItem::StablePageNumber,
    ReaderStatusBarItem::BookProgressPercentage,
    ReaderStatusBarItem::TitleBook,
    ReaderStatusBarItem::TitleChapter,
    ReaderStatusBarItem::Empty,
};

std::string itemLabel(const ReaderStatusBarItem item) {
  switch (item) {
    case ReaderStatusBarItem::Date:
      return tr(STR_DATE);
    case ReaderStatusBarItem::Clock:
      return tr(STR_STATUS_BAR_CLOCK);
    case ReaderStatusBarItem::Battery:
      return tr(STR_BATTERY);
    case ReaderStatusBarItem::TimeLeftBook:
      return std::string(tr(STR_TIME_LEFT)) + " (" + tr(STR_BOOK) + ")";
    case ReaderStatusBarItem::TimeLeftChapter:
      return std::string(tr(STR_TIME_LEFT)) + " (" + tr(STR_CHAPTER) + ")";
    case ReaderStatusBarItem::ChapterPageCount:
      return tr(STR_CHAPTER_PAGE_COUNT);
    case ReaderStatusBarItem::StablePageNumber:
      return tr(STR_STABLE_PAGE_NUMBERS);
    case ReaderStatusBarItem::BookProgressPercentage:
      return tr(STR_BOOK_PROGRESS_PERCENTAGE);
    case ReaderStatusBarItem::TitleBook:
      return std::string(tr(STR_TITLE)) + " (" + tr(STR_BOOK) + ")";
    case ReaderStatusBarItem::TitleChapter:
      return std::string(tr(STR_TITLE)) + " (" + tr(STR_CHAPTER) + ")";
    default:
      return tr(STR_STATUS_BAR_EMPTY);
  }
}

const char* xtcModeLabel(const uint8_t mode) {
  switch (mode) {
    case CrossPointSettings::XTC_STATUS_BAR_BOTTOM:
      return tr(STR_BOTTOM);
    case CrossPointSettings::XTC_STATUS_BAR_TOP:
      return tr(STR_TOP);
    case CrossPointSettings::XTC_STATUS_BAR_BOTH:
      return tr(STR_STATUS_BAR_BOTH);
    default:
      return tr(STR_HIDE);
  }
}

const char* progressModeLabel(const uint8_t mode) {
  switch (mode) {
    case CrossPointSettings::BOOK_PROGRESS:
      return tr(STR_BOOK);
    case CrossPointSettings::CHAPTER_PROGRESS:
      return tr(STR_CHAPTER);
    default:
      return tr(STR_HIDE);
  }
}

const char* thicknessLabel(const uint8_t thickness) {
  switch (thickness) {
    case CrossPointSettings::PROGRESS_BAR_THIN:
      return tr(STR_PROGRESS_BAR_THIN);
    case CrossPointSettings::PROGRESS_BAR_THICK:
      return tr(STR_PROGRESS_BAR_THICK);
    default:
      return tr(STR_PROGRESS_BAR_MEDIUM);
  }
}

const StrId percentageFormatNames[] = {StrId::STR_PERCENTAGE_FORMAT_WHOLE, StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                                       StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS};
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();
  view = displayContext ? View::Top : View::Root;
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  listNav.reset();
  uiReady = false;
  refreshItemCount();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &StatusBarSettingsActivity::onRowEvent, this);
  app.setScreen(&StatusBarSettingsActivity::settingsScreen, this);
  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

bool StatusBarSettingsActivity::handleHomeGesture() {
  goBack();
  return true;
}

void StatusBarSettingsActivity::goBack() {
  if (displayContext || view == View::Root) {
    finishAfterBackPress();
  } else {
    view = View::Root;
    selectedIndex = 0;
    topIndex = 0;
    listNav.reset();
    refreshItemCount();
    requestUpdate();
  }
}

ReaderStatusBarPosition StatusBarSettingsActivity::selectedPosition() const {
  return view == View::Top ? ReaderStatusBarPosition::Top : ReaderStatusBarPosition::Bottom;
}

void StatusBarSettingsActivity::refreshItemCount() {
  visibleItemCount = displayContext || view == View::Root ? 3 : PROGRESS_BAR_THICKNESS + 1;
  selectedIndex = std::clamp(selectedIndex, 0, visibleItemCount - 1);
}

int StatusBarSettingsActivity::previewHeight() const {
  if (view == View::Root) return 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getLineHeight(UI_10_FONT_ID) + 18 +
         (displayContext ? metrics.statusBarVerticalMargin + ReaderStatusBarConfig::TOP_TEXT_INSET
                         : UITheme::getReaderStatusBarHeight(selectedPosition())) +
         metrics.verticalSpacing;
}

int StatusBarSettingsActivity::topPreviewOriginY() const {
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  (void)marginRight;
  (void)marginBottom;
  (void)marginLeft;
  return marginTop + UITheme::getInstance().getMetrics().topPadding + UITheme::getTopStatusBarInset(renderer);
}

Rect StatusBarSettingsActivity::settingsHeaderRect() const {
  Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (view == View::Top) header.y = topPreviewOriginY() + previewHeight();
  return header;
}

void StatusBarSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, settingsHeaderRect())) {
    goBack();
    return;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.hasTouch()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
      const int next = scrollListBy(topIndex, delta, visibleRows, visibleItemCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });
}

void StatusBarSettingsActivity::handleSelection() {
  if (view == View::Root && selectedIndex < 2) {
    view = selectedIndex == 0 ? View::Top : View::Bottom;
    selectedIndex = 0;
    topIndex = 0;
    listNav.reset();
    refreshItemCount();
    requestUpdate();
    return;
  }
  openOptionPicker();
}

void StatusBarSettingsActivity::openOptionPicker() {
  if (view == View::Root) {
    const std::vector<std::string> options = {tr(STR_HIDE), tr(STR_BOTTOM), tr(STR_TOP), tr(STR_STATUS_BAR_BOTH)};
    optionPopup.show(StrId::STR_XTC_STATUS_BAR, options, SETTINGS.xtcStatusBarMode, [this](const int selected) {
      if (SETTINGS.xtcStatusBarMode != selected) SETTINGS.legacyXtcTopUsesBottom = 0;
      SETTINGS.xtcStatusBarMode = static_cast<uint8_t>(selected);
      SETTINGS.saveToFile();
      requestUpdate();
    });
    requestUpdate();
    return;
  }

  const auto position = selectedPosition();
  const int item = selectedIndex;
  const auto config = SETTINGS.readerStatusBar(position);
  const auto currentItem = displayContext ? SETTINGS.displayStatusBar.slots[item]
                                          : config.slots[std::min(item, static_cast<int>(SLOT_RIGHT_3))];
  std::vector<std::string> options;
  std::vector<uint8_t> rawValues;
  StrId titleId = displayContext      ? StrId::STR_STATUS_BAR
                  : view == View::Top ? StrId::STR_TOP_STATUS_BAR
                                      : StrId::STR_BOTTOM_STATUS_BAR;
  int currentIndex = 0;
  if (item <= SLOT_RIGHT_3) {
    for (const auto choice : pickerItems) {
      if (displayContext && !validDisplayStatusBarItemValue(static_cast<int>(choice), halClock.isAvailable())) continue;
      if ((choice == ReaderStatusBarItem::Clock || choice == ReaderStatusBarItem::Date) && !halClock.isAvailable())
        continue;
      if (currentItem == choice) currentIndex = static_cast<int>(options.size());
      options.push_back(itemLabel(choice));
      rawValues.push_back(static_cast<uint8_t>(choice));
    }
  } else if (item == PERCENTAGE_FORMAT) {
    titleId = StrId::STR_PERCENTAGE_FORMAT;
    for (uint8_t i = 0; i < CrossPointSettings::BOOK_PERCENTAGE_FORMAT_COUNT; ++i) {
      options.emplace_back(I18N.get(percentageFormatNames[i]));
      rawValues.push_back(i);
    }
    currentIndex = config.percentageFormat;
  } else if (item == PROGRESS_BAR) {
    titleId = StrId::STR_PROGRESS_BAR;
    options = {tr(STR_HIDE), tr(STR_BOOK), tr(STR_CHAPTER)};
    rawValues = {CrossPointSettings::HIDE_PROGRESS, CrossPointSettings::BOOK_PROGRESS,
                 CrossPointSettings::CHAPTER_PROGRESS};
    for (unsigned i = 0; i < rawValues.size(); ++i) {
      if (rawValues[i] == config.progressBar) currentIndex = static_cast<int>(i);
    }
  } else {
    titleId = StrId::STR_PROGRESS_BAR_THICKNESS;
    options = {tr(STR_PROGRESS_BAR_THIN), tr(STR_PROGRESS_BAR_MEDIUM), tr(STR_PROGRESS_BAR_THICK)};
    rawValues = {0, 1, 2};
    currentIndex = config.progressBarThickness;
  }
  optionPopup.show(titleId, options, currentIndex, [this, position, item, rawValues](const int selected) {
    if (selected < 0 || static_cast<size_t>(selected) >= rawValues.size()) return;
    if (displayContext) {
      SETTINGS.displayStatusBar.slots[item] = static_cast<ReaderStatusBarItem>(rawValues[selected]);
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    }
    auto updated = SETTINGS.readerStatusBar(position);
    const uint8_t value = rawValues[selected];
    if (item <= SLOT_RIGHT_3) {
      updated.slots[item] = static_cast<ReaderStatusBarItem>(value);
    } else if (item == PERCENTAGE_FORMAT) {
      updated.percentageFormat = value;
    } else if (item == PROGRESS_BAR) {
      updated.progressBar = value;
    } else {
      updated.progressBarThickness = value;
    }
    SETTINGS.setReaderStatusBar(position, updated);
    SETTINGS.saveToFile();
    refreshItemCount();
    requestUpdate();
  });
  requestUpdate();
}

void StatusBarSettingsActivity::settingsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<StatusBarSettingsActivity*>(user)->buildSettingsScreen(screen);
}

void StatusBarSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<StatusBarSettingsActivity*>(user);
  if (self->optionPopup.isActive() || event.value < 0 || event.value >= self->visibleItemCount) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->handleSelection();
}

void StatusBarSettingsActivity::buildSettingsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool landscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = landscape ? metrics.buttonHintsHeight : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const Rect header = settingsHeaderRect();
  const int contentTop = header.y + header.height + metrics.verticalSpacing;
  const int bottomPreviewHeight = view == View::Bottom ? previewHeight() : 0;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - bottomPreviewHeight - metrics.verticalSpacing * 2;
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(contentTop), static_cast<int16_t>(pageWidth - (contentX + contentWidth)),
                  static_cast<int16_t>(pageHeight - (contentTop + contentHeight)), static_cast<int16_t>(contentX)});

  const auto config = view == View::Root ? ReaderStatusBarConfig{} : SETTINGS.readerStatusBar(selectedPosition());
  std::vector<std::string> labels(visibleItemCount);
  std::vector<std::string> values(visibleItemCount);
  std::vector<fui::ListItem> items;
  items.reserve(visibleItemCount);
  for (int i = 0; i < visibleItemCount; ++i) {
    fui::ListItem row;
    row.actionValue = static_cast<int16_t>(i);
    if (displayContext) {
      row.label = i == 0 ? tr(STR_STATUS_BAR_LEFT) : i == 1 ? tr(STR_CENTER) : tr(STR_STATUS_BAR_RIGHT);
      values[i] = itemLabel(SETTINGS.displayStatusBar.slots[i]);
      row.value = values[i].c_str();
    } else if (view == View::Root) {
      row.label = i == 0 ? tr(STR_TOP_STATUS_BAR) : i == 1 ? tr(STR_BOTTOM_STATUS_BAR) : tr(STR_XTC_STATUS_BAR);
      row.value = i == 2 ? xtcModeLabel(SETTINGS.xtcStatusBarMode) : ">";
    } else {
      const int item = i;
      if (item <= SLOT_RIGHT_3) {
        values[i] = itemLabel(config.slots[item]);
        if (item <= SLOT_LEFT_3) {
          labels[i] = std::string(tr(STR_STATUS_BAR_LEFT)) + " " + std::to_string(item + 1);
        } else if (item == SLOT_CENTER) {
          labels[i] = tr(STR_CENTER);
        } else {
          labels[i] = std::string(tr(STR_STATUS_BAR_RIGHT)) + " " + std::to_string(item - SLOT_RIGHT_1 + 1);
        }
        row.label = labels[i].c_str();
        row.value = values[i].c_str();
        if (item == SLOT_LEFT_1) row.sectionHeading = tr(STR_STATUS_BAR_LEFT);
        if (item == SLOT_CENTER) row.sectionHeading = tr(STR_CENTER);
        if (item == SLOT_RIGHT_1) row.sectionHeading = tr(STR_STATUS_BAR_RIGHT);
      } else if (item == PERCENTAGE_FORMAT) {
        row.label = tr(STR_PERCENTAGE_FORMAT);
        row.value = I18N.get(percentageFormatNames[config.percentageFormat]);
      } else if (item == PROGRESS_BAR) {
        row.label = tr(STR_PROGRESS_BAR);
        row.value = progressModeLabel(config.progressBar);
      } else {
        row.label = tr(STR_PROGRESS_BAR_THICKNESS);
        row.value = thicknessLabel(config.progressBarThickness);
      }
    }
    items.push_back(row);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.headerText = screen.theme().smallText;
  props.headerText.bold = true;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  // Section headings and wrapped values can fit fewer rows than this fixed-height estimate.
  visibleRows = listNav.trusts(visibleItemCount) ? listNav.pageRowsFor(visibleItemCount) : std::max<int>(rows, 1);
  topIndex = scrollListBy(topIndex, 0, visibleRows, visibleItemCount);
  props.topIndex = static_cast<uint16_t>(topIndex);
  if (displayContext || view == View::Root) {
    props.nav = &listNav;
    screen.list(props);
    visibleRows = listNav.pageRowsFor(visibleItemCount);
    topIndex = listNav.top;
    return;
  }

  const auto renderScrollingList = [&] {
    props.nav = &listNav;
    screen.list(props);
    visibleRows = listNav.pageRowsFor(visibleItemCount);
    topIndex = listNav.top;
  };

  // Keep the separator aligned with FreeInkUI's variable-height rows when the list scrolls or wraps.
  const auto resolvedProps = screen.resolveListProps(props);
  const auto body = screen.body();
  const int dividerItem = SLOT_RIGHT_3;
  const int dividerIndex = [&] {
    for (int i = topIndex; i < visibleItemCount; ++i) {
      if (i == dividerItem) return i;
    }
    return -1;
  }();
  if (dividerIndex < topIndex) {
    renderScrollingList();
    return;
  }

  fui::ListProps layoutProps = resolvedProps;
  layoutProps.scrollIndicator = false;
  int16_t rowWidth = body.width;
  if (layoutProps.rowInset > 0) rowWidth = static_cast<int16_t>(rowWidth - layoutProps.rowInset * 2);
  const int16_t headerLineHeight = screen.target().lineHeight(layoutProps.headerText.font);
  const int16_t headerHeight =
      layoutProps.headerRowHeight > 0 ? layoutProps.headerRowHeight : static_cast<int16_t>(headerLineHeight + 4);
  int16_t dividerY = body.y;
  for (int i = topIndex; i <= dividerIndex; ++i) {
    const auto& item = items[static_cast<size_t>(i)];
    if (item.sectionHeading != nullptr && item.sectionHeading[0] != '\0') {
      if (i != topIndex) dividerY = static_cast<int16_t>(dividerY + layoutProps.sectionGap);
      dividerY = static_cast<int16_t>(dividerY + headerHeight + layoutProps.rowGap);
    }
    dividerY = static_cast<int16_t>(dividerY +
                                    fui::measureListRow(screen.target(), nullptr, rowWidth, layoutProps, item).height);
    if (i != dividerIndex) dividerY = static_cast<int16_t>(dividerY + layoutProps.rowGap);
  }

  int16_t afterDividerHeight = 0;
  for (int i = dividerIndex + 1; i < visibleItemCount; ++i) {
    const auto& item = items[static_cast<size_t>(i)];
    afterDividerHeight = static_cast<int16_t>(
        afterDividerHeight + fui::measureListRow(screen.target(), nullptr, rowWidth, layoutProps, item).height);
    if (i + 1 < visibleItemCount) afterDividerHeight = static_cast<int16_t>(afterDividerHeight + layoutProps.rowGap);
  }
  const int16_t availableAfterDivider = static_cast<int16_t>(body.bottom() - dividerY);
  const int16_t maxDividerMargin = static_cast<int16_t>((availableAfterDivider - afterDividerHeight - 1) / 2);
  if (maxDividerMargin < 0) {
    // On compact screens, keep every option in one scrollable list instead of
    // splitting off a segment that cannot fit its first row.
    renderScrollingList();
    screen.target().fill(fui::Rect{body.x, dividerY, body.width, 1}, fui::Paint::solid(fui::Color::Black));
    return;
  }
  const int16_t dividerMargin = std::min(layoutProps.sectionGap, maxDividerMargin);

  fui::ListProps beforeDivider = props;
  beforeDivider.items = items.data() + topIndex;
  beforeDivider.count = static_cast<uint16_t>(dividerIndex - topIndex + 1);
  beforeDivider.topIndex = 0;
  beforeDivider.selectedIndex =
      selectedIndex >= topIndex && selectedIndex <= dividerIndex ? static_cast<int16_t>(selectedIndex - topIndex) : -1;
  beforeDivider.scrollIndicator = false;
  screen.list(beforeDivider, static_cast<int16_t>(dividerY - body.y));

  screen.spacer(dividerMargin);
  const auto dividerBody = screen.body();
  screen.target().fill(fui::Rect{dividerBody.x, dividerBody.y, dividerBody.width, 1},
                       fui::Paint::solid(fui::Color::Black));
  screen.spacer(1);
  screen.spacer(dividerMargin);

  fui::ListProps afterDivider = props;
  afterDivider.items = items.data() + dividerIndex + 1;
  afterDivider.count = static_cast<uint16_t>(visibleItemCount - dividerIndex - 1);
  afterDivider.topIndex = 0;
  afterDivider.selectedIndex =
      selectedIndex > dividerIndex ? static_cast<int16_t>(selectedIndex - dividerIndex - 1) : -1;
  afterDivider.scrollIndicator = false;
  screen.list(afterDivider);

  // The split sections share one scroll position, so keep one indicator for the full list.
  if (resolvedProps.scrollIndicator) {
    fui::drawListScrollIndicator(screen.target(), body, static_cast<uint32_t>(visibleItemCount),
                                 static_cast<uint32_t>(visibleItemCount - topIndex), static_cast<uint32_t>(topIndex),
                                 resolvedProps.scrollIndicatorWidth, resolvedProps.scrollIndicatorSide,
                                 resolvedProps.scrollIndicatorInset);
  }
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool landscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? metrics.buttonHintsHeight : 0;
  const int contentWidth = pageWidth - (landscape ? metrics.buttonHintsHeight : 0);
  const char* headerTitle = displayContext       ? tr(STR_STATUS_BAR)
                            : view == View::Root ? tr(STR_STATUS_BARS)
                            : view == View::Top  ? tr(STR_TOP_STATUS_BAR)
                                                 : tr(STR_BOTTOM_STATUS_BAR);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const Rect header = settingsHeaderRect();
  const bool showHeaderStatus = view == View::Root;
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, headerTitle, readerContext, 0, nullptr,
                                TouchHeaderBackButton::TITLE_VERTICAL_OFFSET, showHeaderStatus);
  } else {
    GUI.drawHeader(renderer, Rect{contentX, header.y, contentWidth, header.height}, headerTitle, nullptr, readerContext,
                   showHeaderStatus);
  }
  uiReady = false;
  app.render();
  uiReady = true;
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  if (view != View::Root) {
    const auto position = selectedPosition();
    const int barHeight = displayContext ? metrics.statusBarVerticalMargin + ReaderStatusBarConfig::TOP_TEXT_INSET
                                         : UITheme::getReaderStatusBarHeight(position);
    const int previewOriginY = view == View::Top
                                   ? topPreviewOriginY()
                                   : pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - barHeight;
    const int labelY = view == View::Top ? previewOriginY + barHeight + 9
                                         : previewOriginY - renderer.getLineHeight(UI_10_FONT_ID) - 18;
    renderer.drawText(UI_10_FONT_ID, contentX + metrics.contentSidePadding, labelY, tr(STR_PREVIEW));
    ReaderStatusBarContent content;
    content.outsideReader = displayContext;
    content.bookProgress = 75.12f;
    content.chapterProgress = 35.0f;
    content.chapterPage = 8;
    content.chapterPageCount = 32;
    content.stablePage = 120;
    content.stablePageCount = 540;
    content.bookTitle = tr(STR_EXAMPLE_BOOK);
    content.chapterTitle = tr(STR_EXAMPLE_CHAPTER);
    content.timeLeftBook = "3h 40m";
    content.timeLeftChapter = "1h 20m";
    content.previewClock = "12:34";
    content.previewOriginY = previewOriginY;
    const auto displayConfig = SETTINGS.displayStatusBar.asReaderConfig();
    GUI.drawReaderStatusBar(renderer, position, content, displayContext ? &displayConfig : nullptr);
  }
  renderer.displayBuffer();
}
