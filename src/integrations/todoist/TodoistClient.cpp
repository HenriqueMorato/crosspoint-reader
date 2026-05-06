#include "TodoistClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace todoist {

namespace {

constexpr const char* kEndpointBase =
    "https://api.todoist.com/api/v1/tasks/filter?query=";
constexpr int kHttpTimeoutMs = 15000;
// Keep HTTP rx/tx buffers small. mbedTLS handshake on ESP32-C3 needs ~32 KB
// of heap on top of these — every KB we free here is one mbedTLS can take.
constexpr size_t kHttpBufSize = 2048;
constexpr size_t kMaxTasks = 64;
// Hard cap on the response body. Allocated once via malloc() and never
// reallocated: std::string's geometric growth (2x) creates transient memory
// spikes that trip OOM on the C3 when responses get large (e.g. the
// ThisMonth filter for an active account). A fixed buffer trades a higher
// steady-state footprint for predictability.
constexpr size_t kMaxResponseBytes = 32 * 1024;

struct ResponseBuffer {
  char* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;
  bool truncated = false;
  bool allocFailed = false;
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (!buf || !evt->data || evt->data_len <= 0) return ESP_OK;
  // Lazy allocation: claim the response buffer only after the TLS handshake
  // has run and freed its scratch. Allocating up-front leaves mbedtls without
  // a contiguous block big enough for SSL setup on the C3 (~40KB), even
  // when total free heap looks healthy.
  if (!buf->data) {
    if (buf->allocFailed) return ESP_OK;
    buf->data = static_cast<char*>(malloc(buf->capacity));
    if (!buf->data) {
      buf->allocFailed = true;
      LOG_ERR("TDST", "OOM allocating %u-byte response buffer",
              static_cast<unsigned>(buf->capacity));
      return ESP_OK;
    }
  }
  size_t len = static_cast<size_t>(evt->data_len);
  size_t avail = buf->capacity - buf->size;
  if (len > avail) {
    buf->truncated = true;
    len = avail;
    if (len == 0) return ESP_OK;
  }
  memcpy(buf->data + buf->size, evt->data, len);
  buf->size += len;
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

// Format `today + daysAhead` as YYYY-MM-DD in the device's local timezone.
// Caller must have already verified the clock is set (via NTP).
std::string formatLocalDate(int daysAhead) {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  tm.tm_mday += daysAhead;
  mktime(&tm);  // normalises across month/year boundaries
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return std::string(buf);
}

// Days from today to the upcoming Monday (1..7). ISO week ends on Sunday,
// so the strict `due before:` cutoff for "this week" is next Monday. If
// today is Monday we want a full week ahead, not zero — 0 maps to 7.
int daysUntilNextMonday() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  // tm_wday: 0=Sun, 1=Mon, ..., 6=Sat
  int days = (1 - tm.tm_wday + 7) % 7;
  return days == 0 ? 7 : days;
}

// First day of next calendar month, YYYY-MM-DD.
std::string firstOfNextMonth() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  tm.tm_mon += 1;
  tm.tm_mday = 1;
  mktime(&tm);  // normalises December → January roll-over
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return std::string(buf);
}

// Percent-encode for use in a URL query value.
std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  static const char hex[] = "0123456789ABCDEF";
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') ||
        (uc >= '0' && uc <= '9') ||
        uc == '-' || uc == '_' || uc == '.' || uc == '~') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(hex[uc >> 4]);
      out.push_back(hex[uc & 0x0F]);
    }
  }
  return out;
}

// Build the unencoded Todoist filter query for the given two-axis selection.
//
// Each axis emits an independent fragment, OR-combined. The date fragment
// is responsible for excluding overdue (range filters add `due after:
// yesterday`), so the overdue fragment is purely additive — None means
// "don't add an overdue clause."
std::string buildQuery(DateFilter dateF, OverdueFilter overdueF) {
  std::string date;
  switch (dateF) {
    case DateFilter::None:  break;
    case DateFilter::Today: date = "today"; break;
    case DateFilter::ThisWeek:
      date = "due after: yesterday & due before: " +
             formatLocalDate(daysUntilNextMonday());
      break;
    case DateFilter::ThisMonth:
      date = "due after: yesterday & due before: " + firstOfNextMonth();
      break;
  }

  std::string overdue;
  switch (overdueF) {
    case OverdueFilter::None:                                                break;
    case OverdueFilter::Last7Days: overdue = "overdue & due after: -7 days"; break;
    case OverdueFilter::All:       overdue = "overdue";                      break;
  }

  if (date.empty() && overdue.empty()) return "today";  // degenerate fallback
  if (date.empty())    return overdue;
  if (overdue.empty()) return date;
  return "(" + date + ") | (" + overdue + ")";
}

}  // namespace

