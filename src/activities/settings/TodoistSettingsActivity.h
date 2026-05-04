#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#include <GfxRenderer.h>

/**
 * Submenu for Todoist integration settings.
 * Items: sleep-screen toggle, activity orientation, snapshot orientation, forget.
 */
class TodoistSettingsActivity final : public Activity {
 public:
  explicit TodoistSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TodoistSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void handleSelection();
  GfxRenderer::Orientation nextOrientation(GfxRenderer::Orientation current) const;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  static constexpr int kItemCount = 4;
};
