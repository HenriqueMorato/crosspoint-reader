# Todoist Integration — Design Spec

**Date:** 2026-05-04
**Project:** CrossPoint Reader (Xteink X4, ESP32-C3)
**Scope:** Personal fork; modular enough that other users can supply their own API key.
**Status:** Design approved, awaiting user review before implementation planning.

---

## 1. Summary

Add a Todoist integration that lets the user open a read-only "Today" task list on the e-reader, and optionally use the most recent rendering of that list as the device's sleep screen. All Todoist state lives on the SD card; the internal flash settings partition (SPIFFS) is **not** modified, so reflashing upstream firmware leaves no persistent residue.

Refresh is **explicitly user-driven** — there is no background polling, no scheduled wake, and no battery cost beyond the WiFi connection used during a manual fetch.

## 2. Goals

- View the user's Todoist "Today" filter on the device.
- Persist the most recent rendering as a BMP usable by the existing `SleepActivity` pipeline.
- Let the user toggle the Todoist sleep screen on/off, falling back to the existing `/.sleep` rotation when off.
- Keep all Todoist state on the SD card (token, toggles, snapshot, CA cert).
- Reuse the project's existing TLS pattern (`esp_crt_bundle_attach`) so first-run setup requires only the user's API token.

## 3. Non-goals (v1)

- Mark-complete, add, edit, or delete tasks (read-only only).
- Filters other than "today".
- Background / scheduled refresh.
- Multiple Todoist accounts.
- An on-device API token entry UI (token is placed on SD by the user).
- An OAuth flow (we use Todoist's personal API token).
- Web settings page integration (entirely SD-driven).

## 4. User-facing behavior

### 4.1 First-time setup

1. User generates a personal API token in Todoist's web settings.
2. User creates `/.crosspoint/todoist.json` on the SD card containing the token.
3. User selects "Todoist" from the home screen menu.
4. Activity brings up WiFi, fetches today's tasks (TLS validated against ESP-IDF's built-in Mozilla CA bundle), renders them, captures a snapshot, and drops WiFi.

### 4.2 Daily use

- **Open Todoist activity from home screen** → fetches once, renders, captures snapshot, drops WiFi.
- **Up / Down** → scroll the list (no re-fetch).
- **Confirm** → manual refresh (re-runs WiFi → fetch → render → snapshot → drop WiFi).
- **Back** → exit.
- **Sleep** → if the toggle is on and the snapshot's orientation matches the configured Todoist orientation, blit the snapshot. Otherwise fall through to the existing sleep behavior.

### 4.3 Settings the user controls

| Setting | Stored in | Default |
|---|---|---|
| Todoist sleep screen on/off | `/.crosspoint/todoist.json` | `false` |
| Activity orientation (active view) | `/.crosspoint/todoist.json` | `portrait` |
| Snapshot orientation (sleep screen) | `/.crosspoint/todoist.json` | `portrait` |
| API token | `/.crosspoint/todoist.json` | (none — user must provide) |

Settings are exposed via a Todoist submenu in the existing `SettingsActivity`. A "Forget Todoist" action deletes both `todoist.json` and `todoist_sleep.bmp`.

**Persistence model:** every change in the settings submenu is written to `/.crosspoint/todoist.json` immediately. `TodoistConfig` setters compare against the current value (write-throttling) and, on change, perform an atomic write — serialize to `.todoist.json.tmp`, fsync, rename to `todoist.json`. Surviving power loss mid-write means a clean partial-state recovery: either the old file or the new file, never a corrupt half.

The two orientation settings are independent. Use case: read tasks in landscape on the desk, but show the sleep screen in portrait on a nightstand.

### 4.4 Active view layout

```
  Today — Updated 09:42                         [ 7 tasks ]
  ─────────────────────────────────────────────────────────
  [!]  09:30   Pay landlord                              p1
       14:00   Reply to Mateus                           p3
               Pick up groceries (no time)               p4
  [!]          Overdue: renew SIM                        p2
  ...
```

- Header line: "Today — Updated HH:MM" plus task count.
- One row per task: overdue glyph, due time (if any), title (ellipsised), priority indicator.
- Title wraps via truncation, never line-wraps.
- All coordinates and fonts go through `UITheme` (the GUI macro). No hardcoded geometry.
- Active-view renderer orientation is set explicitly to `TodoistConfig::getActivityOrientation()` regardless of the device's physical orientation.

