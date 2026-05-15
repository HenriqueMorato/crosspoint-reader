#pragma once

#include "integrations/weather/WeatherTypes.h"

#include <GfxRenderer.h>

#include <cstdint>
#include <ctime>
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

// Overall layout of the Todoist activity. Minimal is the original compact
// list (Todoist title + Updated stamp + tasks). Daily is a richer dashboard
// with today's date, day-of-week, weather row, and tasks, with the Updated
// stamp moved to the bottom. The sleep-screen snapshot mirrors whichever
// design is active.
enum class DesignMode : uint8_t {
  Minimal = 0,   // ← default, backwards-compatible
  Daily = 1,
};

// Display format for any date rendered by the Todoist activity (header
// "Updated ..." stamp and per-row due-date suffixes). Year is omitted in
// every variant — task date ranges never span multiple years in normal
// use, so the year is implicit and the column stays narrow.
enum class DateFormat : uint8_t {
  DayMonthSlash = 0,   // 31/12                               ← default
  MonthDaySlash = 1,   // 12/31
  DayMonthDash = 2,    // 31-12
  MonthDayDash = 3,    // 12-31
  DayMonthDot = 4,     // 31.12
  MonthDayDot = 5,     // 12.31
};

// Render a date into `out` according to `fmt`. Always writes a NUL terminator.
// `outSize` must be >= 6 (5 chars + NUL). Returns chars written (excluding
// NUL), or 0 on overflow / invalid input.
size_t formatDate(int day, int month, DateFormat fmt, char* out, size_t outSize);

// Canonical wire/display string for `f`, e.g. "dd/mm". Used both as the
// JSON persist value and as the settings-row label — the patterns are
// universal English shorthand, so a single source of truth works for both.
const char* dateFormatToString(DateFormat f);

// Canonical wire string for the design mode ("minimal" / "daily"). The
// human-readable settings label is i18n'd (see the STR_TODOIST_DESIGN
// family of strings) so this is wire-only and not reused as a label.
const char* designModeToString(DesignMode d);

// Source-of-truth for location and timezone. Auto pulls from ip-api.com on
// each Todoist refresh (cached 24h on the SD card so we don't burn an HTTP
// call per fetch). Manual uses the values typed by the user via the city
// entry flow / GMT cycler. Defaults to Auto for both — convenient for
// residential ISP users, overridable for anyone on Starlink/CGNAT/VPN
// where IP geolocation misreports the city.
enum class LocationMode : uint8_t {
  Auto = 0,    // ← default
  Manual = 1,
};

enum class GmtMode : uint8_t {
  Auto = 0,    // ← default
  Manual = 1,
};

