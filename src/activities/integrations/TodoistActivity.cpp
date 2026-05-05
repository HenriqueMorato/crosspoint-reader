#include "TodoistActivity.h"

#include "Logging.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "integrations/todoist/TodoistConfig.h"
#include "util/ScreenshotUtil.h"

#include <HalStorage.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <sys/time.h>

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace {

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

// Try NTP first. If it times out, set the clock to the firmware's build
// date so TLS cert validation can still succeed — mbedTLS rejects certs
// whose notBefore lies in the future relative to the device clock, and
// the api.todoist.com cert's notBefore is well after the epoch fallback.
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
  if (retry < 50) {
    LOG_DBG("TDST", "NTP synced");
    return;
  }

  LOG_DBG("TDST", "NTP timeout, using build date for TLS validation");
  struct tm tm = {};
  if (strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &tm) != nullptr) {
    time_t t = mktime(&tm);
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, nullptr);
  } else {
    LOG_ERR("TDST", "Build date parse failed; clock unset");
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

}  // namespace

void TodoistActivity::onEnter() {
  Activity::onEnter();
  _entryOrientation = renderer.getOrientation();
  renderer.setOrientation(TODOIST_CONFIG.getActivityOrientation());

  _state = State::Loading;
  _scrollOffset = 0;
  _tasks.clear();

  requestUpdate(true);  // paint Loading screen
  startFetch();
}

void TodoistActivity::onExit() {
  wifiOff();
  // Restore orientation so HomeActivity (and its cached coverBuffer) are
  // displayed in the orientation they were rendered in.
  renderer.setOrientation(_entryOrientation);
  Activity::onExit();
}

void TodoistActivity::loop() {
  // Don't call mappedInput.update() here — main loop already calls gpio.update()
  // before dispatching to the activity. A second update() in the same frame
  // re-snapshots state and clears the press/release edges, leaving wasReleased()
  // permanently false.
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

  // Scroll: ButtonNavigator polls each loop iteration.
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

    // Chronological sort: oldest overdue first, then today's timed tasks in
    // ascending time, then today's untimed tasks last. dueDate is "YYYY-MM-DD"
    // so plain strcmp gives chronological order; empty dueTime sorts after
    // any "HH:MM" because '\0' < any printable char — flip the empty-time
    // case explicitly so untimed entries land at the end of their date group.
    std::sort(_tasks.begin(), _tasks.end(),
              [](const todoist::TodoistTask& a, const todoist::TodoistTask& b) {
                int dateCmp = strcmp(a.dueDate, b.dueDate);
                if (dateCmp != 0) return dateCmp < 0;
                bool aTimed = a.dueTime[0] != '\0';
                bool bTimed = b.dueTime[0] != '\0';
                if (aTimed != bTimed) return aTimed;  // timed first
                if (aTimed) return strcmp(a.dueTime, b.dueTime) < 0;
                return false;  // both untimed: stable order
              });

    _state = State::ShowingTasks;
    _scrollOffset = 0;
    captureSnapshotIfNeeded();
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

bool TodoistActivity::writeSnapshotMeta(GfxRenderer::Orientation o) {
  char buf[160];
  int len = snprintf(buf, sizeof(buf),
                     "{\"orientation\":\"%s\",\"captured_hour\":%u,\"captured_min\":%u}",
                     orientationToString(o), static_cast<unsigned>(_capturedHour),
                     static_cast<unsigned>(_capturedMin));
  if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
    LOG_ERR("TDST", "Meta format failed");
    return false;
  }

  if (Storage.exists(kSnapshotMetaTmpPath)) Storage.remove(kSnapshotMetaTmpPath);

  HalFile file;
  if (!Storage.openFileForWrite("TDST", kSnapshotMetaTmpPath, file)) {
    LOG_ERR("TDST", "Cannot open meta tmp");
    return false;
  }
  size_t n = file.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
  file.close();
  if (n != static_cast<size_t>(len)) {
    LOG_ERR("TDST", "Short meta write");
    Storage.remove(kSnapshotMetaTmpPath);
    return false;
  }

  if (Storage.exists(kSnapshotMetaPath)) Storage.remove(kSnapshotMetaPath);
  if (!Storage.rename(kSnapshotMetaTmpPath, kSnapshotMetaPath)) {
    LOG_ERR("TDST", "Meta rename failed");
    Storage.remove(kSnapshotMetaTmpPath);
    return false;
  }
  return true;
}

