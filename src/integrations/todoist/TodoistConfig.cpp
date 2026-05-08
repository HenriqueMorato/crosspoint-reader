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

}  // namespace

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

bool TodoistConfig::persist() {
  JsonDocument doc;
  doc["api_token"] = apiToken;
  doc["sleep_screen_enabled"] = sleepScreenEnabled;
  doc["activity_orientation"] = orientationToString(activityOrientation);
  doc["snapshot_orientation"] = orientationToString(snapshotOrientation);
  doc["date_filter"] = dateFilterToString(dateFilter);
  doc["overdue_filter"] = overdueFilterToString(overdueFilter);
  doc["gmt_offset"] = static_cast<int>(gmtOffset);

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
    loaded = false;
  }
  return ok;
}

}  // namespace todoist
