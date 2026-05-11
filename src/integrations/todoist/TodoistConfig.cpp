#include "TodoistConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace todoist {

namespace {

constexpr const char* kConfigPath = "/.crosspoint/todoist.json";
constexpr const char* kConfigTmpPath = "/.crosspoint/todoist.json.tmp";
constexpr const char* kSnapshotBmpPath = "/.crosspoint/todoist_sleep.bmp";
constexpr const char* kSnapshotMetaPath = "/.crosspoint/todoist_sleep.meta";
constexpr const char* kSnapshotMetaTmpPath = "/.crosspoint/todoist_sleep.meta.tmp";

const char* orientationToString(GfxRenderer::Orientation o) {
  switch (o) {
    case GfxRenderer::Orientation::Portrait: return "portrait";
    case GfxRenderer::Orientation::PortraitInverted: return "portrait_inverted";
    case GfxRenderer::Orientation::LandscapeClockwise: return "landscape_cw";
    case GfxRenderer::Orientation::LandscapeCounterClockwise: return "landscape_ccw";
  }
  return "portrait";
}

GfxRenderer::Orientation orientationFromString(const char* s, GfxRenderer::Orientation fallback) {
  if (!s) return fallback;
  if (strcmp(s, "portrait") == 0) return GfxRenderer::Orientation::Portrait;
  if (strcmp(s, "portrait_inverted") == 0) return GfxRenderer::Orientation::PortraitInverted;
  if (strcmp(s, "landscape_cw") == 0) return GfxRenderer::Orientation::LandscapeClockwise;
  if (strcmp(s, "landscape_ccw") == 0) return GfxRenderer::Orientation::LandscapeCounterClockwise;
  return fallback;
}

const char* dateFilterToString(DateFilter f) {
  switch (f) {
    case DateFilter::None:      return "none";
    case DateFilter::Today:     return "today";
    case DateFilter::ThisWeek:  return "this_week";
    case DateFilter::ThisMonth: return "this_month";
  }
  return "today";
}

DateFilter dateFilterFromString(const char* s, DateFilter fallback) {
  if (!s) return fallback;
  if (strcmp(s, "none") == 0)       return DateFilter::None;
  if (strcmp(s, "today") == 0)      return DateFilter::Today;
  if (strcmp(s, "this_week") == 0)  return DateFilter::ThisWeek;
  if (strcmp(s, "this_month") == 0) return DateFilter::ThisMonth;
  return fallback;
}

const char* overdueFilterToString(OverdueFilter f) {
  switch (f) {
    case OverdueFilter::None:      return "none";
    case OverdueFilter::Last7Days: return "last_7_days";
    case OverdueFilter::All:       return "all";
  }
  return "last_7_days";
}

OverdueFilter overdueFilterFromString(const char* s, OverdueFilter fallback) {
  if (!s) return fallback;
  if (strcmp(s, "none") == 0)        return OverdueFilter::None;
  if (strcmp(s, "last_7_days") == 0) return OverdueFilter::Last7Days;
  if (strcmp(s, "all") == 0)         return OverdueFilter::All;
  return fallback;
}

DateFormat dateFormatFromString(const char* s, DateFormat fallback) {
  if (!s) return fallback;
  if (strcmp(s, "dd/mm") == 0) return DateFormat::DayMonthSlash;
  if (strcmp(s, "mm/dd") == 0) return DateFormat::MonthDaySlash;
  if (strcmp(s, "dd-mm") == 0) return DateFormat::DayMonthDash;
  if (strcmp(s, "mm-dd") == 0) return DateFormat::MonthDayDash;
  if (strcmp(s, "dd.mm") == 0) return DateFormat::DayMonthDot;
  if (strcmp(s, "mm.dd") == 0) return DateFormat::MonthDayDot;
  return fallback;
}

DesignMode designModeFromString(const char* s, DesignMode fallback) {
  if (!s) return fallback;
  if (strcmp(s, "minimal") == 0) return DesignMode::Minimal;
  if (strcmp(s, "daily") == 0)   return DesignMode::Daily;
  return fallback;
}

weather::TemperatureUnit temperatureUnitFromString(const char* s,
                                                   weather::TemperatureUnit fallback) {
  if (!s) return fallback;
  if (strcmp(s, "celsius") == 0)    return weather::TemperatureUnit::Celsius;
  if (strcmp(s, "fahrenheit") == 0) return weather::TemperatureUnit::Fahrenheit;
  return fallback;
}

}  // namespace

