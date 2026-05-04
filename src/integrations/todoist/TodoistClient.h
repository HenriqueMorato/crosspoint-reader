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
