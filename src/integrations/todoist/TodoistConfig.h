#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

namespace todoist {

// Two-axis fetch filter. DateFilter sets the upper bound (how far into the
// future to include); OverdueFilter sets the lower bound (how far back
// overdue tasks count). Combined in TodoistClient when building the query.
//
// Defaults reproduce the pre-filter behaviour:
// `(today | overdue) & due after: -7 days`  ==  Today + Last7Days.
//
// Calendar boundaries are computed client-side from `time(nullptr)` because
// Todoist's filter parser rejects `this week` / `this month` as tokens
// (verified empirically). ThisWeek uses the ISO-week convention (Mon-Sun),
// so the cutoff is the upcoming Monday — i.e. the filter includes today
// through the upcoming Sunday. ThisMonth cutoff is the 1st of next month.

enum class DateFilter : uint8_t {
  None = 0,       // no date upper bound (overdue-only views)
  Today = 1,      // today only                              ← default
  ThisWeek = 2,   // today through end of ISO week (Sunday)
  ThisMonth = 3,  // today through end of current month
};

enum class OverdueFilter : uint8_t {
  None = 0,       // exclude overdue
  Last7Days = 1,  // overdue within last 7 days only         ← default
  All = 2,        // all overdue, no lower bound
};

class TodoistConfig {
 public:
  static TodoistConfig& getInstance();

  // Loads /.crosspoint/todoist.json if present. Safe to call repeatedly.
  // Returns true on successful load (file present, parsed). False means
  // the integration is effectively disabled until the user fixes the file.
  bool load();

  // Token getters. Empty string means "not configured".
  const std::string& getApiToken() const { return apiToken; }

  // Active fetch filter axes. Cycled independently via the Todoist
  // settings menu. The actual query string is built by TodoistClient
  // because the calendar-bounded date filters (ThisWeek / ThisMonth)
  // need today's date to compute the "due before:" cutoff.
  DateFilter getDateFilter() const { return dateFilter; }
  OverdueFilter getOverdueFilter() const { return overdueFilter; }

  // Toggle: render Todoist snapshot on sleep when available.
  bool isSleepScreenEnabled() const { return sleepScreenEnabled; }

  // Active-view orientation (when Todoist activity is open).
  GfxRenderer::Orientation getActivityOrientation() const { return activityOrientation; }

  // Sleep-screen snapshot orientation.
  GfxRenderer::Orientation getSnapshotOrientation() const { return snapshotOrientation; }

  // GMT offset in whole hours (-12..+14). The user-facing convention
  // matches civil usage: "GMT-3" means 3 hours behind UTC. Applied to the
  // C runtime via setenv("TZ", ...) so localtime_r returns wall-clock
  // time matching the user's locale. Range covers every standard zone;
  // half-hour offsets (India, Nepal) are not supported in v1.
  int8_t getGmtOffset() const { return gmtOffset; }

  // Setters. Each performs a value-change check and an atomic write
  // (.tmp + rename) on change. Returns false on persistence failure.
  bool setSleepScreenEnabled(bool enabled);
  bool setActivityOrientation(GfxRenderer::Orientation o);
  bool setSnapshotOrientation(GfxRenderer::Orientation o);
  bool setDateFilter(DateFilter f);
  bool setOverdueFilter(OverdueFilter f);
  bool setGmtOffset(int8_t hours);

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
  DateFilter dateFilter = DateFilter::Today;
  OverdueFilter overdueFilter = OverdueFilter::Last7Days;
  int8_t gmtOffset = 0;
  bool loaded = false;
};

#define TODOIST_CONFIG todoist::TodoistConfig::getInstance()

}  // namespace todoist