## 5. Architecture

### 5.1 New modules

```
src/integrations/todoist/
├── TodoistConfig.{h,cpp}    Singleton. Loads/persists /.crosspoint/todoist.json.
├── TodoistClient.{h,cpp}    HTTPS + JSON for the "today" endpoint. Stateless. TLS via esp_crt_bundle_attach.
└── TodoistTask.h            POD struct: title, due_datetime, priority, overdue.

src/activities/integrations/
└── TodoistActivity.{h,cpp}  Live scrollable view + WiFi management + snapshot capture.
```

### 5.2 Modified files

```
src/activities/boot_sleep/SleepActivity.cpp     Add Todoist pre-check at top of switch.
src/activities/home/HomeActivity.{cpp}           Add "Todoist" menu entry.
src/activities/settings/SettingsActivity.{cpp}   Add Todoist submenu (toggle, orientation, Forget).
src/main.cpp                                     Boot-time TodoistConfig::getInstance().load();
lib/I18n/translations/english.yaml               Add new STR_TODOIST_* keys.
```

### 5.3 Files NOT modified

- `src/CrossPointSettings.{h,cpp}` — no SPIFFS schema change.
- `src/SettingsList.h` — no new SPIFFS-backed settings.
- `partitions.csv` — unchanged.

This is the property that makes rollback to upstream trivial.

### 5.4 Dependency boundaries

- `TodoistActivity` depends on `TodoistConfig` (read), `TodoistClient` (fetch), `HalDisplay` (render), WiFi.
- `TodoistClient` depends on `esp_http_client`, ArduinoJson, and `esp_crt_bundle_attach` (ESP-IDF Mozilla CA bundle).
- `TodoistConfig` depends only on `HalStorage` and ArduinoJson.
- `SleepActivity` depends on `TodoistConfig` (read-only check) and reuses its existing `renderBitmapSleepScreen`.

## 6. Data flow

### 6.1 Opening the activity (happy path)

```
User selects "Todoist" on home screen
  → TodoistActivity::onEnter()
    → renderer.setOrientation(TodoistConfig.getActivityOrientation())
    → drawCenteredText(STR_TODOIST_FETCHING) + displayBuffer()
    → TodoistConfig.getApiToken()  [may render error & exit]
    → bringUpWifi(timeoutMs=15000)  [may render error & exit]
    → syncTimeWithNTP()             [matches KOReaderSyncActivity pattern]
    → TodoistClient::fetchToday(token, &tasks)
        - HTTPS GET https://api.todoist.com/rest/v2/tasks?filter=today
        - Authorization: Bearer <token>
        - Streamed deserializeJson into reserved std::vector<TodoistTask> (cap 64)
    → captureSnapshotIfNeeded()
        IF activity_orientation == snapshot_orientation:
          - renderTaskList(tasks, scrollOffset=0)         [framebuffer only]
          - writeSnapshotBmp("/.crosspoint/todoist_sleep.bmp")
          - writeSnapshotMeta(snapshot_orientation)
          - displayBuffer()                               [user sees this]
        ELSE:
          - renderer.setOrientation(snapshot_orientation)
          - renderTaskList(tasks, scrollOffset=0)         [framebuffer only, NOT displayBuffer]
          - writeSnapshotBmp("/.crosspoint/todoist_sleep.bmp")
          - writeSnapshotMeta(snapshot_orientation)
          - renderer.setOrientation(activity_orientation)
          - renderTaskList(tasks, scrollOffset=0)         [framebuffer]
          - displayBuffer()                               [user sees this only]
    → WiFi.disconnect()
```

### 6.2 Refresh (Confirm pressed)

Same as opening, minus orientation set, minus loading text. `bringUpWifi → syncTime → fetch → render → captureSnapshot → drop WiFi`.

### 6.3 Sleep screen integration