FetchResult TodoistClient::fetch(const std::string& apiToken,
                                 DateFilter dateFilter,
                                 OverdueFilter overdueFilter,
                                 std::vector<TodoistTask>& outTasks) {
  outTasks.clear();
  outTasks.reserve(kMaxTasks);

  if (apiToken.empty()) {
    LOG_ERR("TDST", "Empty token");
    return FetchResult::InvalidToken;
  }

  const std::string query = buildQuery(dateFilter, overdueFilter);
  const std::string url = std::string(kEndpointBase) + urlEncode(query);
  LOG_DBG("TDST", "Query: %s", query.c_str());

  // Response buffer descriptor. The actual char[] is allocated lazily inside
  // the event handler on first ON_DATA — after the TLS handshake has run and
  // mbedtls has released its handshake scratch. Pre-allocating here
  // fragments the heap and breaks SSL setup on the C3 (-0x7F00 alloc fail).
  ResponseBuffer buf;
  buf.capacity = kMaxResponseBytes;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
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
    free(buf.data);
    return FetchResult::NetworkError;
  }

  std::string authHeader = "Bearer " + apiToken;
  if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    LOG_ERR("TDST", "Set header failed");
    esp_http_client_cleanup(client);
    free(buf.data);
    return FetchResult::NetworkError;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("TDST", "HTTP %d (err=%d, %u bytes%s)",
          httpCode, err, static_cast<unsigned>(buf.size),
          buf.truncated ? " [truncated]" : "");

  if (err != ESP_OK) {
    free(buf.data);
    return FetchResult::NetworkError;
  }
  if (buf.allocFailed) {
    // Lazy malloc inside the event handler couldn't claim the response
    // buffer even after TLS — probably a heap-pressure condition. Surface
    // as NetworkError; the body is unrecoverable at this point.
    free(buf.data);
    return FetchResult::NetworkError;
  }
  FetchResult statusResult = httpStatusToFetchResult(httpCode);
  if (statusResult != FetchResult::Ok) {
    free(buf.data);
    return statusResult;
  }

  if (buf.truncated) {
    LOG_ERR("TDST", "Response truncated at cap (%u bytes)",
            static_cast<unsigned>(buf.capacity));
    // Not fatal — try to parse what we have. Worst case ParseError below.
  }

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, buf.data, buf.size);
  // We're done with the raw bytes — ArduinoJson has already copied what it
  // needs into its own document. Free now so heap is available for the
  // task-list std::vector growth and any UI work that follows.
  free(buf.data);
  buf.data = nullptr;
  if (parseErr) {
    LOG_ERR("TDST", "JSON parse: %s", parseErr.c_str());
    return FetchResult::ParseError;
  }
  // v1 wraps tasks in { "results": [...], "next_cursor": null|string }.
  if (!doc["results"].is<JsonArray>()) {
    LOG_ERR("TDST", "Response missing 'results' array");
    return FetchResult::ParseError;
  }
  if (!doc["next_cursor"].isNull()) {
    LOG_DBG("TDST", "Truncated: more tasks available via cursor");
  }

  JsonArray arr = doc["results"].as<JsonArray>();
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
    // Copy YYYY-MM-DD prefix from `date` (always present when `due` exists);
    // sort comparator below relies on lexicographic order = chronological order.
    t.dueDate[0] = '\0';
    if (date && strlen(date) >= 10) {
      memcpy(t.dueDate, date, 10);
      t.dueDate[10] = '\0';
    }
    t.overdue = isOverdue(date);

    outTasks.push_back(t);
  }

  LOG_DBG("TDST", "Parsed %u tasks", static_cast<unsigned>(outTasks.size()));
  return FetchResult::Ok;
}

}  // namespace todoist
