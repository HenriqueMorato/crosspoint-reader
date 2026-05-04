# Todoist Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only Todoist "Today" view to the CrossPoint Reader, with optional sleep-screen integration backed by an SD-card-only configuration.

**Architecture:** All Todoist state lives on the SD card under `/.crosspoint/`. A `TodoistConfig` singleton owns load/save with atomic temp-file-and-rename writes. A stateless `TodoistClient` performs HTTPS GETs against `api.todoist.com` using `esp_crt_bundle_attach` (the existing Mozilla CA bundle pattern from `KOReaderSyncClient`). A `TodoistActivity` brings WiFi up, fetches once on entry, renders, captures a snapshot via `ScreenshotUtil::saveFramebufferAsBmp`, and drops WiFi. `SleepActivity` gains a pre-check that blits the Todoist snapshot when the toggle is on and the snapshot orientation matches the configured snapshot orientation. SPIFFS / `CrossPointSettings` is **not** modified.

**Tech Stack:** ESP32-C3 (ESP-IDF + Arduino), C++20 no-RTTI no-exceptions, ArduinoJson 7.4.2, `esp_http_client` + `esp_crt_bundle_attach`, ESP-IDF SNTP, existing UITheme/HalStorage/HalDisplay abstractions.

**Spec:** `docs/superpowers/specs/2026-05-04-todoist-integration-design.md`

**Invariants enforced across phases:**
- All user-facing strings via `tr(STR_TODOIST_*)`. No hardcoded UI strings.
- All logging via `LOG_DBG`/`LOG_ERR` with module tag `"TDST"`.
- All UI rendering via the `GUI` macro (UITheme).
- All file I/O via `Storage` (HalStorage). Never use SdFat directly.
- No SPIFFS schema changes. No additions to `CrossPointSettings`.
- Reserve all `std::vector` capacity before `push_back`. No heap growth in render loops.
- `string_view`s never crossed into C APIs without explicit null-termination.

**Verification model (firmware project — no unit test harness):** Each task ends in a `pio run` build. Phase-end smoke tests are run manually on hardware (Xteink X4) and reported by the user. Build environments verified at the end of every phase: `default`, `gh_release`, `slim`.

---

## File Structure

**New files (5 created total across the project):**
```
src/integrations/todoist/TodoistConfig.h          Phase 1
src/integrations/todoist/TodoistConfig.cpp        Phase 1
src/integrations/todoist/TodoistTask.h            Phase 1
src/integrations/todoist/TodoistClient.h          Phase 2
src/integrations/todoist/TodoistClient.cpp        Phase 2
src/activities/integrations/TodoistActivity.h     Phase 3
src/activities/integrations/TodoistActivity.cpp   Phase 3
src/activities/settings/TodoistSettingsActivity.h   Phase 5
src/activities/settings/TodoistSettingsActivity.cpp Phase 5
```

**Files modified:**
```
src/main.cpp                                    Phase 1 (boot-time config load)
lib/I18n/translations/english.yaml              Phase 1 (STR_TODOIST_*)
src/activities/home/HomeActivity.h              Phase 3 (menu entry)
src/activities/home/HomeActivity.cpp            Phase 3 (menu entry + dispatch)
src/activities/boot_sleep/SleepActivity.cpp     Phase 4 (pre-check)
src/activities/settings/SettingsActivity.h      Phase 5 (Todoist submenu entry)
src/activities/settings/SettingsActivity.cpp    Phase 5 (Todoist submenu entry)
```

**Phase file budgets:**
- Phase 1: 5 files (3 created, 2 modified)
- Phase 2: 2 files (created)
- Phase 3: 4 files (2 created, 2 modified)
- Phase 4: 1 file (modified)
- Phase 5: 4 files (2 created, 2 modified) — submenu split out so SettingsActivity changes stay minimal

---

## Phase 1 — Config Foundation

**Scope:** `TodoistConfig` singleton, `TodoistTask` POD, i18n string additions, boot-time load. No network, no rendering, no UI yet.

**Why first:** Every other phase depends on `TodoistConfig` reads, `TodoistTask` shape, and translated strings. We land this phase on its own and verify a clean build before touching network code.

### Task 1.1: Add i18n strings

**Files:**
- Modify: `lib/I18n/translations/english.yaml`

- [ ] **Step 1: Append Todoist strings to English YAML**

Open `lib/I18n/translations/english.yaml`. Find a suitable location near the end of the file (after existing setting strings, before the closing of the file — the file is a flat top-level mapping). Add the following block:

```yaml
STR_TODOIST: "Todoist"
STR_TODOIST_FETCHING: "Fetching tasks..."
STR_TODOIST_NO_TOKEN: "No Todoist token configured"
STR_TODOIST_NO_SD: "SD card unavailable"
STR_TODOIST_OFFLINE: "WiFi unavailable"
STR_TODOIST_FETCH_FAILED: "Could not fetch tasks"
STR_TODOIST_NO_TASKS: "All clear today"
STR_TODOIST_SLEEP_SCREEN: "Todoist sleep screen"
STR_TODOIST_ACTIVITY_ORIENTATION: "Activity orientation"
STR_TODOIST_SNAPSHOT_ORIENTATION: "Sleep screen orientation"
STR_TODOIST_FORGET: "Forget Todoist"
STR_TODOIST_TODAY_HEADER: "Today - Updated %02d:%02d"
STR_TODOIST_REFRESHING: "Refreshing..."
STR_TODOIST_INVALID_TOKEN: "Invalid token"
STR_TODOIST_RATE_LIMITED: "Rate limited - try later"
```

- [ ] **Step 2: Regenerate i18n headers**

Run: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`

Expected: command exits 0; files `lib/I18n/I18nKeys.h`, `lib/I18n/I18nStrings.h`, `lib/I18n/I18nStrings.cpp` updated. Use `git status` to confirm. (These files are gitignored — they will not be committed but must exist for the build.)

- [ ] **Step 3: Verify build**

Run: `pio run`

Expected: PASS, 0 errors. (No code uses these strings yet, but the generator must succeed.)

- [ ] **Step 4: Commit**

```bash
git add lib/I18n/translations/english.yaml
git commit -m "$(cat <<'EOF'
feat: add Todoist i18n strings

Adds STR_TODOIST_* keys for the upcoming Todoist integration. Generated
headers (I18nKeys.h, I18nStrings.{h,cpp}) are regenerated at build time
and remain gitignored.
EOF
)"
```

---

### Task 1.2: Create `TodoistTask` POD

**Files:**
- Create: `src/integrations/todoist/TodoistTask.h`

- [ ] **Step 1: Create header**

Create directory if needed: `mkdir -p src/integrations/todoist`

Write `src/integrations/todoist/TodoistTask.h`:

```cpp
#pragma once

#include <cstdint>

namespace todoist {

// Fixed-size POD so a std::vector<TodoistTask> reserves a contiguous block
// without any per-task heap allocation. Title is hard-truncated; the active
// view ellipsises further when rendering.
struct TodoistTask {
  static constexpr size_t kTitleCapacity = 96;
  static constexpr size_t kDueTimeCapacity = 6;  // "HH:MM\0"