```
SleepActivity::onEnter():
  - Existing initial popup logic runs unchanged.
  - PRE-CHECK (new):
      if TodoistConfig::isSleepScreenEnabled()
         AND Storage.exists("/.crosspoint/todoist_sleep.bmp")
         AND meta.orientation == TodoistConfig::getSnapshotOrientation():
           renderer.setOrientation(TodoistConfig::getSnapshotOrientation())
           open + parse BMP
           call existing renderBitmapSleepScreen(bitmap)
           return
  - Otherwise: fall through to existing switch on SETTINGS.sleepScreen.
```

The orientation check exists for one reason: if the user changed `TodoistConfig.snapshot_orientation` in settings between the last capture and going to sleep, the saved snapshot is in the old orientation and would render rotated. Falling through is safer than rendering wrong-orientation content. **Device orientation is intentionally not part of this check** — the Todoist snapshot always renders in its configured orientation regardless of how the device is physically held.

The Todoist snapshot is **not** dropped into `/.sleep/` because that folder is a randomized rotation pool. We want deterministic display of the Todoist snapshot when toggled on.

### 6.4 Exit

```
TodoistActivity::onExit():
  - Free task vector (per-task strings are fixed-size char arrays, no heap to free).
  - Activity::onExit().
```

No FreeRTOS tasks, no malloc'd buffers held across the activity boundary. Synchronous fetch in `onEnter` simplifies lifecycle.

## 7. Settings & configuration

### 7.1 `/.crosspoint/todoist.json` schema

```json
{
  "api_token": "string, required, ≥20 chars",
  "sleep_screen_enabled": true,
  "activity_orientation": "portrait" | "portrait_inverted" | "landscape_cw" | "landscape_ccw",
  "snapshot_orientation": "portrait" | "portrait_inverted" | "landscape_cw" | "landscape_ccw"
}
```

Both orientation fields default to `"portrait"`. Unknown keys are ignored. Missing fields fall back to defaults — only `api_token` is strictly required. `TodoistConfig::set*()` setters guard with value-change checks before writing (per CLAUDE.md SD/SPIFFS write throttling rule, which applies to SD too — finite write cycles), then perform an atomic write via temp-file-and-rename.

### 7.2 i18n keys (additions to English YAML)

| Key | English |
|---|---|
| `STR_TODOIST` | "Todoist" |
| `STR_TODOIST_FETCHING` | "Fetching tasks…" |
| `STR_TODOIST_NO_TOKEN` | "No Todoist token configured" |
| `STR_TODOIST_NO_SD` | "SD card unavailable" |
| `STR_TODOIST_OFFLINE` | "WiFi unavailable" |
| `STR_TODOIST_FETCH_FAILED` | "Could not fetch tasks" |
| `STR_TODOIST_NO_TASKS` | "All clear today" |
| `STR_TODOIST_SLEEP_SCREEN` | "Todoist sleep screen" |
| `STR_TODOIST_ACTIVITY_ORIENTATION` | "Activity orientation" |
| `STR_TODOIST_SNAPSHOT_ORIENTATION` | "Sleep screen orientation" |
| `STR_TODOIST_FORGET` | "Forget Todoist" |
| `STR_TODOIST_TODAY_HEADER` | "Today — Updated %02d:%02d" |

Other languages fall back to English per existing project convention.

### 7.3 TLS certificate handling

The project already ships ESP-IDF's Mozilla root-CA bundle. The Todoist API uses Let's Encrypt, whose root (ISRG Root X1) is in that bundle. We use the existing pattern unchanged from `lib/KOReaderSync/KOReaderSyncClient.cpp`:

```cpp
esp_http_client_config_t cfg = {};
cfg.url = "https://api.todoist.com/rest/v2/tasks?filter=today";
cfg.crt_bundle_attach = esp_crt_bundle_attach;  // ESP-IDF Mozilla CA bundle
// ... etc
```

No PEM file shipped, no SD setup, no first-run copy logic. Cert rotation is handled by ESP-IDF/firmware updates (decade-scale concern).

## 8. Error handling

Following `LOG_ERR + fallback` (CLAUDE.md "Error Handling Philosophy"). Active view errors render a static screen and wait for Back. Sleep-screen errors silently fall through to the existing pipeline.

