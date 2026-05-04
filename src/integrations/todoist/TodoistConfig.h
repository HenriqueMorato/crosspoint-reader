#pragma once

#include <GfxRenderer.h>

#include <string>

namespace todoist {

class TodoistConfig {
 public:
  static TodoistConfig& getInstance();

  // Loads /.crosspoint/todoist.json if present. Safe to call repeatedly.
  // Returns true on successful load (file present, parsed). False means
  // the integration is effectively disabled until the user fixes the file.
  bool load();

  // Token getters. Empty string means "not configured".
  const std::string& getApiToken() const { return apiToken; }

  // Toggle: render Todoist snapshot on sleep when available.
  bool isSleepScreenEnabled() const { return sleepScreenEnabled; }

  // Active-view orientation (when Todoist activity is open).
  GfxRenderer::Orientation getActivityOrientation() const { return activityOrientation; }

  // Sleep-screen snapshot orientation.
  GfxRenderer::Orientation getSnapshotOrientation() const { return snapshotOrientation; }

  // Setters. Each performs a value-change check and an atomic write
  // (.tmp + rename) on change. Returns false on persistence failure.
  bool setSleepScreenEnabled(bool enabled);
  bool setActivityOrientation(GfxRenderer::Orientation o);
  bool setSnapshotOrientation(GfxRenderer::Orientation o);

  // Token convenience: true when token is non-empty and >= 20 chars.
  bool hasValidToken() const;

  // Forget: deletes /.crosspoint/todoist.json and /.crosspoint/todoist_sleep.bmp
  // and resets in-memory state to defaults.
  bool forget();

 private:
  TodoistConfig() = default;
  TodoistConfig(const TodoistConfig&) = delete;
  TodoistConfig& operator=(const TodoistConfig&) = delete;

  // Serialize current state to /.crosspoint/todoist.json via temp+rename.
  bool persist();

  std::string apiToken;
  bool sleepScreenEnabled = false;
  GfxRenderer::Orientation activityOrientation = GfxRenderer::Orientation::Portrait;
  GfxRenderer::Orientation snapshotOrientation = GfxRenderer::Orientation::Portrait;
  bool loaded = false;
};

#define TODOIST_CONFIG todoist::TodoistConfig::getInstance()

}  // namespace todoist