  char title[kTitleCapacity];
  char dueTime[kDueTimeCapacity];  // empty string if no time
  uint8_t priority;                // 1 (lowest) to 4 (highest), 0 = unknown
  bool overdue;
};

}  // namespace todoist
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS. (Header is unused; we are confirming it parses.)

- [ ] **Step 3: Commit**

```bash
git add src/integrations/todoist/TodoistTask.h
git commit -m "feat: add TodoistTask POD"
```

---

### Task 1.3: Create `TodoistConfig` header

**Files:**
- Create: `src/integrations/todoist/TodoistConfig.h`

- [ ] **Step 1: Write header**

Write `src/integrations/todoist/TodoistConfig.h`:

```cpp
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
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS. (Still no callers; we are confirming declarations parse.)

- [ ] **Step 3: Commit**

```bash
git add src/integrations/todoist/TodoistConfig.h
git commit -m "feat: add TodoistConfig singleton header"
```

---

### Task 1.4: Implement `TodoistConfig`

**Files:**
- Create: `src/integrations/todoist/TodoistConfig.cpp`

- [ ] **Step 1: Write implementation**

Write `src/integrations/todoist/TodoistConfig.cpp`:

```cpp
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
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS, 0 errors.

If `Storage.openFileForRead`/`openFileForWrite` signatures differ (e.g., `HalFile` is named differently), inspect `lib/hal/HalStorage.h` and adjust includes/types. Do NOT change the HAL — adjust the caller.

- [ ] **Step 3: Commit**

```bash
git add src/integrations/todoist/TodoistConfig.cpp
git commit -m "$(cat <<'EOF'
feat: implement TodoistConfig persistence

Atomic writes via temp file + rename. Bounded 4KB read for the JSON
file. ArduinoJson handles parse/serialize. All paths under /.crosspoint/
on the SD card; no SPIFFS access.
EOF
)"
```

---

### Task 1.5: Boot-time config load

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Confirm insertion point**

Open `src/main.cpp`. The exact insertion point is **immediately after** `SETTINGS.loadFromFile();` (around line 261). Existing context, in order:

```cpp
  if (!Storage.begin()) { /* ... */ }
  HalSystem::checkPanic();

  SETTINGS.loadFromFile();                                  // <-- after THIS line
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
```

- [ ] **Step 2: Add include and load call**

Add at the top of `src/main.cpp` next to the `KOREADER_STORE` / `OPDS_STORE` includes:

```cpp
#include "integrations/todoist/TodoistConfig.h"
```

Insert exactly one line, immediately after `SETTINGS.loadFromFile();`:

```cpp
  todoist::TodoistConfig::getInstance().load();
```

(Use the `TODOIST_CONFIG` macro directly is fine too: `TODOIST_CONFIG.load();`. The macro expands to `todoist::TodoistConfig::getInstance()`.)

- [ ] **Step 3: Verify build**

Run: `pio run`

Expected: PASS, 0 errors.

- [ ] **Step 4: Verify all release environments build**

Run: `pio run -e gh_release && pio run -e slim`

Expected: both PASS.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat: load TodoistConfig at boot"
```

---

### Phase 1 Smoke Test (manual, on hardware)

- [ ] Flash device (`pio run -t upload`).
- [ ] On a paired SD card, place a hand-written `/.crosspoint/todoist.json`:
  ```json
  {
    "api_token": "this-is-a-fake-token-for-testing-twenty-chars",
    "sleep_screen_enabled": false,
    "activity_orientation": "portrait",
    "snapshot_orientation": "landscape_cw"
  }
  ```
- [ ] Boot device with serial monitor. Look for `LOG_DBG("TDST", "Config loaded ...")`.
- [ ] Boot with no `todoist.json` present. Look for `LOG_DBG("TDST", "No config at ...")`. No errors.
- [ ] Boot with malformed JSON (drop a `,`). Look for `LOG_ERR("TDST", "JSON parse error...")`. No crash.

If all four pass, Phase 1 is complete.

---

## Phase 2 — API Client

**Scope:** `TodoistClient` HTTPS GET against `api.todoist.com` and JSON parse into `std::vector<TodoistTask>`. Stateless, no UI, no WiFi management. Caller (Phase 3) is responsible for bringing WiFi up.

### Task 2.1: Create `TodoistClient` header

**Files:**
- Create: `src/integrations/todoist/TodoistClient.h`

- [ ] **Step 1: Write header**

Write `src/integrations/todoist/TodoistClient.h`:

```cpp
#pragma once

#include "TodoistTask.h"

#include <string>
#include <vector>

namespace todoist {

enum class FetchResult {
  Ok,
  InvalidToken,    // 401/403
  RateLimited,     // 429
  NetworkError,    // timeout, TLS handshake, transport
  ServerError,     // 5xx
  ParseError,      // bad/unexpected JSON
};

class TodoistClient {
 public:
  // Synchronous fetch. Caller must guarantee WiFi is up. Reserves out
  // capacity to a sane upper bound; truncates extras silently.
  // Always returns; never blocks indefinitely (15s HTTP timeout).
  static FetchResult fetchToday(const std::string& apiToken,
                                std::vector<TodoistTask>& outTasks);
};

}  // namespace todoist
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/integrations/todoist/TodoistClient.h
git commit -m "feat: add TodoistClient header"
```

---

### Task 2.2: Implement `TodoistClient::fetchToday`

**Files:**
- Create: `src/integrations/todoist/TodoistClient.cpp`

- [ ] **Step 1: Write implementation**

Write `src/integrations/todoist/TodoistClient.cpp`:

```cpp
#include "TodoistClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstdio>
#include <cstring>