// TTL for the cached ip-api response. After this many seconds the next
// refresh will hit ip-api again; until then the cached lat/lon/city/offset
// are reused. 24 h matches our once-per-day weather cache rhythm.
inline constexpr int kAutoLocationCacheTtlSec = 24 * 60 * 60;

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

  // Display format for dates rendered by the Todoist activity.
  DateFormat getDateFormat() const { return dateFormat; }

  // Overall layout selection. See DesignMode.
  DesignMode getDesignMode() const { return designMode; }

  // Toggle: render Todoist snapshot on sleep when available.
  bool isSleepScreenEnabled() const { return sleepScreenEnabled; }

  // Active-view orientation (when Todoist activity is open).
  GfxRenderer::Orientation getActivityOrientation() const { return activityOrientation; }

  // Sleep-screen snapshot orientation.
  GfxRenderer::Orientation getSnapshotOrientation() const { return snapshotOrientation; }

  // Manually-entered location (user types a city, we geocode via Open-Meteo
  // and persist lat/lon/name here). hasLocation() keys off the name being
  // empty, not the coordinates, because (0,0) is a legitimate point off the
  // African coast and we shouldn't silently re-geolocate a user who happens
  // to be near it. Lat/lon are stored as doubles to preserve enough
  // precision for forecast accuracy (~0.0001° = ~11 m).
  //
  // These are the *manual* values regardless of the active LocationMode —
  // they survive a switch to Auto so the user can flip back without
  // retyping. Use getEffective* if you want the active values.
  double getLatitude() const { return latitude; }
  double getLongitude() const { return longitude; }
  const std::string& getLocationName() const { return locationName; }
  bool hasLocation() const { return !locationName.empty(); }

  // Auto-detected location, cached from the last successful ip-api.com
  // lookup. Empty `autoLocationName` == no cache yet. autoGmtOffsetSeconds
  // preserves ip-api's full-precision offset (e.g. 19800 for IST = +05:30)
  // rather than rounding to whole hours like the manual cycler does.
  // autoFetchedAt is a unix epoch; older than kAutoLocationCacheTtlSec ago
  // == stale, and the next refresh re-fetches.
  double getAutoLatitude() const { return autoLatitude; }
  double getAutoLongitude() const { return autoLongitude; }
  const std::string& getAutoLocationName() const { return autoLocationName; }
  int getAutoGmtOffsetSeconds() const { return autoGmtOffsetSeconds; }
  time_t getAutoFetchedAt() const { return autoFetchedAt; }
  bool hasAutoLocation() const { return !autoLocationName.empty(); }
  // True when the cache is older than the TTL OR the cache is empty.
  // Either condition means the next fetch path should hit ip-api.
  bool isAutoLocationStale(time_t now) const;

  // Active source-of-truth selection. Default Auto for both — IP-based
  // geolocation is convenient and accurate on residential networks. User
  // can flip either to Manual via Settings → Todoist if their IP misreports
  // (Starlink, CGNAT, mobile, VPN).
  LocationMode getLocationMode() const { return locationMode; }
  GmtMode getGmtMode() const { return gmtMode; }

  // Resolved values that downstream code (weather fetch, applyTimezone,
  // renderer) should consume. Auto path returns the cached ip-api values
  // (zero/empty if no cache yet); Manual path returns the typed values.
  double getEffectiveLatitude() const;
  double getEffectiveLongitude() const;
  // Returns a reference; one of the underlying std::string members.
  // Caller may copy if it needs lifetime beyond the next mutation.
  const std::string& getEffectiveLocationName() const;
  // Effective offset *in seconds east of UTC* — covers half-hour zones
  // when Auto provides them. Manual values are hour-precision and
  // multiplied by 3600 here.
  int getEffectiveGmtOffsetSeconds() const;
  bool hasEffectiveLocation() const;

  // Display unit for the Daily weather row. Sent verbatim to Open-Meteo's
  // `temperature_unit` query parameter and used to pick the trailing
  // suffix ('C' or 'F') on screen.
  weather::TemperatureUnit getTemperatureUnit() const { return temperatureUnit; }

  // Cached forecast (most recent successful fetch). The cache is keyed by
  // the calendar date in the user's configured timezone: a refresh that
  // happens later on the same date reuses these values instead of burning
  // two more HTTPS round-trips. Cleared on location change, unit change,
  // and Forget. `cachedWeatherDate` empty == no cache.
  const std::string& getCachedWeatherDate() const { return cachedWeatherDate; }
  uint8_t getCachedWeatherWmo() const { return cachedWeatherWmo; }
  int16_t getCachedWeatherHi() const { return cachedWeatherHi; }
  int16_t getCachedWeatherLo() const { return cachedWeatherLo; }

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
  bool setDateFormat(DateFormat f);
  bool setDesignMode(DesignMode d);
  bool setTemperatureUnit(weather::TemperatureUnit u);

  // Mode setters. Changing either invalidates the cached weather because
  // the *effective* location/timezone may have just shifted under us
  // (e.g. switching from Manual Lisbon to Auto-detected São Paulo).
  bool setLocationMode(LocationMode m);
  bool setGmtMode(GmtMode m);

  // Update all three manual location fields atomically. Called by the
  // city-entry flow in TodoistSettingsActivity after a successful
  // Open-Meteo geocode. Persists once at the end so a partial geocode
  // can't leave the JSON in a mixed state.
  bool setLocation(double lat, double lon, const char* cityUtf8);

  // Persist the result of an ip-api.com fetch. fetchedAt is a unix epoch
  // (typically `time(nullptr)` from the caller). Invalidates the cached
  // weather because the auto coords may have moved.
  bool setAutoLocation(double lat, double lon, const char* cityUtf8,
                       int offsetSeconds, time_t fetchedAt);

  // Reset the *manual* location to "not set". The auto cache is unaffected.
  // Invalidates the cached weather.
  bool clearLocation();

  // Reset the *auto* cache so the next refresh re-detects via ip-api.
  // Used by the "Clear cached location" settings row. Also invalidates
  // the cached weather (different place = different forecast).
  bool clearAutoCache();

  // Write a fresh forecast to the cache. `todayYmd` must be 10 chars
  // ("YYYY-MM-DD"). Persists in the same JSON file as the rest of the
  // config — one extra SD write per day per location.
  bool setCachedWeather(const char* todayYmd, uint8_t wmo, int hi, int lo);

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
  DateFormat dateFormat = DateFormat::DayMonthSlash;
  DesignMode designMode = DesignMode::Minimal;
  double latitude = 0.0;
  double longitude = 0.0;
  std::string locationName;
  weather::TemperatureUnit temperatureUnit = weather::TemperatureUnit::Celsius;
  // Mode selectors — Auto by default so a fresh install Just Works on
  // residential WiFi. Existing configs without these keys also default to
  // Auto (intentional: forces every install onto the new default; user
  // can flip to Manual via settings if their IP misreports).
  LocationMode locationMode = LocationMode::Auto;
  GmtMode gmtMode = GmtMode::Auto;
  // Auto cache — populated by ip-api.com fetches. Zero/empty until first
  // successful fetch. autoGmtOffsetSeconds keeps the raw seconds value
  // from ip-api so half-hour zones (IST, NPT) survive the round-trip.
  double autoLatitude = 0.0;
  double autoLongitude = 0.0;
  std::string autoLocationName;
  int autoGmtOffsetSeconds = 0;
  time_t autoFetchedAt = 0;
  // Cached forecast. cachedWeatherDate empty == no cache.
  std::string cachedWeatherDate;
  uint8_t cachedWeatherWmo = 0;
  int16_t cachedWeatherHi = 0;
  int16_t cachedWeatherLo = 0;
  bool loaded = false;
};

#define TODOIST_CONFIG todoist::TodoistConfig::getInstance()

}  // namespace todoist
