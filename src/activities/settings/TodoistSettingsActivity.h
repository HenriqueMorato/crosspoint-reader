#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#include <GfxRenderer.h>

/**
 * Submenu for Todoist integration settings.
 * Items (in order): design mode, sleep-screen toggle, activity orientation,
 * snapshot orientation, date filter, overdue filter, GMT offset, date format,
 * temperature unit, location, forget.
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
  // Opens the on-screen keyboard for city entry and, on confirm, geocodes
  // via Open-Meteo + persists. Must only be called when WiFi is already
  // connected — the caller (handleSelection case 9) is responsible for
  // launching WifiSelectionActivity first if not.
  void launchCityEntry();
  GfxRenderer::Orientation nextOrientation(GfxRenderer::Orientation current) const;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  static constexpr int kItemCount = 11;
};