namespace todoist {

namespace {

constexpr const char* kEndpoint = "https://api.todoist.com/rest/v2/tasks?filter=today";
constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kHttpBufSize = 4096;
constexpr size_t kMaxTasks = 64;
constexpr size_t kMaxResponseBytes = 64 * 1024;  // hard cap

struct ResponseBuffer {
  std::string body;
  bool truncated = false;
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (!buf || !evt->data || evt->data_len <= 0) return ESP_OK;
  if (buf->body.size() + static_cast<size_t>(evt->data_len) > kMaxResponseBytes) {
    buf->truncated = true;
    return ESP_OK;
  }
  buf->body.append(static_cast<const char*>(evt->data), static_cast<size_t>(evt->data_len));
  return ESP_OK;
}

// Extracts HH:MM from a full ISO datetime ("YYYY-MM-DDTHH:MM:SS...") or
// from a date-only ("YYYY-MM-DD"). Writes to out (size kDueTimeCapacity).
// Empty out means "no time-of-day".
void extractDueTime(const char* due, char* out, size_t outCap) {
  if (outCap == 0) return;
  out[0] = '\0';
  if (!due) return;
  const char* tPos = strchr(due, 'T');
  if (!tPos) return;  // date-only, no time
  // Want 5 chars after T: "HH:MM"
  if (strlen(tPos + 1) < 5) return;
  if (outCap < 6) return;
  snprintf(out, outCap, "%c%c:%c%c",
           tPos[1], tPos[2], tPos[4], tPos[5]);
}

// Returns true if the due date is strictly before today's date in the
// device's current timezone. We compare via time_t to avoid string parsing
// of timezone offsets. If parsing fails, returns false (not overdue).
bool isOverdue(const char* due) {
  if (!due) return false;
  int y, mo, d;
  if (sscanf(due, "%4d-%2d-%2d", &y, &mo, &d) != 3) return false;

  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // clock not set yet
  struct tm nowTm;
  localtime_r(&now, &nowTm);

  if (y < nowTm.tm_year + 1900) return true;
  if (y > nowTm.tm_year + 1900) return false;
  if (mo < nowTm.tm_mon + 1) return true;
  if (mo > nowTm.tm_mon + 1) return false;
  return d < nowTm.tm_mday;
}

void copyTitle(char* dst, size_t dstCap, const char* src) {
  if (dstCap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= dstCap) n = dstCap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

FetchResult httpStatusToFetchResult(int code) {
  if (code == 200) return FetchResult::Ok;
  if (code == 401 || code == 403) return FetchResult::InvalidToken;
  if (code == 429) return FetchResult::RateLimited;
  if (code >= 500) return FetchResult::ServerError;
  return FetchResult::NetworkError;
}

}  // namespace

FetchResult TodoistClient::fetchToday(const std::string& apiToken,
                                      std::vector<TodoistTask>& outTasks) {
  outTasks.clear();
  outTasks.reserve(kMaxTasks);

  if (apiToken.empty()) {
    LOG_ERR("TDST", "Empty token");
    return FetchResult::InvalidToken;
  }

  ResponseBuffer buf;
  buf.body.reserve(8192);

  esp_http_client_config_t config = {};
  config.url = kEndpoint;
  config.event_handler = httpEventHandler;
  config.user_data = &buf;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kHttpBufSize;
  config.buffer_size_tx = kHttpBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("TDST", "esp_http_client_init failed");
    return FetchResult::NetworkError;
  }

  std::string authHeader = "Bearer " + apiToken;
  if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    LOG_ERR("TDST", "Set header failed");
    esp_http_client_cleanup(client);
    return FetchResult::NetworkError;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("TDST", "HTTP %d (err=%d, %u bytes)",
          httpCode, err, static_cast<unsigned>(buf.body.size()));

  if (err != ESP_OK) return FetchResult::NetworkError;
  FetchResult statusResult = httpStatusToFetchResult(httpCode);
  if (statusResult != FetchResult::Ok) return statusResult;

  if (buf.truncated) {
    LOG_ERR("TDST", "Response truncated at cap");
    // Not fatal — try to parse what we have. Worst case ParseError below.
  }

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, buf.body);
  if (parseErr) {
    LOG_ERR("TDST", "JSON parse: %s", parseErr.c_str());
    return FetchResult::ParseError;
  }
  if (!doc.is<JsonArray>()) {
    LOG_ERR("TDST", "Response is not a JSON array");
    return FetchResult::ParseError;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject task : arr) {
    if (outTasks.size() >= kMaxTasks) break;
    TodoistTask t = {};
    copyTitle(t.title, TodoistTask::kTitleCapacity, task["content"] | "");
    t.priority = static_cast<uint8_t>(task["priority"] | 1);

    // Todoist `due` may be null, an object with `date` and optional
    // `datetime`. Prefer `datetime` for time-of-day extraction.
    const char* date = nullptr;
    const char* datetime = nullptr;
    if (task["due"].is<JsonObject>()) {
      JsonObject due = task["due"].as<JsonObject>();
      date = due["date"] | static_cast<const char*>(nullptr);
      datetime = due["datetime"] | static_cast<const char*>(nullptr);
    }
    extractDueTime(datetime, t.dueTime, TodoistTask::kDueTimeCapacity);
    t.overdue = isOverdue(date);

    outTasks.push_back(t);
  }

  LOG_DBG("TDST", "Parsed %u tasks", static_cast<unsigned>(outTasks.size()));
  return FetchResult::Ok;
}

}  // namespace todoist
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS, 0 errors. The explicit `#include <esp_crt_bundle.h>` is the same one `lib/KOReaderSync/KOReaderSyncClient.cpp:6` uses — keep it as-is.

- [ ] **Step 3: Verify gh_release & slim builds**

Run: `pio run -e gh_release && pio run -e slim`

Expected: both PASS.

- [ ] **Step 4: Commit**

```bash
git add src/integrations/todoist/TodoistClient.cpp
git commit -m "$(cat <<'EOF'
feat: implement TodoistClient.fetchToday

Stateless HTTPS GET against api.todoist.com using esp_crt_bundle_attach
for TLS, the same pattern as KOReaderSyncClient. Streams the response
into a bounded buffer (64KB cap), parses with ArduinoJson, fills a
caller-provided vector reserved to 64 tasks. Maps HTTP statuses to a
FetchResult enum so the activity layer can render a precise error.
EOF
)"
```

---

### Phase 2 Smoke Test

There is no UI yet, so direct runtime verification waits until Phase 3. Build-only verification this phase:

- [ ] `pio run -t clean && pio run` — clean build PASS.
- [ ] `pio check` — no new high/medium issues introduced.
- [ ] Code review: confirm `esp_crt_bundle_attach` usage matches `KOReaderSyncClient.cpp`.

---

## Phase 3 — Activity & Home-Screen Entry

**Scope:** `TodoistActivity` (renders the task list, scrolls, refresh on Confirm), home screen menu item.

**Note:** Snapshot capture is deferred to Phase 4 — Phase 3's activity renders to the screen but does not yet write a BMP. This keeps the file budget at 4 and lets us verify network-driven rendering in isolation.

### Task 3.1: Create `TodoistActivity` header

**Files:**
- Create: `src/activities/integrations/TodoistActivity.h`

- [ ] **Step 1: Write header**

Create directory if needed: `mkdir -p src/activities/integrations`

Write `src/activities/integrations/TodoistActivity.h`:

```cpp
#pragma once

#include "Activity.h"
#include "MappedInputManager.h"
#include "integrations/todoist/TodoistClient.h"
#include "integrations/todoist/TodoistTask.h"
#include "util/ButtonNavigator.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

class TodoistActivity : public Activity {
 public:
  TodoistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  enum class State {
    Loading,        // initial fetch in progress / awaiting WiFi
    ShowingTasks,
    ShowingError,
  };

  // Step 1 of fetch: ensure WiFi. If already connected, calls
  // proceedWithFetch() directly. Otherwise launches WifiSelectionActivity
  // and re-enters proceedWithFetch() on its result.
  void startFetch();

  // Step 2 of fetch: NTP + TodoistClient::fetchToday + populate state.
  // Called once WiFi is up.
  void proceedWithFetch();

  // Maps a FetchResult to the corresponding StrId for an error message.
  StrId fetchResultToStrId(todoist::FetchResult r) const;

  void renderLoading();
  void renderError();
  void renderTaskList();

  std::vector<todoist::TodoistTask> _tasks;
  State _state = State::Loading;
  int _scrollOffset = 0;
  StrId _errorStrId = StrId::STR_TODOIST_FETCH_FAILED;
  uint8_t _capturedHour = 0;
  uint8_t _capturedMin = 0;
  ButtonNavigator _navigator;
  bool _navigatorBound = false;
};
```

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS. (Header has no body yet; we are confirming declarations parse.)