| Scenario | Behavior |
|---|---|
| `todoist.json` missing / malformed / no token | `STR_TODOIST_NO_TOKEN` |
| SD unreachable | `STR_TODOIST_NO_SD` |
| WiFi credentials missing or AP unreachable (15s) | `STR_TODOIST_OFFLINE` |
| HTTPS 401 / 403 | `STR_TODOIST_FETCH_FAILED` "invalid token" |
| HTTPS 429 | `STR_TODOIST_FETCH_FAILED` "rate limited" |
| HTTPS 5xx / timeout / TLS handshake failure | `STR_TODOIST_FETCH_FAILED` (specific sub-line) |
| Empty tasks array | `STR_TODOIST_NO_TASKS`, snapshot still captured |
| JSON parse error | `STR_TODOIST_FETCH_FAILED` "unexpected response" |
| `malloc` for snapshot buffer fails | LOG_ERR; skip snapshot; active view unaffected |
| Snapshot write fails (SD full, etc.) | LOG_ERR; skip; previous snapshot remains valid |
| Orientation mismatch at sleep | Fall through to existing sleep modes |
| Snapshot exists but toggle off | Ignore snapshot; existing sleep modes run |
| User pulls SD mid-fetch | `STR_TODOIST_FETCH_FAILED` "storage unavailable" |

**Failed refresh never overwrites a valid snapshot.** Stale tasks > blank tasks for the sleep screen.

## 9. Memory budget

| Buffer | Size | Lifetime |
|---|---|---|
| Framebuffer (existing) | 48 KB | Owned by renderer |
| `std::vector<TodoistTask>` reserved 64 | ~7 KB | `onEnter` to `onExit` |
| ArduinoJson document (streaming) | ~8 KB | Fetch only |
| Per-task title `char[96]` | (already in vector) | — |
| HTTPS + TLS client transient (esp_http_client + mbedTLS handshake) | ~30 KB | Fetch only |

Peak transient pressure during fetch: ~45 KB on top of baseline. We log `ESP.getFreeHeap()` before and after fetch to verify on hardware.

Stack: rendering uses no large local variables. Fixed-size `char[96]` per-task lives in heap-vector storage, not stack. No `std::function`, no recursive rendering.

## 10. Rollback

| Level | Action | Reversibility |
|---|---|---|
| Soft | Settings → "Todoist sleep screen" off | Instant; sleep returns to existing behavior |
| Forget | Settings → "Forget Todoist" | Deletes `todoist.json` + `todoist_sleep.bmp` |
| Code-level | `git revert` Todoist commits in fork; rebuild & flash | One build cycle |
| Upstream firmware | Flash upstream `gh_release` | Replaces binary; **no SPIFFS schema divergence to worry about** because we never wrote there |
| Nuclear | `esptool.py erase_flash` + reflash upstream | Full factory state |

## 11. Testing

### 11.1 Build-time (verifiable by AI)

- `pio run -t clean && pio run` — 0 errors, 0 new warnings.
- `pio run -e gh_release && pio run -e gh_release_rc && pio run -e slim` — all pass.
- `pio check` — no new high/medium issues.
- `clang-format` — idempotent across new files.
- `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/` — generated headers in sync with source YAMLs.
- Manual code review: orientation handling correct; no hardcoded geometry; all UI strings via `tr()`; all logging via `LOG_*`.

### 11.2 Hardware checklist (user runs)

**Configuration:**
- [ ] No `todoist.json` → "no token" screen
- [ ] Malformed `todoist.json` → "no token" screen, log shows parse error
- [ ] Empty `api_token` → "no token" screen
- [ ] No SD card → "SD unavailable"

**Network:**
- [ ] No WiFi credentials → "WiFi unavailable"
- [ ] WiFi credentials but AP unreachable after 15s → "WiFi unavailable"
- [ ] Working WiFi + valid token → tasks render
- [ ] Working WiFi, valid token, mid-fetch WiFi drop → "fetch failed"

**API:**
- [ ] Valid token, empty today → "all clear today" + snapshot saved
- [ ] Valid token, populated today → list renders correctly
- [ ] Invalid/revoked token → "fetch failed: invalid token"