const char* dateFormatToString(DateFormat f) {
  switch (f) {
    case DateFormat::DayMonthSlash: return "dd/mm";
    case DateFormat::MonthDaySlash: return "mm/dd";
    case DateFormat::DayMonthDash:  return "dd-mm";
    case DateFormat::MonthDayDash:  return "mm-dd";
    case DateFormat::DayMonthDot:   return "dd.mm";
    case DateFormat::MonthDayDot:   return "mm.dd";
  }
  return "dd/mm";
}

const char* designModeToString(DesignMode d) {
  switch (d) {
    case DesignMode::Minimal: return "minimal";
    case DesignMode::Daily:   return "daily";
  }
  return "minimal";
}

size_t formatDate(int day, int month, DateFormat fmt, char* out, size_t outSize) {
  if (!out || outSize < 6) return 0;
  // Day-first formats put day before separator; month-first invert.
  // Separator differs across the three families (slash / dash / dot).
  const char sep = (fmt == DateFormat::DayMonthSlash || fmt == DateFormat::MonthDaySlash) ? '/'
                 : (fmt == DateFormat::DayMonthDash  || fmt == DateFormat::MonthDayDash)  ? '-'
                                                                                          : '.';
  const bool dayFirst = (fmt == DateFormat::DayMonthSlash ||
                         fmt == DateFormat::DayMonthDash ||
                         fmt == DateFormat::DayMonthDot);
  const int a = dayFirst ? day : month;
  const int b = dayFirst ? month : day;
  int n = snprintf(out, outSize, "%02d%c%02d", a, sep, b);
  return (n > 0 && static_cast<size_t>(n) < outSize) ? static_cast<size_t>(n) : 0;
}

TodoistConfig& TodoistConfig::getInstance() {
  static TodoistConfig instance;
  return instance;
}

bool TodoistConfig::load() {
  loaded = false;
  apiToken.clear();
  sleepScreenEnabled = false;
  activityOrientation = GfxRenderer::Orientation::Portrait;
  snapshotOrientation = GfxRenderer::Orientation::Portrait;
  dateFilter = DateFilter::Today;
  overdueFilter = OverdueFilter::Last7Days;
  gmtOffset = 0;
  dateFormat = DateFormat::DayMonthSlash;
  designMode = DesignMode::Minimal;
  latitude = 0.0;
  longitude = 0.0;
  locationName.clear();
  temperatureUnit = weather::TemperatureUnit::Celsius;
  cachedWeatherDate.clear();
  cachedWeatherWmo = 0;
  cachedWeatherHi = 0;
  cachedWeatherLo = 0;

  if (!Storage.exists(kConfigPath)) {
    LOG_DBG("TDST", "No config at %s", kConfigPath);
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TDST", kConfigPath, file)) {
    LOG_ERR("TDST", "Cannot open config for read");
    return false;
  }

  // Bounded read: config files are small. Anything past 4 KB is suspect.
  constexpr size_t kMaxConfigBytes = 4096;
  std::string buffer;
  buffer.reserve(kMaxConfigBytes);
  uint8_t chunk[256];
  while (true) {
    int n = file.read(chunk, sizeof(chunk));
    if (n <= 0) break;
    if (buffer.size() + static_cast<size_t>(n) > kMaxConfigBytes) {
      LOG_ERR("TDST", "Config exceeds %u bytes", static_cast<unsigned>(kMaxConfigBytes));
      return false;
    }
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
  }

  JsonDocument doc;
  auto err = deserializeJson(doc, buffer);
  if (err) {
    LOG_ERR("TDST", "JSON parse error: %s", err.c_str());
    return false;
  }

  apiToken = doc["api_token"] | std::string("");
  sleepScreenEnabled = doc["sleep_screen_enabled"] | false;
  activityOrientation = orientationFromString(
      doc["activity_orientation"] | static_cast<const char*>(nullptr),
      GfxRenderer::Orientation::Portrait);
  snapshotOrientation = orientationFromString(
      doc["snapshot_orientation"] | static_cast<const char*>(nullptr),
      GfxRenderer::Orientation::Portrait);
  dateFilter = dateFilterFromString(
      doc["date_filter"] | static_cast<const char*>(nullptr),
      DateFilter::Today);
  overdueFilter = overdueFilterFromString(
      doc["overdue_filter"] | static_cast<const char*>(nullptr),
      OverdueFilter::Last7Days);

  // Clamp on load: a corrupt or hand-edited file shouldn't be able to set
  // a wild offset that produces nonsense local time.
  int rawOffset = doc["gmt_offset"] | 0;
  if (rawOffset < -12) rawOffset = -12;
  if (rawOffset > 14)  rawOffset = 14;
  gmtOffset = static_cast<int8_t>(rawOffset);

  dateFormat = dateFormatFromString(
      doc["date_format"] | static_cast<const char*>(nullptr),
      DateFormat::DayMonthSlash);

  designMode = designModeFromString(
      doc["design"] | static_cast<const char*>(nullptr),
      DesignMode::Minimal);

  // Lat/lon default to 0 (sentinel "not configured") — hasLocation() keys
  // off the name being empty, not the coordinates, because (0,0) is a
  // legitimate point off the African coast and we shouldn't silently
  // re-geolocate a user who happens to be near it.
  latitude  = doc["latitude"]  | 0.0;
  longitude = doc["longitude"] | 0.0;
  locationName = doc["location_name"] | std::string("");
  temperatureUnit = temperatureUnitFromString(
      doc["temperature_unit"] | static_cast<const char*>(nullptr),
      weather::TemperatureUnit::Celsius);

  cachedWeatherDate = doc["weather_date"] | std::string("");
  cachedWeatherWmo = static_cast<uint8_t>(doc["weather_wmo"] | 0);
  cachedWeatherHi = static_cast<int16_t>(doc["weather_hi"] | 0);
  cachedWeatherLo = static_cast<int16_t>(doc["weather_lo"] | 0);

  loaded = true;
  LOG_DBG("TDST", "Config loaded (token=%s, sleep=%d)",
          apiToken.empty() ? "no" : "yes", sleepScreenEnabled);
  return true;
}