- [ ] **Step 3: Commit**

```bash
git add src/activities/integrations/TodoistActivity.h
git commit -m "feat: add TodoistActivity header"
```

---

### Task 3.2: Implement `TodoistActivity` lifecycle and fetch

**Files:**
- Create: `src/activities/integrations/TodoistActivity.cpp`

- [ ] **Step 1: Write implementation**

Write `src/activities/integrations/TodoistActivity.cpp`. The async WiFi pattern mirrors `src/activities/reader/KOReaderSyncActivity.cpp:219-239` exactly — there is **no** synchronous `connectToSavedWifi()` helper in this codebase; `WifiSelectionActivity` via `startActivityForResult` is the only path.

The UITheme API is fixed: use `GUI.drawPopup(renderer, msg)` for centered messages (same call SleepActivity uses for "Going to sleep"), `GUI.drawHeader(renderer, rect, title, ...)` for the top header, and `GUI.drawList(renderer, rect, count, selectedIndex, titleLambda, subtitleLambda, iconLambda, valueLambda, highlightValue)` for the rows. There is no `drawCenteredText`, no `drawHeading`, no `drawListRow`, no `getListRowHeight`.

```cpp
#include "TodoistActivity.h"

#include "Logging.h"
#include "UITheme.h"
#include "activities/network/WifiSelectionActivity.h"
#include "integrations/todoist/TodoistConfig.h"

#include <WiFi.h>
#include <esp_sntp.h>

#include <ctime>
#include <cstdio>
#include <cstring>

namespace {

void syncTimeWithNTP() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < 50) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ++retry;
  }
  if (retry >= 50) LOG_DBG("TDST", "NTP timeout (using fallback)");
}

}  // namespace

void TodoistActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(TODOIST_CONFIG.getActivityOrientation());

  _state = State::Loading;
  _scrollOffset = 0;
  _tasks.clear();

  // Bind navigator scroll callbacks once.
  if (!_navigatorBound) {
    _navigator.onNext([this] {
      if (_state != State::ShowingTasks) return;
      if (_scrollOffset + 1 < static_cast<int>(_tasks.size())) {
        ++_scrollOffset;
        requestUpdate();
      }
    });
    _navigator.onPrevious([this] {
      if (_state != State::ShowingTasks) return;
      if (_scrollOffset > 0) {
        --_scrollOffset;
        requestUpdate();
      }
    });
    _navigatorBound = true;
  }

  requestUpdate(true);  // paint Loading screen
  startFetch();
}

void TodoistActivity::onExit() {
  Activity::onExit();
}

void TodoistActivity::loop() {
  mappedInput.update();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Confirm = Refresh (works in both ShowingTasks and ShowingError).
  if ((_state == State::ShowingTasks || _state == State::ShowingError) &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    _state = State::Loading;
    _scrollOffset = 0;
    _tasks.clear();
    requestUpdate(true);
    startFetch();
    return;
  }

  // Up/Down for scroll handled by the navigator (auto-repeat).
}

void TodoistActivity::startFetch() {
  if (!TODOIST_CONFIG.hasValidToken()) {
    _state = State::ShowingError;
    _errorStrId = StrId::STR_TODOIST_NO_TOKEN;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("TDST", "Already connected to WiFi");
    proceedWithFetch();
    return;
  }

  LOG_DBG("TDST", "Launching WifiSelectionActivity");
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& result) {
        if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
          _state = State::ShowingError;
          _errorStrId = StrId::STR_TODOIST_OFFLINE;
          requestUpdate();
          return;
        }
        // After WifiSelectionActivity returns, our orientation may have been
        // clobbered by its own UI. Re-apply the configured activity orient.
        renderer.setOrientation(TODOIST_CONFIG.getActivityOrientation());
        proceedWithFetch();
      });
}

void TodoistActivity::proceedWithFetch() {
  syncTimeWithNTP();

  using todoist::FetchResult;
  FetchResult r = todoist::TodoistClient::fetchToday(TODOIST_CONFIG.getApiToken(), _tasks);

  if (r == FetchResult::Ok) {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    _capturedHour = static_cast<uint8_t>(tm_now.tm_hour);
    _capturedMin = static_cast<uint8_t>(tm_now.tm_min);
    _state = State::ShowingTasks;
    _scrollOffset = 0;
  } else {
    _state = State::ShowingError;
    _errorStrId = fetchResultToStrId(r);
  }
  requestUpdate(true);
}

StrId TodoistActivity::fetchResultToStrId(todoist::FetchResult r) const {
  using todoist::FetchResult;
  switch (r) {
    case FetchResult::InvalidToken: return StrId::STR_TODOIST_INVALID_TOKEN;
    case FetchResult::RateLimited:  return StrId::STR_TODOIST_RATE_LIMITED;
    case FetchResult::Ok:
    case FetchResult::ServerError:
    case FetchResult::NetworkError:
    case FetchResult::ParseError:
    default:                        return StrId::STR_TODOIST_FETCH_FAILED;
  }
}

void TodoistActivity::render(RenderLock&&) {
  switch (_state) {
    case State::Loading:        renderLoading(); break;
    case State::ShowingError:   renderError();   break;
    case State::ShowingTasks:   renderTaskList(); break;
  }
  renderer.displayBuffer();
}

void TodoistActivity::renderLoading() {
  renderer.clearBuffer();
  GUI.drawPopup(renderer, tr(STR_TODOIST_FETCHING));
}

void TodoistActivity::renderError() {
  renderer.clearBuffer();
  GUI.drawPopup(renderer, I18N.get(_errorStrId));
}

void TodoistActivity::renderTaskList() {
  renderer.clearBuffer();

  const int pageWidth = renderer.getDisplayWidth();
  const int pageHeight = renderer.getDisplayHeight();
  const auto& metrics = GUI.getMetrics();  // BaseTheme exposes layout metrics

  // Header
  char header[64];
  snprintf(header, sizeof(header), I18N.get(StrId::STR_TODOIST_TODAY_HEADER),
           _capturedHour, _capturedMin);

  GUI.drawHeader(renderer, Rect{0, 0, pageWidth, metrics.headerHeight}, header);

  if (_tasks.empty()) {
    GUI.drawPopup(renderer, tr(STR_TODOIST_NO_TASKS));
    return;
  }

  const int contentTop = metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight},
      static_cast<int>(_tasks.size()),
      _scrollOffset,  // selected index used as scroll anchor
      // Title lambda: "[!] HH:MM Title"
      [this](int index) -> std::string {
        const auto& t = _tasks[index];
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s%s%s",
                 t.overdue ? "[!] " : "",
                 t.dueTime[0] ? t.dueTime : "",
                 t.dueTime[0] ? "  " : "",
                 t.title);
        return std::string(buf);
      },
      nullptr,  // subtitle
      nullptr,  // icon
      // Value lambda: priority indicator
      [this](int index) -> std::string {
        char buf[8];
        snprintf(buf, sizeof(buf), "p%u",
                 static_cast<unsigned>(_tasks[index].priority));
        return std::string(buf);
      },
      false);
}
```

**Notes for the implementer:**

