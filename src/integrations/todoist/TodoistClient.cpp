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