**Snapshot & sleep:**
- [ ] Open in portrait, sleep → portrait snapshot displays
- [ ] Open, toggle off, sleep → falls through to existing `/.sleep` rotation
- [ ] Set activity_orientation = portrait, snapshot_orientation = portrait, open, sleep → portrait snapshot displays
- [ ] Set activity_orientation = landscape_cw, snapshot_orientation = portrait, open → user sees landscape view; sleep → portrait snapshot displays (user never saw the portrait render)
- [ ] Open with snapshot_orientation = portrait, then change snapshot_orientation to landscape in settings (without re-opening), sleep → falls through (snapshot orientation no longer matches config)
- [ ] Re-open activity after changing snapshot_orientation → captures fresh snapshot in new orientation, sleep displays it
- [ ] Verify settings persistence: change a toggle, force power-cycle device, re-boot → setting is preserved (atomic write survived)
- [ ] Delete `todoist_sleep.bmp` manually, sleep → falls through
- [ ] Re-open activity → re-fetches and re-snapshots

**Memory:**
- [ ] Heap pre-entry vs post-exit: within ~2 KB of baseline
- [ ] Open / close 20× → no monotonic leak
- [ ] Stack high-water mark > 512 bytes

**Rollback:**
- [ ] Toggle off → sleep returns immediately
- [ ] "Forget Todoist" → both SD files removed; toggle disabled
- [ ] Reflash upstream `gh_release` → device boots, normal reading flow works, no missing-symbol crashes

**Battery sanity:**
- [ ] Standby with toggle on → drain matches normal sleep baseline (e-ink holds image for free)
- [ ] One activity open per day for a week → battery roughly matches reading-only baseline

## 12. Future work (v2 candidates)

- Mark-complete from device (introduces POST + retry/queue logic).
- User-configurable filter string (defaulting to "today").
- Multiple project / label views, side-button-cycled.
- Background refresh on a long timer (battery cost — only if user explicitly opts in).
- On-device token entry via `KeyboardEntryActivity` (slow but self-contained).
- Web settings page integration for token entry.

## 13. Decisions log

| # | Decision | Rationale |
|---|---|---|
| 1 | v1 read-only | Use case is consumption, not editing; halves scope |
| 2 | Hardcoded "today" filter | Bounded data; matches user request "tasks for the day" |
| 3 | All state on SD, none in SPIFFS | Personal-fork rollback cleanliness; SD is removable & inspectable |
| 4 | Manual refresh only, no scheduling | Battery; user already opens device daily for reading |
| 5 | Capture snapshot on every entry | Predictable; no "stale because never refreshed" hidden state |
| 6 | Snapshot stored as BMP, reuses sleep pipeline | Cheapest sleep wake; reuses ~150 lines of existing rendering |
| 7 | Two independent orientations (activity + snapshot) | Active reading and sleep-screen viewing happen in different physical contexts (e.g., desk vs nightstand). One render-twice trick keeps the user from seeing the snapshot-orientation render. |
| 8 | Snapshot does not auto-expire | User has full control via Refresh button; staleness is user's responsibility |
| 9 | Captured timestamp rendered in header | User calibrates trust visually |
| 10 | Home-screen menu entry, activity manages WiFi | Daily-use ergonomics |
| 11 | Confirm = Refresh | v1 has no other use for Confirm; remap in v2 if mark-complete is added |
| 12 | TLS via existing `esp_crt_bundle_attach` (Mozilla CA bundle) | Already shipped in firmware; ISRG Root X1 (Let's Encrypt) is in the bundle; zero new cert plumbing |
| 13 | Todoist REST API v2 + personal token | Avoids OAuth complexity; matches "personal fork" use case |

## 14. Implementation phases

Per CLAUDE.md "Phased Execution" (each phase ≤ 5 files):

1. **Config foundation** — `TodoistConfig` singleton (load/save/getters/setters/atomic write), `TodoistTask` POD, i18n string additions, boot-time load in `main.cpp`.
2. **API client** — `TodoistClient` (HTTPS via `esp_crt_bundle_attach`, streaming JSON parse, error mapping).
3. **Activity** — `TodoistActivity` (WiFi bring-up, fetch, render task list, scroll, refresh), home-screen menu entry.
4. **Snapshot capture** — framebuffer-to-BMP via existing `ScreenshotUtil`, meta sidecar, render-twice trick when orientations differ.
5. **Sleep integration & settings** — `SleepActivity` pre-check, Todoist submenu in `SettingsActivity` (toggle, activity orientation, snapshot orientation, Forget).

Each phase ends with build + on-device smoke test before advancing.