- `Rect`, `metrics`, and `BaseTheme::getMetrics()` access patterns mirror what `HomeActivity.cpp:228-252` and `KOReaderSettingsActivity.cpp:121-143` already do. If `getMetrics()` is exposed under a different name, look at how those two files obtain layout dimensions and copy that exact incantation.
- `I18N` is the macro defined in `lib/I18n/I18n.h:39` (`#define I18N I18n::getInstance()`). `tr(STR_X)` works for static strings; for variable string IDs use `I18N.get(strId)`.
- The `_navigator` member is updated by the framework (it was registered globally via `ButtonNavigator::setMappedInputManager(mappedInputManager)` in `main.cpp` at boot). No explicit `update()` call needed in `loop()` — the global manager drives it.
- `WifiSelectionActivity` may change the renderer's orientation; we re-apply ours in the callback before `proceedWithFetch()`.

- [ ] **Step 2: Verify build**

Run: `pio run`

Expected: PASS, 0 errors. Fix any UITheme method mismatches by referring to actual UITheme.h. Fix WiFi connect calls by mirroring KOReaderSyncActivity.

- [ ] **Step 3: Commit**

```bash
git add src/activities/integrations/TodoistActivity.cpp
git commit -m "$(cat <<'EOF'
feat: implement TodoistActivity (fetch + render + scroll)

WiFi up + NTP sync + TodoistClient::fetchToday in onEnter; render task
list with header timestamp; Up/Down scroll; Confirm refreshes; Back
exits. Snapshot capture is deferred to phase 4.
EOF
)"
```

---

### Task 3.3: Add Todoist menu entry to HomeActivity

**Files:**
- Modify: `src/activities/home/HomeActivity.h`
- Modify: `src/activities/home/HomeActivity.cpp`

**Verified mechanics:** `HomeActivity` builds a `std::vector<const char*> menuItems` and `std::vector<UIIcon> menuIcons` (see `HomeActivity.cpp:228-252`), then passes both via lambdas to `GUI.drawButtonMenu`. There is dynamic index logic for OPDS / Continue Reading. We add Todoist at a fixed position (after Settings) and update three places: `getMenuItemCount`, the `Confirm` dispatch, and the `menuItems` / `menuIcons` vector building.

- [ ] **Step 1: Add handler declaration in header**

Open `src/activities/home/HomeActivity.h`. Locate the private `onXxxOpen()` handler declarations (e.g., `onOpdsLibraryOpen()`, `onSettingsOpen()`). Add:

```cpp
void onTodoistOpen();
```

- [ ] **Step 2: Update menu count**

Open `src/activities/home/HomeActivity.cpp`. Locate `getMenuItemCount()` (around lines 23-32). Bump the static count by 1:

```cpp
int HomeActivity::getMenuItemCount() const {
  int count = 5;  // File Browser, Recents, File transfer, Settings, Todoist
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}
```

- [ ] **Step 3: Update dispatch in Confirm handler**

Locate the `wasReleased(MappedInputManager::Button::Confirm)` block (around lines 187-210) where `idx` is incremented and `settingsIdx` is computed. Add `todoistIdx` after it:

```cpp
const int settingsIdx = idx++;
const int todoistIdx = idx;
// ... existing if/else chain ...
} else if (menuSelectedIndex == settingsIdx) {
  onSettingsOpen();
} else if (menuSelectedIndex == todoistIdx) {
  onTodoistOpen();
}
```

- [ ] **Step 4: Add label + icon to the menu vectors**

Locate the menu item construction (around `HomeActivity.cpp:228-238`):

```cpp
std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS),
                                      tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE)};
std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};
```

Append Todoist at the end of both vectors:

```cpp
std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS),
                                      tr(STR_FILE_TRANSFER), tr(STR_SETTINGS_TITLE),
                                      tr(STR_TODOIST)};
std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings, Settings};
```

