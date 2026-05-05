#include "TodoistSettingsActivity.h"

#include "Logging.h"
#include "components/UITheme.h"
#include "integrations/todoist/TodoistConfig.h"

#include <I18n.h>

namespace {

const char* orientationLabel(GfxRenderer::Orientation o) {
  switch (o) {
    case GfxRenderer::Orientation::Portrait:                  return "Portrait";
    case GfxRenderer::Orientation::PortraitInverted:          return "Portrait inverted";
    case GfxRenderer::Orientation::LandscapeClockwise:        return "Landscape CW";
    case GfxRenderer::Orientation::LandscapeCounterClockwise: return "Landscape CCW";
  }
  return "Portrait";
}

}  // namespace

void TodoistSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void TodoistSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % kItemCount;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + kItemCount - 1) % kItemCount;
    requestUpdate();
  });
}

void TodoistSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case 0:
      TODOIST_CONFIG.setSleepScreenEnabled(!TODOIST_CONFIG.isSleepScreenEnabled());
      return;
    case 1:
      TODOIST_CONFIG.setActivityOrientation(nextOrientation(TODOIST_CONFIG.getActivityOrientation()));
      return;
    case 2:
      TODOIST_CONFIG.setSnapshotOrientation(nextOrientation(TODOIST_CONFIG.getSnapshotOrientation()));
      return;
    case 3:
      TODOIST_CONFIG.forget();
      return;
  }
}

GfxRenderer::Orientation TodoistSettingsActivity::nextOrientation(GfxRenderer::Orientation current) const {
  // Cycle in physical clockwise rotation: each step rotates the device 90° CW.
  switch (current) {
    case GfxRenderer::Orientation::Portrait:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case GfxRenderer::Orientation::LandscapeClockwise:
      return GfxRenderer::Orientation::PortraitInverted;
    case GfxRenderer::Orientation::PortraitInverted:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

void TodoistSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TODOIST));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, kItemCount, selectedIndex,
      [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(tr(STR_TODOIST_SLEEP_SCREEN));
          case 1: return std::string(tr(STR_TODOIST_ACTIVITY_ORIENTATION));
          case 2: return std::string(tr(STR_TODOIST_SNAPSHOT_ORIENTATION));
          case 3: return std::string(tr(STR_TODOIST_FORGET));
        }
        return "";
      },
      nullptr, nullptr,
      [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(TODOIST_CONFIG.isSleepScreenEnabled() ? "On" : "Off");
          case 1: return std::string(orientationLabel(TODOIST_CONFIG.getActivityOrientation()));
          case 2: return std::string(orientationLabel(TODOIST_CONFIG.getSnapshotOrientation()));
          case 3: return std::string("");
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
