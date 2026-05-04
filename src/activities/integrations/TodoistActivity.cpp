#include "TodoistActivity.h"

#include "Logging.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
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
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_TODOIST_FETCHING));
}

void TodoistActivity::renderError() {
  renderer.clearScreen();
  GUI.drawPopup(renderer, I18N.get(_errorStrId));
}

void TodoistActivity::renderTaskList() {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char header[64];
  snprintf(header, sizeof(header), I18N.get(StrId::STR_TODOIST_TODAY_HEADER),
           _capturedHour, _capturedMin);

  GUI.drawHeader(renderer, Rect(0, 0, pageWidth, metrics.headerHeight), header);

  if (_tasks.empty()) {
    GUI.drawPopup(renderer, tr(STR_TODOIST_NO_TASKS));
    return;
  }

  const int contentTop = metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight;

  GUI.drawList(
      renderer, Rect(0, contentTop, pageWidth, contentHeight),
      static_cast<int>(_tasks.size()),
      _scrollOffset,
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
      nullptr,
      nullptr,
      [this](int index) -> std::string {
        char buf[8];
        snprintf(buf, sizeof(buf), "p%u",
                 static_cast<unsigned>(_tasks[index].priority));
        return std::string(buf);
      },
      false);
}