(`UIIcon::Settings` is reused for now — there is no Todoist icon. If you'd rather suppress the icon, check whether `drawButtonMenu` accepts a `nullptr` icon lambda; if so, branch the icon argument. Easiest: keep the Settings glyph for v1.)

The `OPDS` and `Continue Reading` `.insert(...)` calls earlier in the function (around `HomeActivity.cpp:234-241`) move the dynamic items to the front; the appended Todoist entry remains at the end where its index matches `todoistIdx` from Step 3.

- [ ] **Step 5: Implement `onTodoistOpen()`**

At the bottom of `HomeActivity.cpp`, add:

```cpp
void HomeActivity::onTodoistOpen() {
  startActivityForResult(
      std::make_unique<TodoistActivity>(renderer, mappedInput),
      [this](const ActivityResult&) { requestUpdate(true); });
}
```

Place the include at the top of the file with the other activity includes:

```cpp
#include "activities/integrations/TodoistActivity.h"
```

- [ ] **Step 6: Verify build**

Run: `pio run`

Expected: PASS, 0 errors.

- [ ] **Step 7: Verify all environments build**

Run: `pio run -e gh_release && pio run -e slim`

Expected: both PASS.

- [ ] **Step 8: Commit**

```bash
git add src/activities/home/HomeActivity.h src/activities/home/HomeActivity.cpp
git commit -m "feat: add Todoist entry to home screen menu"
```

---

### Phase 3 Smoke Test (manual, on hardware)

- [ ] Flash device. With a valid `todoist.json` (real personal API token) on SD and WiFi credentials configured:
- [ ] Boot, navigate to home screen, confirm "Todoist" entry appears.
- [ ] Open Todoist. Expect "Fetching tasks…" → list of today's tasks with header `Today - Updated HH:MM`.
- [ ] Up/Down scroll the list.
- [ ] Confirm triggers refresh (header timestamp updates).
- [ ] Back exits to home screen.
- [ ] With no `todoist.json` → "No Todoist token configured" screen.
- [ ] With invalid token → "Invalid token" after fetch.
- [ ] WiFi off / unreachable → "WiFi unavailable" within 15 s.

If all pass, Phase 3 is complete.

---

## Phase 4 — Snapshot Capture

**Scope:** Add snapshot-capture step to `TodoistActivity` after a successful render. Implement the render-twice trick when `activity_orientation != snapshot_orientation`. Persist a small meta sidecar so `SleepActivity` can verify orientation.

### Task 4.1: Add snapshot writer helpers to `TodoistActivity`

**Files:**
- Modify: `src/activities/integrations/TodoistActivity.h`
- Modify: `src/activities/integrations/TodoistActivity.cpp`

- [ ] **Step 1: Add private helpers to header**

Edit `src/activities/integrations/TodoistActivity.h`. In the `private:` section, add:

```cpp
void captureSnapshotIfNeeded();
bool writeSnapshotMeta(GfxRenderer::Orientation o);
```

- [ ] **Step 2: Add includes and constants in .cpp**

At the top of `src/activities/integrations/TodoistActivity.cpp` add:

```cpp
#include "util/ScreenshotUtil.h"
#include <HalStorage.h>
#include <ArduinoJson.h>
```

In the anonymous namespace add:

```cpp
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
```

- [ ] **Step 3: Implement `writeSnapshotMeta`**

Append to `src/activities/integrations/TodoistActivity.cpp`:

```cpp
bool TodoistActivity::writeSnapshotMeta(GfxRenderer::Orientation o) {
  JsonDocument doc;
  doc["orientation"] = orientationToString(o);
  doc["captured_hour"] = _capturedHour;
  doc["captured_min"] = _capturedMin;

  std::string out;
  serializeJson(doc, out);

  if (Storage.exists(kSnapshotMetaTmpPath)) Storage.remove(kSnapshotMetaTmpPath);

  HalFile file;
  if (!Storage.openFileForWrite("TDST", kSnapshotMetaTmpPath, file)) {
    LOG_ERR("TDST", "Cannot open meta tmp");
    return false;
  }
  size_t n = file.write(reinterpret_cast<const uint8_t*>(out.data()), out.size());
  file.close();
  if (n != out.size()) {
    LOG_ERR("TDST", "Short meta write");
    Storage.remove(kSnapshotMetaTmpPath);
    return false;
  }

  if (Storage.exists(kSnapshotMetaPath)) Storage.remove(kSnapshotMetaPath);
  if (!Storage.rename(kSnapshotMetaTmpPath, kSnapshotMetaPath)) {
    LOG_ERR("TDST", "Meta rename failed");
    return false;
  }
  return true;
}
```

- [ ] **Step 4: Implement `captureSnapshotIfNeeded`**

Append:

```cpp
void TodoistActivity::captureSnapshotIfNeeded() {
  // Precondition: _state == ShowingTasks and _tasks is populated.
  // Caller is responsible for that. We only render & save here.
  if (_state != State::ShowingTasks) return;

  const auto activityOrient = TODOIST_CONFIG.getActivityOrientation();
  const auto snapshotOrient = TODOIST_CONFIG.getSnapshotOrientation();

  if (activityOrient == snapshotOrient) {
    // Render task list to framebuffer (no displayBuffer here — the activity's
    // normal render() will paint the user-facing frame moments later).
    renderer.clearBuffer();
    renderTaskList();
    if (!ScreenshotUtil::saveFramebufferAsBmp(
            kSnapshotBmpPath, renderer.getFrameBuffer(),
            renderer.getDisplayWidth(), renderer.getDisplayHeight())) {
      LOG_ERR("TDST", "Snapshot save failed");
    } else {
      writeSnapshotMeta(snapshotOrient);
    }
    return;
  }

  // Different orientations: render in snapshot orientation to framebuffer
  // only (no displayBuffer), save, then revert. The next requestUpdate(true)
  // in proceedWithFetch will repaint in activity orientation.
  renderer.setOrientation(snapshotOrient);
  renderer.clearBuffer();
  renderTaskList();
  if (!ScreenshotUtil::saveFramebufferAsBmp(
          kSnapshotBmpPath, renderer.getFrameBuffer(),
          renderer.getDisplayWidth(), renderer.getDisplayHeight())) {
    LOG_ERR("TDST", "Snapshot save failed");
  } else {
    writeSnapshotMeta(snapshotOrient);
  }
  renderer.setOrientation(activityOrient);
}
```

`renderer.getFrameBuffer()` (capital B) is verified at `lib/GfxRenderer/GfxRenderer.h:159`; same accessor `ScreenshotUtil` itself uses (`src/util/ScreenshotUtil.cpp:71`).

- [ ] **Step 5: Wire snapshot into the render flow**

In `TodoistActivity::proceedWithFetch()`, immediately after setting `_state = State::ShowingTasks` and the timestamp fields, BEFORE the trailing `requestUpdate(true)`:

```cpp
if (r == FetchResult::Ok) {
  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  _capturedHour = static_cast<uint8_t>(tm_now.tm_hour);
  _capturedMin = static_cast<uint8_t>(tm_now.tm_min);
  _state = State::ShowingTasks;
  _scrollOffset = 0;
  captureSnapshotIfNeeded();   // <-- add this line
} else {
  // ... unchanged ...
}
requestUpdate(true);
```

The flow is: snapshot first (possibly in snapshot orientation), then `requestUpdate(true)` triggers `render()` which calls `renderTaskList()` in activity orientation and `displayBuffer()`s. The user only ever sees the activity-orientation frame.

- [ ] **Step 6: Verify build**

Run: `pio run`

Expected: PASS. If `renderer.getFrameBuffer()` doesn't compile, replace with the project's actual accessor (look at how `ScreenshotUtil` is invoked elsewhere — if callers pass a buffer obtained differently, mirror them).

- [ ] **Step 7: Verify all environments**

Run: `pio run -e gh_release && pio run -e slim`

Expected: both PASS.

- [ ] **Step 8: Commit**

```bash
git add src/activities/integrations/TodoistActivity.h src/activities/integrations/TodoistActivity.cpp
git commit -m "$(cat <<'EOF'
feat: capture Todoist snapshot to /.crosspoint/todoist_sleep.bmp

After a successful fetch, render the task list to the framebuffer in
snapshot_orientation (or activity_orientation if they match), save it
via ScreenshotUtil, write a small JSON meta sidecar with the captured
orientation and timestamp, then return so the activity's normal render
flow paints the user-facing frame in activity_orientation.
EOF
)"
```

---

### Phase 4 Smoke Test (manual, on hardware)

- [ ] Set `activity_orientation = portrait`, `snapshot_orientation = portrait` in `todoist.json`. Open Todoist. Verify `/.crosspoint/todoist_sleep.bmp` exists on SD and is non-zero size. Open the BMP in any image viewer; confirm a portrait task list rendering.
- [ ] Set `activity_orientation = landscape_cw`, `snapshot_orientation = portrait`. Re-open Todoist. Verify the on-screen frame is landscape (user view), and `todoist_sleep.bmp` on disk is portrait.
- [ ] Verify `/.crosspoint/todoist_sleep.meta` has correct `orientation` and timestamp.

---

## Phase 5 — Sleep Integration & Settings Submenu

**Scope:** `SleepActivity` pre-check that blits the Todoist snapshot when applicable, plus a `TodoistSettingsActivity` reachable from the existing Settings menu.

### Task 5.1: Add Todoist pre-check to SleepActivity

**Files:**
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`

- [ ] **Step 1: Read current onEnter**

Open `src/activities/boot_sleep/SleepActivity.cpp` lines 18-46. Confirm the popup block ends and the `switch (SETTINGS.sleepScreen)` begins. The pre-check goes between the popup and the switch.

- [ ] **Step 2: Add include**

At the top of `src/activities/boot_sleep/SleepActivity.cpp` add (next to other integration includes):

```cpp
#include "integrations/todoist/TodoistConfig.h"
#include <ArduinoJson.h>
```

- [ ] **Step 3: Add a private helper for the pre-check**

`FsFile` IS `HalFile` (alias declared in `lib/hal/HalStorage.h:101`). The existing custom-image flow at `SleepActivity.cpp:100-110` uses `FsFile` directly with `Storage.openFileForRead("SLP", filename, file)` then `Bitmap bitmap(file, true);` — we mirror that exactly.

In `SleepActivity.cpp` add this helper near the top (anonymous namespace) or just above `renderBitmapSleepScreen`:

```cpp
namespace {

bool tryRenderTodoistSleepScreen(GfxRenderer& renderer) {
  using todoist::TodoistConfig;

  if (!TodoistConfig::getInstance().isSleepScreenEnabled()) return false;
  if (!Storage.exists("/.crosspoint/todoist_sleep.bmp")) return false;
  if (!Storage.exists("/.crosspoint/todoist_sleep.meta")) return false;

  // Read meta and verify orientation matches the configured snapshot_orientation.
  FsFile metaFile;
  if (!Storage.openFileForRead("TDST", "/.crosspoint/todoist_sleep.meta", metaFile)) return false;

  std::string buf;
  buf.reserve(256);
  uint8_t chunk[128];
  while (true) {
    int n = metaFile.read(chunk, sizeof(chunk));
    if (n <= 0) break;
    buf.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
    if (buf.size() > 1024) break;  // sanity cap
  }
  metaFile.close();

  JsonDocument doc;
  if (deserializeJson(doc, buf)) return false;

  const char* orient = doc["orientation"] | "";
  auto cfgOrient = TodoistConfig::getInstance().getSnapshotOrientation();
  const char* expected = nullptr;
  switch (cfgOrient) {
    case GfxRenderer::Orientation::Portrait:                  expected = "portrait"; break;
    case GfxRenderer::Orientation::PortraitInverted:          expected = "portrait_inverted"; break;
    case GfxRenderer::Orientation::LandscapeClockwise:        expected = "landscape_cw"; break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise: expected = "landscape_ccw"; break;
  }
  if (!expected || strcmp(orient, expected) != 0) {
    LOG_DBG("TDST", "Snapshot orientation mismatch (got %s, want %s)", orient, expected);
    return false;
  }

  // Open and render the BMP — same incantation as the random-sleep flow at
  // SleepActivity.cpp:100-110.
  FsFile bmpFile;
  if (!Storage.openFileForRead("TDST", "/.crosspoint/todoist_sleep.bmp", bmpFile)) return false;

  renderer.setOrientation(cfgOrient);
  Bitmap bitmap(bmpFile, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("TDST", "Snapshot BMP header parse failed");
    bmpFile.close();
    return false;
  }
  renderBitmapSleepScreen(bitmap);
  bmpFile.close();
  return true;
}

}  // namespace
```

- [ ] **Step 4: Call the helper before the switch**

Modify `onEnter()`. After the popup block (around line 28) and before `switch (SETTINGS.sleepScreen)`:

```cpp
  if (tryRenderTodoistSleepScreen(renderer)) {
    return;
  }

  switch (SETTINGS.sleepScreen) {
    // ... existing code ...
```

- [ ] **Step 5: Verify build**

Run: `pio run`

Expected: PASS. Resolve any include / type mismatches by mirroring existing code in the same file.

- [ ] **Step 6: Commit**

```bash
git add src/activities/boot_sleep/SleepActivity.cpp
git commit -m "$(cat <<'EOF'
feat: add Todoist pre-check to SleepActivity

Before falling through to the existing sleep-screen switch, check the
Todoist toggle and the snapshot's recorded orientation. On match, render
the saved BMP via the existing renderBitmapSleepScreen helper and return.
On any mismatch or missing file, fall through silently to existing
behavior (the random /.sleep rotation, etc).
EOF
)"
```

---

### Task 5.2: Create `TodoistSettingsActivity`

**Files:**
- Create: `src/activities/settings/TodoistSettingsActivity.h`
- Create: `src/activities/settings/TodoistSettingsActivity.cpp`

- [ ] **Step 1: Read the reference settings activity**

Read `src/activities/settings/KOReaderSettingsActivity.cpp` and `.h` end-to-end. We mirror its structure: `ButtonNavigator` for Up/Down, `handleSelection()` on Confirm, `GUI.drawList` in `render()`, `Back` to exit.

- [ ] **Step 2: Write the header**

`ButtonNavigator` is in `src/util/ButtonNavigator.h:8-52` and uses callback registration (`onNext([this]{...})`, `onPrevious([this]{...})`). Its constructor takes auto-repeat timing (default 500ms). The global mapped-input manager drives it (set up at boot in `main.cpp`).

Write `src/activities/settings/TodoistSettingsActivity.h`:

```cpp
#pragma once

#include "Activity.h"
#include "MappedInputManager.h"
#include "util/ButtonNavigator.h"

#include <GfxRenderer.h>

class TodoistSettingsActivity : public Activity {
 public:
  TodoistSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void handleSelection();
  // Cycle through the four orientations.
  GfxRenderer::Orientation nextOrientation(GfxRenderer::Orientation current) const;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool navigatorBound = false;
  // Item indices: 0 = sleep toggle, 1 = activity orientation,
  // 2 = snapshot orientation, 3 = forget.
  static constexpr int kItemCount = 4;
};
```

- [ ] **Step 3: Verify header builds**

Run: `pio run`. PASS expected.

- [ ] **Step 4: Write the implementation**

Write `src/activities/settings/TodoistSettingsActivity.cpp`. Mirror the structure of `KOReaderSettingsActivity.cpp:42-50` for navigator setup and `:121-143` for the `GUI.drawList` call:

```cpp
#include "TodoistSettingsActivity.h"

#include "Logging.h"
#include "UITheme.h"
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

  if (!navigatorBound) {
    buttonNavigator.onNext([this] {
      selectedIndex = (selectedIndex + 1) % kItemCount;
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      selectedIndex = (selectedIndex + kItemCount - 1) % kItemCount;
      requestUpdate();
    });
    navigatorBound = true;
  }

  requestUpdate(true);
}

