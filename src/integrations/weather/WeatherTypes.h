#pragma once

#include <cstdint>
#include <cstddef>

namespace weather {

enum class TemperatureUnit : uint8_t {
  Celsius = 0,
  Fahrenheit = 1,
};

enum class FetchResult {
  Ok,
  NetworkError,   // timeout, TLS, transport, OOM in response buffer
  RateLimited,    // 429 (ipapi.co caps free tier at ~1k/day per IP)
  ServerError,    // 5xx
  ParseError,     // missing/invalid JSON fields
};

// Single-day forecast snapshot used by the Daily Todoist view.
// hi/lo are already in the requested unit; the caller decides the °C/°F
// suffix to render. wmoCode maps to a short English label via
// wmoCodeToLabel() — no localisation in v1 (matches the day-of-week
// strings in TodoistActivity).
struct Forecast {
  bool valid = false;
  int hi = 0;
  int lo = 0;
  uint8_t wmoCode = 0;
  TemperatureUnit unit = TemperatureUnit::Celsius;
  // Free-form city name from the IP geolocator (e.g. "Mountain View",
  // "São Paulo"). UTF-8. Kept on the Forecast and not just in the
  // config so the renderer has a single source for "what's on screen".
  char locationName[32] = {};
};

// English label for an Open-Meteo / WMO weather code. Returns a short
// string (<= ~14 chars) suitable for a single-line dashboard slot.
// Unknown codes fall back to "—".
const char* wmoCodeToLabel(uint8_t code);

// 16x16 1-bit packed icon bitmap for a WMO code. Returns nullptr for
// codes we don't have a category icon for (caller should skip drawing).
// Bitmaps live in src/components/icons/weather.h.
const uint8_t* wmoCodeToIcon(uint8_t code);

// Canonical wire string for the temperature unit ("celsius" / "fahrenheit").
// Matches the Open-Meteo `temperature_unit` query parameter exactly so the
// same value can be used as the JSON persist value and as the API arg.
const char* temperatureUnitToString(TemperatureUnit u);

// One-character suffix for display ('C' or 'F').
char temperatureUnitSuffix(TemperatureUnit u);

}  // namespace weather
