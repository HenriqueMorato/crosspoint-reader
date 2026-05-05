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

const char* dateFilterLabel(todoist::DateFilter f) {
  switch (f) {
    case todoist::DateFilter::None:      return tr(STR_TODOIST_FILTER_NONE);
    case todoist::DateFilter::Today:     return tr(STR_TODOIST_FILTER_TODAY);
    case todoist::DateFilter::ThisWeek:  return tr(STR_TODOIST_FILTER_THIS_WEEK);
    case todoist::DateFilter::ThisMonth: return tr(STR_TODOIST_FILTER_THIS_MONTH);
  }
  return tr(STR_TODOIST_FILTER_TODAY);
}

const char* overdueFilterLabel(todoist::OverdueFilter f) {
  switch (f) {
    case todoist::OverdueFilter::None:      return tr(STR_TODOIST_FILTER_NONE);
    case todoist::OverdueFilter::Last7Days: return tr(STR_TODOIST_FILTER_LAST_7D);
    case todoist::OverdueFilter::All:       return tr(STR_TODOIST_FILTER_ALL);
  }
  return tr(STR_TODOIST_FILTER_LAST_7D);
}

todoist::DateFilter nextDateFilter(todoist::DateFilter f) {
  // Cycle widest-on-the-end: None → Today → ThisWeek → ThisMonth → None.
  switch (f) {
    case todoist::DateFilter::None:      return todoist::DateFilter::Today;
    case todoist::DateFilter::Today:     return todoist::DateFilter::ThisWeek;
    case todoist::DateFilter::ThisWeek:  return todoist::DateFilter::ThisMonth;
    case todoist::DateFilter::ThisMonth: return todoist::DateFilter::None;
  }
  return todoist::DateFilter::Today;
}

todoist::OverdueFilter nextOverdueFilter(todoist::OverdueFilter f) {
  switch (f) {
    case todoist::OverdueFilter::None:      return todoist::OverdueFilter::Last7Days;
    case todoist::OverdueFilter::Last7Days: return todoist::OverdueFilter::All;
    case todoist::OverdueFilter::All:       return todoist::OverdueFilter::None;
  }
  return todoist::OverdueFilter::Last7Days;
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
      TODOIST_CONFIG.setDateFilter(nextDateFilter(TODOIST_CONFIG.getDateFilter()));
      return;
    case 4:
      TODOIST_CONFIG.setOverdueFilter(nextOverdueFilter(TODOIST_CONFIG.getOverdueFilter()));
      return;
    case 5:
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
          case 3: return std::string(tr(STR_TODOIST_DATE_FILTER));
          case 4: return std::string(tr(STR_TODOIST_OVERDUE_FILTER));
          case 5: return std::string(tr(STR_TODOIST_FORGET));
        }
        return "";
      },
      nullptr, nullptr,
      [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(TODOIST_CONFIG.isSleepScreenEnabled() ? "On" : "Off");
          case 1: return std::string(orientationLabel(TODOIST_CONFIG.getActivityOrientation()));
          case 2: return std::string(orientationLabel(TODOIST_CONFIG.getSnapshotOrientation()));
          case 3: return std::string(dateFilterLabel(TODOIST_CONFIG.getDateFilter()));
          case 4: return std::string(overdueFilterLabel(TODOIST_CONFIG.getOverdueFilter()));
          case 5: return std::string("");
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