void TodoistSettingsActivity::loop() {
  mappedInput.update();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
  }
}

void TodoistSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case 0:
      TODOIST_CONFIG.setSleepScreenEnabled(!TODOIST_CONFIG.isSleepScreenEnabled());
      return;
    case 1:
      TODOIST_CONFIG.setActivityOrientation(
          nextOrientation(TODOIST_CONFIG.getActivityOrientation()));
      return;
    case 2:
      TODOIST_CONFIG.setSnapshotOrientation(
          nextOrientation(TODOIST_CONFIG.getSnapshotOrientation()));
      return;
    case 3:
      TODOIST_CONFIG.forget();
      return;
  }
}

GfxRenderer::Orientation TodoistSettingsActivity::nextOrientation(
    GfxRenderer::Orientation current) const {
  switch (current) {
    case GfxRenderer::Orientation::Portrait:
      return GfxRenderer::Orientation::PortraitInverted;
    case GfxRenderer::Orientation::PortraitInverted:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case GfxRenderer::Orientation::LandscapeClockwise:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

void TodoistSettingsActivity::render(RenderLock&&) {
  renderer.clearBuffer();

  const int pageWidth = renderer.getDisplayWidth();
  const int pageHeight = renderer.getDisplayHeight();
  const auto& metrics = GUI.getMetrics();

  // Header
  GUI.drawHeader(renderer, Rect{0, 0, pageWidth, metrics.headerHeight},
                 tr(STR_TODOIST));

  const int contentTop = metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight},
      kItemCount, selectedIndex,
      // title (rowTitle)
      [](int i) -> std::string {
        switch (i) {
          case 0: return std::string(tr(STR_TODOIST_SLEEP_SCREEN));
          case 1: return std::string(tr(STR_TODOIST_ACTIVITY_ORIENTATION));
          case 2: return std::string(tr(STR_TODOIST_SNAPSHOT_ORIENTATION));
          case 3: return std::string(tr(STR_TODOIST_FORGET));
        }
        return "";
      },
      nullptr,  // rowSubtitle
      nullptr,  // rowIcon
      // value (rowValue)
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

  renderer.displayBuffer();
}
```

**Notes:**
- `tr(STR_X)` already returns `const char*`; we wrap in `std::string()` only because `drawList`'s `rowTitle` lambda is typed `std::function<std::string(int)>` per `BaseTheme.h:136`.
- Orientation labels are intentionally English (technical strings, personal-fork integration). If the user wants them translated later, add `STR_TODOIST_ORIENT_PORTRAIT` etc and swap.
- If `GUI.getMetrics()` doesn't exist with that exact name, look at how `KOReaderSettingsActivity::render()` and `HomeActivity::render()` get layout dimensions and copy literally.

- [ ] **Step 5: Verify build**

Run: `pio run`

Expected: PASS, 0 errors. Resolve any UITheme/ButtonNavigator API mismatches by mirroring `KOReaderSettingsActivity.cpp` literally.

- [ ] **Step 6: Commit**

```bash
git add src/activities/settings/TodoistSettingsActivity.h src/activities/settings/TodoistSettingsActivity.cpp
git commit -m "$(cat <<'EOF'
feat: add TodoistSettingsActivity (toggle, orientations, forget)

Submenu reachable from the existing Settings activity. Item 0 toggles
the sleep-screen feature; items 1-2 cycle the active-view and snapshot
orientations; item 3 forgets all Todoist state on the SD card. Every
setter writes through to /.crosspoint/todoist.json atomically.
EOF
)"
```

---

### Task 5.3: Wire TodoistSettingsActivity into SettingsActivity

**Files:**
- Modify: `src/activities/settings/SettingsActivity.h`
- Modify: `src/activities/settings/SettingsActivity.cpp`

- [ ] **Step 1: Add include and handler declaration**

Open `src/activities/settings/SettingsActivity.h`. In the private section near other `onXxxOpen()` declarations:

```cpp
void onTodoistSettingsOpen();
```

Open `src/activities/settings/SettingsActivity.cpp`. Add at the top with other activity includes:

```cpp
#include "TodoistSettingsActivity.h"
```

- [ ] **Step 2: Add menu entry**

Find the existing menu items array / index logic in `SettingsActivity.cpp`. Add a "Todoist" entry after KOReader (or wherever integrations live). Mirror the EXACT pattern used to add KOReader's settings menu entry — index calculation, list rendering, dispatch in `handleSelection()`.

In `handleSelection()` add a branch:

```cpp
case kTodoistIdx:
  onTodoistSettingsOpen();
  return;
