#include "TodoistSettingsActivity.h"

#include "Logging.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "integrations/todoist/TodoistConfig.h"
#include "integrations/weather/WeatherClient.h"
#include "integrations/weather/WeatherTypes.h"

#include <I18n.h>
#include <WiFi.h>

#include <variant>

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

// Step the GMT offset by +1 hour, wrapping +14 → -12. Whole-hour zones
// only; half-hour offsets (India, Nepal) are not represented in v1.
int8_t nextGmtOffset(int8_t current) {
  int next = static_cast<int>(current) + 1;
  if (next > 14) next = -12;
  return static_cast<int8_t>(next);
}

// "GMT+0", "GMT-3", "GMT+5". %+d always emits a sign so the value lines
// up regardless of polarity. Returned pointer is to a function-local
// static — only safe to call once per render frame, which is the case
// here (settings list invokes the value-formatter once per row).
const char* gmtOffsetLabel(int8_t offset) {
  static char buf[8];
  snprintf(buf, sizeof(buf), "GMT%+d", static_cast<int>(offset));
  return buf;
}

const char* designModeLabel(todoist::DesignMode d) {
  switch (d) {
    case todoist::DesignMode::Minimal: return tr(STR_TODOIST_DESIGN_MINIMAL);
    case todoist::DesignMode::Daily:   return tr(STR_TODOIST_DESIGN_DAILY);
  }
  return tr(STR_TODOIST_DESIGN_MINIMAL);
}

todoist::DesignMode nextDesignMode(todoist::DesignMode d) {
  switch (d) {
    case todoist::DesignMode::Minimal: return todoist::DesignMode::Daily;
    case todoist::DesignMode::Daily:   return todoist::DesignMode::Minimal;
  }
  return todoist::DesignMode::Minimal;
}

const char* temperatureUnitLabel(weather::TemperatureUnit u) {
  return (u == weather::TemperatureUnit::Fahrenheit)
             ? tr(STR_TODOIST_TEMP_UNIT_F)
             : tr(STR_TODOIST_TEMP_UNIT_C);
}

weather::TemperatureUnit nextTemperatureUnit(weather::TemperatureUnit u) {
  return (u == weather::TemperatureUnit::Celsius) ? weather::TemperatureUnit::Fahrenheit
                                                  : weather::TemperatureUnit::Celsius;
}

todoist::DateFormat nextDateFormat(todoist::DateFormat f) {
  switch (f) {
    case todoist::DateFormat::DayMonthSlash: return todoist::DateFormat::MonthDaySlash;
    case todoist::DateFormat::MonthDaySlash: return todoist::DateFormat::DayMonthDash;
    case todoist::DateFormat::DayMonthDash:  return todoist::DateFormat::MonthDayDash;
    case todoist::DateFormat::MonthDayDash:  return todoist::DateFormat::DayMonthDot;
    case todoist::DateFormat::DayMonthDot:   return todoist::DateFormat::MonthDayDot;
    case todoist::DateFormat::MonthDayDot:   return todoist::DateFormat::DayMonthSlash;
  }
  return todoist::DateFormat::DayMonthSlash;
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
      TODOIST_CONFIG.setDesignMode(nextDesignMode(TODOIST_CONFIG.getDesignMode()));
      return;
    case 1:
      TODOIST_CONFIG.setSleepScreenEnabled(!TODOIST_CONFIG.isSleepScreenEnabled());
      return;
    case 2:
      TODOIST_CONFIG.setActivityOrientation(nextOrientation(TODOIST_CONFIG.getActivityOrientation()));
      return;
    case 3:
      TODOIST_CONFIG.setSnapshotOrientation(nextOrientation(TODOIST_CONFIG.getSnapshotOrientation()));
      return;
    case 4:
      TODOIST_CONFIG.setDateFilter(nextDateFilter(TODOIST_CONFIG.getDateFilter()));
      return;
    case 5:
      TODOIST_CONFIG.setOverdueFilter(nextOverdueFilter(TODOIST_CONFIG.getOverdueFilter()));
      return;
    case 6:
      TODOIST_CONFIG.setGmtOffset(nextGmtOffset(TODOIST_CONFIG.getGmtOffset()));
      return;
    case 7:
      TODOIST_CONFIG.setDateFormat(nextDateFormat(TODOIST_CONFIG.getDateFormat()));
      return;
    case 8:
      TODOIST_CONFIG.setTemperatureUnit(nextTemperatureUnit(TODOIST_CONFIG.getTemperatureUnit()));
      return;
    case 9:
      // WiFi must be up before we can geocode. If the user reached this
      // screen without first opening the Todoist activity (which brings
      // WiFi up via WifiSelectionActivity), esp_http_client will fault
      // deep in lwIP/mbedTLS — a NULL FreeRTOS semaphore — because the
      // network stack mutexes haven't been created. Bring it up here
      // ourselves when needed, then chain into the keyboard entry.
      if (WiFi.status() == WL_CONNECTED) {
        launchCityEntry();
      } else {
        LOG_DBG("TDST", "WiFi down, launching WifiSelectionActivity before city entry");
        startActivityForResult(
            std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
            [this](const ActivityResult& result) {
              if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                LOG_ERR("TDST", "WiFi not connected, cannot geocode");
                return;
              }
              launchCityEntry();
            });
      }
      return;
    case 10:
      TODOIST_CONFIG.forget();
      return;
  }
}

