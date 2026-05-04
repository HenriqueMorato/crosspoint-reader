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
      file.close();
      return false;
    }
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
  }
  file.close();

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

bool TodoistConfig::persist() {
  JsonDocument doc;
  doc["api_token"] = apiToken;
  doc["sleep_screen_enabled"] = sleepScreenEnabled;
  doc["activity_orientation"] = orientationToString(activityOrientation);
  doc["snapshot_orientation"] = orientationToString(snapshotOrientation);

  // Atomic write: serialize to .tmp, close, rename to final path.
  if (Storage.exists(kConfigTmpPath)) {
    Storage.remove(kConfigTmpPath);
  }

  HalFile file;
  if (!Storage.openFileForWrite("TDST", kConfigTmpPath, file)) {
    LOG_ERR("TDST", "Cannot open tmp for write");
    return false;
  }

  std::string out;
  serializeJson(doc, out);
  size_t written = file.write(reinterpret_cast<const uint8_t*>(out.data()), out.size());
  file.close();

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

  apiToken.clear();
  sleepScreenEnabled = false;
  activityOrientation = GfxRenderer::Orientation::Portrait;
  snapshotOrientation = GfxRenderer::Orientation::Portrait;
  loaded = false;
  return ok;
}

}  // namespace todoist