void TodoistActivity::captureSnapshotIfNeeded() {
  if (_state != State::ShowingTasks) return;

  const auto activityOrient = TODOIST_CONFIG.getActivityOrientation();
  const auto snapshotOrient = TODOIST_CONFIG.getSnapshotOrientation();

  if (activityOrient == snapshotOrient) {
    renderTaskList(/*drawHints=*/false);
    if (!ScreenshotUtil::saveFramebufferAsBmpOriented(
            kSnapshotBmpPath, renderer.getFrameBuffer(),
            renderer.getDisplayWidth(), renderer.getDisplayHeight(),
            snapshotOrient)) {
      LOG_ERR("TDST", "Snapshot save failed");
    } else {
      LOG_DBG("TDST", "Snapshot saved to %s", kSnapshotBmpPath);
      writeSnapshotMeta(snapshotOrient);
    }
    return;
  }

  // Different orientations: render in snapshot orientation, save, then revert.
  renderer.setOrientation(snapshotOrient);
  renderTaskList(/*drawHints=*/false);
  if (!ScreenshotUtil::saveFramebufferAsBmpOriented(
          kSnapshotBmpPath, renderer.getFrameBuffer(),
          renderer.getDisplayWidth(), renderer.getDisplayHeight(),
          snapshotOrient)) {
    LOG_ERR("TDST", "Snapshot save failed");
  } else {
    LOG_DBG("TDST", "Snapshot saved to %s (rotated)", kSnapshotBmpPath);
    writeSnapshotMeta(snapshotOrient);
  }
  renderer.setOrientation(activityOrient);
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
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_TODOIST_FETCHING));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TodoistActivity::renderError() {
  renderer.clearScreen();
  GUI.drawPopup(renderer, I18N.get(_errorStrId));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TodoistActivity::renderTaskList(bool drawHints) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char header[64];
  snprintf(header, sizeof(header), I18N.get(StrId::STR_TODOIST_TODAY_HEADER),
           _capturedHour, _capturedMin);

  GUI.drawHeader(renderer, Rect(0, 0, pageWidth, metrics.headerHeight), header);

  if (!drawHints) {
    // Snapshot mode — paint over the battery icon + percentage text drawn by
    // drawHeader. Battery on a sleep screen is misleading: the value was true
    // when the snapshot was taken, not when the screen is being viewed.
    // 80px matches BaseTheme's reserved battery region.
    constexpr int kBatteryRegionWidth = 80;
    renderer.fillRect(pageWidth - kBatteryRegionWidth, 5, kBatteryRegionWidth,
                      metrics.batteryHeight + 10, false);
  }

  if (drawHints) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (_tasks.empty()) {
    GUI.drawPopup(renderer, tr(STR_TODOIST_NO_TASKS));
    return;
  }

  const int contentTop = metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - (drawHints ? metrics.buttonHintsHeight : 0);

  // Compact bullet list. Each task gets only the height it needs (1 or 2
  // wrapped lines), with a small gap between tasks. No separator lines —
  // the bullet glyph is the row delimiter.
  constexpr int kSidePadding = 20;
  constexpr const char* kBulletNormal = "\xE2\x80\xA2";  // U+2022 BULLET
  constexpr const char* kBulletOverdue = "!";            // overdue marker
  constexpr int kBulletGap = 8;     // px between bullet and title
  constexpr int kRowGap = 6;        // px between consecutive tasks
  constexpr int kMaxLines = 2;

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // Reserve space for the widest possible glyph so the text column lines up
  // regardless of which marker each row ends up using.
  const int bulletColWidth = std::max(
      renderer.getTextWidth(UI_10_FONT_ID, kBulletNormal),
      renderer.getTextWidth(UI_10_FONT_ID, kBulletOverdue));
  const int textX = kSidePadding + bulletColWidth + kBulletGap;
  const int textWidth = pageWidth - kSidePadding - textX;

  const int totalTasks = static_cast<int>(_tasks.size());
  if (_scrollOffset > totalTasks - 1) _scrollOffset = std::max(0, totalTasks - 1);

  int y = contentTop;
  int rendered = 0;
  for (int taskIdx = _scrollOffset; taskIdx < totalTasks; ++taskIdx) {
    const auto& t = _tasks[taskIdx];

    char fullTitle[128];
    snprintf(fullTitle, sizeof(fullTitle), "%s%s%s",
             t.dueTime[0] ? t.dueTime : "",
             t.dueTime[0] ? "  " : "",
             t.title);

    auto lines = renderer.wrappedText(UI_10_FONT_ID, fullTitle, textWidth, kMaxLines);
    const int taskHeight = static_cast<int>(lines.size()) * lineHeight;
    if (y + taskHeight > contentTop + contentHeight) break;  // would clip

    // Marker aligned with the first line baseline. Overdue tasks swap the
    // bullet for an exclamation mark so the row visually stands out without
    // wasting horizontal space on a "[!]" prefix.
    const char* marker = t.overdue ? kBulletOverdue : kBulletNormal;
    renderer.drawText(UI_10_FONT_ID, kSidePadding, y + lineHeight - 4, marker, true);

    for (size_t li = 0; li < lines.size(); ++li) {
      renderer.drawText(UI_10_FONT_ID, textX, y + (static_cast<int>(li) + 1) * lineHeight - 4,
                        lines[li].c_str(), true);
    }

    y += taskHeight + kRowGap;
    rendered++;
  }

  // Scroll bar when there are tasks below the visible window.
  if (rendered < totalTasks - _scrollOffset || _scrollOffset > 0) {
    const int barX = pageWidth - 6;
    const int barTrackHeight = contentHeight;
    const int barHeight = std::max(8, (barTrackHeight * rendered) / totalTasks);
    const int maxOffset = std::max(1, totalTasks - rendered);
    const int barY = contentTop + ((barTrackHeight - barHeight) * _scrollOffset) / maxOffset;
    renderer.fillRect(barX, barY, 2, barHeight, true);
  }
}