bool TodoistConfig::hasValidToken() const {
  return apiToken.size() >= 20;
}

bool TodoistConfig::setSleepScreenEnabled(bool enabled) {
  if (enabled == sleepScreenEnabled) return true;
  sleepScreenEnabled = enabled;
  return persist();
}

bool TodoistConfig::setActivityOrientation(GfxRenderer::Orientation o) {
  if (o == activityOrientation) return true;
  activityOrientation = o;
  return persist();
}

bool TodoistConfig::setSnapshotOrientation(GfxRenderer::Orientation o) {
  if (o == snapshotOrientation) return true;
  snapshotOrientation = o;
  return persist();
}

bool TodoistConfig::setDateFilter(DateFilter f) {
  if (f == dateFilter) return true;
  dateFilter = f;
  return persist();
}

bool TodoistConfig::setOverdueFilter(OverdueFilter f) {
  if (f == overdueFilter) return true;
  overdueFilter = f;
  return persist();
}

bool TodoistConfig::setGmtOffset(int8_t hours) {
  if (hours < -12) hours = -12;
  if (hours > 14)  hours = 14;
  if (hours == gmtOffset) return true;
  gmtOffset = hours;
  return persist();
}

bool TodoistConfig::setDateFormat(DateFormat f) {
  if (f == dateFormat) return true;
  dateFormat = f;
  return persist();
}

bool TodoistConfig::setDesignMode(DesignMode d) {
  if (d == designMode) return true;
  designMode = d;
  return persist();
}

bool TodoistConfig::setTemperatureUnit(weather::TemperatureUnit u) {
  if (u == temperatureUnit) return true;
  temperatureUnit = u;
  // Cached hi/lo were stored in the previous unit. Invalidate so the
  // next refresh re-fetches in the new unit rather than displaying
  // converted-but-cached values that look slightly off.
  cachedWeatherDate.clear();
  return persist();
}

bool TodoistConfig::setLocation(double lat, double lon, const char* cityUtf8) {
  // No early-return on unchanged values: a "re-detect" that resolves to the
  // same coordinates should still rewrite the file (touches mtime), which
  // signals to anyone watching that the detection actually ran.
  latitude = lat;
  longitude = lon;
  locationName = cityUtf8 ? std::string(cityUtf8) : std::string();
  // New (or re-confirmed) location → drop any cached forecast. Cheap to
  // re-fetch, and the user clearly wanted a fresh state.
  cachedWeatherDate.clear();
  return persist();
}

bool TodoistConfig::clearLocation() {
  if (!hasLocation() && latitude == 0.0 && longitude == 0.0 &&
      cachedWeatherDate.empty()) {
    return true;
  }
  latitude = 0.0;
  longitude = 0.0;
  locationName.clear();
  cachedWeatherDate.clear();
  return persist();
}