```

(Use whatever index naming convention the file already uses.)

- [ ] **Step 3: Implement `onTodoistSettingsOpen`**

At the bottom of `SettingsActivity.cpp`:

```cpp
void SettingsActivity::onTodoistSettingsOpen() {
  startActivityForResult(
      std::make_unique<TodoistSettingsActivity>(renderer, mappedInput),
      [this](const ActivityResult&) { requestUpdate(true); });
}
```

- [ ] **Step 4: Verify build**

Run: `pio run`

Expected: PASS, 0 errors.

- [ ] **Step 5: Verify all environments**

Run: `pio run -e gh_release && pio run -e slim && pio run -e gh_release_rc`

Expected: ALL PASS.

- [ ] **Step 6: Commit**

```bash
git add src/activities/settings/SettingsActivity.h src/activities/settings/SettingsActivity.cpp
git commit -m "feat: expose Todoist submenu in SettingsActivity"
```

---

### Phase 5 Smoke Test (manual, on hardware)

- [ ] In Settings, navigate to "Todoist". Verify all 4 items render with current values.
- [ ] Toggle "Todoist sleep screen" → status changes On/Off; reboot device → setting persists.
- [ ] Cycle Activity orientation. Re-open Todoist activity → frame renders in selected orientation.
- [ ] Set snapshot_orientation = portrait, sleep screen on. Open Todoist (capture). Sleep device → portrait Todoist snapshot renders.
- [ ] Open Todoist with snapshot_orientation = landscape_cw. Sleep → landscape Todoist snapshot renders. (Earlier portrait BMP overwritten.)
- [ ] In settings, change snapshot_orientation WITHOUT re-opening Todoist activity. Sleep → falls through to existing `/.sleep` (orientation mismatch in meta). No crash.
- [ ] Disable Todoist sleep screen toggle. Sleep → existing rotation. No Todoist render.
- [ ] "Forget Todoist" → both `todoist.json` and `todoist_sleep.bmp` removed; toggle is now off; sleep → existing rotation.

---

## Final Verification

- [ ] **All build environments pass:** `pio run -e default && pio run -e gh_release && pio run -e gh_release_rc && pio run -e slim`
- [ ] **Static analysis:** `pio check` — no new high/medium issues.
- [ ] **Format:** `find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i` — clean diff.
- [ ] **Heap delta:** monitor `ESP.getFreeHeap()` before entering Todoist activity and after exiting. Within ±2 KB.
- [ ] **20× open/close:** no monotonic heap leak.
- [ ] **Reflash upstream `gh_release`:** boots cleanly, normal reading flow works. Confirms no SPIFFS contamination.

---

## Out-of-scope reminders (do NOT do in this plan)

- Mark-complete / add / edit (v2).
- Filters other than "today" (v2).
- Background refresh / scheduled wake (v2).
- On-device token entry (v2).
- OAuth / multiple accounts (v2).
- Web settings page integration (v2).
- Custom certs on SD (we use `esp_crt_bundle_attach`).

---

## Self-review checklist (run before handing off)

- [x] Spec coverage: every section in the spec maps to a phase. Phase 1 = §5/§7. Phase 2 = §6/§8 client. Phase 3 = §4/§5 activity. Phase 4 = §6.1 snapshot. Phase 5 = §4.3/§6.3 sleep + settings.
- [x] No placeholders (`TBD`, `TODO`, "fill in"). All steps contain real code or exact commands.
- [x] Function/property names consistent across phases: `getApiToken`, `hasValidToken`, `setSleepScreenEnabled`, `setActivityOrientation`, `setSnapshotOrientation`, `forget`, `captureSnapshotIfNeeded`, `writeSnapshotMeta`, `tryRenderTodoistSleepScreen`.
- [x] Each phase is ≤5 files.
- [x] Frequent commits — every task ends with a commit.
- [x] DRY/YAGNI: orientation enum strings are duplicated between `TodoistConfig.cpp` and `TodoistActivity.cpp` deliberately — keeping them as private helpers in their own translation units beats exposing a shared header for two short functions. If a third call site appears, hoist them.
- [x] Adapt-to-actual-API notes added wherever the existing project's helper names might differ (`GUI.drawList`, `ButtonNavigator`, `renderer.getFramebuffer`, BMP file opening). The implementer is told explicitly to mirror the existing reference activities rather than invent.
