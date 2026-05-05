#pragma once

#include "TodoistConfig.h"
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
  // Synchronous fetch. Caller must guarantee WiFi is up and that the system
  // clock is set (NTP) — calendar-bounded date filters compute their cutoff
  // from time(nullptr). Reserves out capacity to a sane upper bound and
  // truncates extras silently. Never blocks indefinitely (15s HTTP timeout).
  static FetchResult fetch(const std::string& apiToken,
                           DateFilter dateFilter,
                           OverdueFilter overdueFilter,
                           std::vector<TodoistTask>& outTasks);
};

}  // namespace todoist