bool TodoistConfig::setCachedWeather(const char* todayYmd, uint8_t wmo,
                                     int hi, int lo) {
  if (!todayYmd || strlen(todayYmd) != 10) return false;
  cachedWeatherDate = todayYmd;
  cachedWeatherWmo = wmo;
  // Defensive clamp: hi/lo are int16 on disk so silly inputs (e.g. a
  // unit-mismatch ¬cached value of -300°C) can't overflow.
  if (hi > 32767) hi = 32767;
  if (hi < -32768) hi = -32768;
  if (lo > 32767) lo = 32767;
  if (lo < -32768) lo = -32768;
  cachedWeatherHi = static_cast<int16_t>(hi);
  cachedWeatherLo = static_cast<int16_t>(lo);
  return persist();
}

bool TodoistConfig::persist() {
  JsonDocument doc;
  doc["api_token"] = apiToken;
  doc["sleep_screen_enabled"] = sleepScreenEnabled;
  doc["activity_orientation"] = orientationToString(activityOrientation);
  doc["snapshot_orientation"] = orientationToString(snapshotOrientation);
  doc["date_filter"] = dateFilterToString(dateFilter);
  doc["overdue_filter"] = overdueFilterToString(overdueFilter);
  doc["gmt_offset"] = static_cast<int>(gmtOffset);
  doc["date_format"] = dateFormatToString(dateFormat);
  doc["design"] = designModeToString(designMode);
  doc["latitude"] = latitude;
  doc["longitude"] = longitude;
  doc["location_name"] = locationName;
  doc["temperature_unit"] = weather::temperatureUnitToString(temperatureUnit);
  doc["weather_date"] = cachedWeatherDate;
  doc["weather_wmo"] = cachedWeatherWmo;
  doc["weather_hi"] = cachedWeatherHi;
  doc["weather_lo"] = cachedWeatherLo;

  // Atomic write: serialize to .tmp, close, rename to final path.
  if (Storage.exists(kConfigTmpPath)) {
    Storage.remove(kConfigTmpPath);
  }

  std::string out;
  serializeJson(doc, out);

  // Scoped: HalFile destructor closes the underlying SdFat file before the
  // rename below — SdFat behaviour against an open handle is implementation-
  // defined, so we don't rely on it.
  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("TDST", kConfigTmpPath, file)) {
      LOG_ERR("TDST", "Cannot open tmp for write");
      return false;
    }
    written = file.write(reinterpret_cast<const uint8_t*>(out.data()), out.size());
  }

  if (written != out.size()) {
    LOG_ERR("TDST", "Short write: %u/%u", static_cast<unsigned>(written),
            static_cast<unsigned>(out.size()));
    Storage.remove(kConfigTmpPath);
    return false;
  }

  if (Storage.exists(kConfigPath)) {
    Storage.remove(kConfigPath);
  }
  if (!Storage.rename(kConfigTmpPath, kConfigPath)) {
    LOG_ERR("TDST", "Rename %s -> %s failed", kConfigTmpPath, kConfigPath);
    Storage.remove(kConfigTmpPath);
    return false;
  }

  LOG_DBG("TDST", "Config persisted");
  return true;
}

bool TodoistConfig::forget() {
  bool ok = true;
  if (Storage.exists(kConfigPath) && !Storage.remove(kConfigPath)) {
    LOG_ERR("TDST", "Could not remove %s", kConfigPath);
    ok = false;
  }
  if (Storage.exists(kSnapshotBmpPath) && !Storage.remove(kSnapshotBmpPath)) {
    LOG_ERR("TDST", "Could not remove %s", kSnapshotBmpPath);
    ok = false;
  }
  if (Storage.exists(kSnapshotMetaPath) && !Storage.remove(kSnapshotMetaPath)) {
    LOG_ERR("TDST", "Could not remove %s", kSnapshotMetaPath);
    ok = false;
  }
  // Best-effort cleanup of orphaned tmp files from any prior failed write.
  Storage.remove(kConfigTmpPath);
  Storage.remove(kSnapshotMetaTmpPath);

  if (ok) {
    apiToken.clear();
    sleepScreenEnabled = false;
    activityOrientation = GfxRenderer::Orientation::Portrait;
    snapshotOrientation = GfxRenderer::Orientation::Portrait;
    dateFilter = DateFilter::Today;
    overdueFilter = OverdueFilter::Last7Days;
    gmtOffset = 0;
    dateFormat = DateFormat::DayMonthSlash;
    designMode = DesignMode::Minimal;
    latitude = 0.0;
    longitude = 0.0;
    locationName.clear();
    temperatureUnit = weather::TemperatureUnit::Celsius;
    cachedWeatherDate.clear();
    cachedWeatherWmo = 0;
    cachedWeatherHi = 0;
    cachedWeatherLo = 0;
    loaded = false;
  }
  return ok;
}

}  // namespace todoist
