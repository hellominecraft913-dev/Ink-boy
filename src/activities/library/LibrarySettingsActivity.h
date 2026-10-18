#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LibrarySettingsActivity final : public Activity {
 public:
  LibrarySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<16, 2>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator buttonNavigator;
  freeink::ui::ListNav listNav;
  int selection = 0;
  int topIndex = 0;
  bool showSelection = true;
  bool uiReady = false;
  bool ignoreConfirmRelease = false;

  static void screen(UiApp::ScreenType& screen, void* user);
  static void onRow(const freeink::ui::ActionEvent& event, void* user);
  static void provideRow(void* user, uint16_t row, freeink::ui::ListItem& item);
  void buildScreen(UiApp::ScreenType& screen);
  void toggle(int row);
};