void TodoistSettingsActivity::launchCityEntry() {
  // Seed with the current value (if set) so the user can re-confirm or
  // tweak rather than retype. 63 chars cap mirrors the urlEncode buffer
  // sizing in WeatherClient.cpp.
  std::string initial = TODOIST_CONFIG.hasLocation() ? TODOIST_CONFIG.getLocationName() : "";
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput,
                                              std::string(tr(STR_TODOIST_LOCATION_PROMPT)),
                                              std::move(initial), 63, InputType::Text),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* kb = std::get_if<KeyboardResult>(&result.data);
        if (!kb || kb->text.empty()) return;
        double lat = 0, lon = 0;
        char canonical[32];
        auto res = weather::WeatherClient::geocodeCity(kb->text.c_str(), lat, lon,
                                                       canonical, sizeof(canonical));
        if (res != weather::FetchResult::Ok) {
          LOG_ERR("TDST", "Geocode failed for '%s' (%d)", kb->text.c_str(),
                  static_cast<int>(res));
          return;
        }
        // setLocation persists + invalidates the cached forecast, so the
        // next Todoist refresh fetches fresh weather for the new
        // coordinates without any extra plumbing.
        if (!TODOIST_CONFIG.setLocation(lat, lon, canonical)) {
          LOG_ERR("TDST", "Could not persist new location");
        }
      });
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
          case 0: return std::string(tr(STR_TODOIST_DESIGN));
          case 1: return std::string(tr(STR_TODOIST_SLEEP_SCREEN));
          case 2: return std::string(tr(STR_TODOIST_ACTIVITY_ORIENTATION));
          case 3: return std::string(tr(STR_TODOIST_SNAPSHOT_ORIENTATION));
          case 4: return std::string(tr(STR_TODOIST_DATE_FILTER));
          case 5: return std::string(tr(STR_TODOIST_OVERDUE_FILTER));
          case 6: return std::string(tr(STR_TODOIST_TIMEZONE));
          case 7: return std::string(tr(STR_TODOIST_DATE_FORMAT));
          case 8: return std::string(tr(STR_TODOIST_TEMP_UNIT));
          case 9: return std::string(tr(STR_TODOIST_LOCATION));
          case 10: return std::string(tr(STR_TODOIST_FORGET));
        }
        return "";
      },
      nullptr, nullptr,
      [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(designModeLabel(TODOIST_CONFIG.getDesignMode()));
          case 1: return std::string(TODOIST_CONFIG.isSleepScreenEnabled() ? "On" : "Off");
          case 2: return std::string(orientationLabel(TODOIST_CONFIG.getActivityOrientation()));
          case 3: return std::string(orientationLabel(TODOIST_CONFIG.getSnapshotOrientation()));
          case 4: return std::string(dateFilterLabel(TODOIST_CONFIG.getDateFilter()));
          case 5: return std::string(overdueFilterLabel(TODOIST_CONFIG.getOverdueFilter()));
          case 6: return std::string(gmtOffsetLabel(TODOIST_CONFIG.getGmtOffset()));
          case 7: return std::string(todoist::dateFormatToString(TODOIST_CONFIG.getDateFormat()));
          case 8: return std::string(temperatureUnitLabel(TODOIST_CONFIG.getTemperatureUnit()));
          case 9: return TODOIST_CONFIG.hasLocation()
                             ? TODOIST_CONFIG.getLocationName()
                             : std::string(tr(STR_TODOIST_LOCATION_NOT_SET));
          case 10: return std::string("");
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
