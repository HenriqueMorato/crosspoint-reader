#include "WeatherClient.h"

#include "components/icons/weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace weather {

namespace {

// Open-Meteo's free geocoding endpoint. Same TLS host family as the
// forecast endpoint — sharing the DNS+cert path avoids the EAI_FAIL DNS
// storm we hit with ipapi.co/ipwho.is. IP geolocation was abandoned in
// v1 because it's unreliable for any user on Starlink, CGNAT, or VPN
// (Starlink in particular routes through a regional ground station, so
// every IP database pins the user at the station's city, not their
// physical location). Manual city entry is the universal answer.
//
// Schema we consume:
//   { "results": [{"name":"Curitiba", "latitude":-25.43, "longitude":-49.27, ...}] }
//   { "generationtime_ms": 0.5 }  // when nothing matches → no "results" key
constexpr const char* kGeocodeEndpointFmt =
    "https://geocoding-api.open-meteo.com/v1/search"
    "?name=%s&count=1&language=en&format=json";

// Open-Meteo forecast: today only, daily aggregates we need, timezone=auto
// so the daily bucket aligns with the user's wall clock (otherwise UTC
// midnight clips one side of "today" depending on longitude).
constexpr const char* kForecastEndpointFmt =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&forecast_days=1&timezone=auto&temperature_unit=%s";

constexpr int kHttpTimeoutMs = 15000;
// Same rationale as TodoistClient: keep TX/RX buffers small so mbedTLS has
// enough heap left for the handshake. ESP32-C3 has no slack here.
constexpr size_t kHttpBufSize = 2048;
// Both endpoints' bodies are well under 4 KB in practice. Geocoding with
// count=1 returns ~400 bytes; Open-Meteo's daily-only forecast is ~300.
constexpr size_t kMaxResponseBytes = 4 * 1024;

// Percent-encode `in` into `out` using the unreserved-set rule from RFC
// 3986 (everything outside `A-Z a-z 0-9 - _ . ~` becomes %XX). Always
// writes a NUL terminator. Returns false on overflow. Used to safely
// embed a UTF-8 city name in the geocoding URL — non-ASCII bytes
// (accented characters, Cyrillic, CJK) are encoded byte-by-byte so the
// caller never needs to know the codepoint structure.
bool urlEncode(const char* in, char* out, size_t outSize) {
  if (outSize == 0) return false;
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (unreserved) {
      if (o + 1 >= outSize) return false;
      out[o++] = static_cast<char>(c);
    } else {
      if (o + 3 >= outSize) return false;
      static const char kHex[] = "0123456789ABCDEF";
      out[o++] = '%';
      out[o++] = kHex[(c >> 4) & 0xF];
      out[o++] = kHex[c & 0xF];
    }
  }
  out[o] = '\0';
  return true;
}

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
  // Lazy alloc after TLS handshake completes — see TodoistClient for the
  // full reasoning. Allocating up-front fragments the heap on the C3 and
  // breaks mbedTLS setup even when free heap looks fine.
  if (!buf->data) {
    if (buf->allocFailed) return ESP_OK;
    buf->data = static_cast<char*>(malloc(buf->capacity));
    if (!buf->data) {
      buf->allocFailed = true;
      size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      LOG_ERR("WTHR", "OOM allocating %u-byte response buffer (free=%u, largest=%u)",
              static_cast<unsigned>(buf->capacity),
              static_cast<unsigned>(freeBytes),
              static_cast<unsigned>(largest));
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

FetchResult httpStatusToFetchResult(int code) {
  if (code == 200) return FetchResult::Ok;
  if (code == 429) return FetchResult::RateLimited;
  if (code >= 500) return FetchResult::ServerError;
  return FetchResult::NetworkError;
}

// Round half-away-from-zero so a forecast of 21.5 renders as 22, not 21
// (banker's rounding via int() truncation would be 21). Matches what most
// weather apps display.
int roundTemp(double v) {
  if (!std::isfinite(v)) return 0;
  return static_cast<int>(v >= 0 ? std::floor(v + 0.5) : std::ceil(v - 0.5));
}

// One HTTPS GET attempt. Caller owns `outBuf.data` on Ok (must free); on
// non-Ok the buffer is freed here so the caller never has to.
FetchResult performGetOnce(const char* url, ResponseBuffer& outBuf) {
  outBuf.capacity = kMaxResponseBytes;

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = &outBuf;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = kHttpTimeoutMs;
  config.buffer_size = kHttpBufSize;
  config.buffer_size_tx = kHttpBufSize;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("WTHR", "esp_http_client_init failed");
    return FetchResult::NetworkError;
  }
  // Accept JSON; without it Open-Meteo still returns JSON but ipapi.co's
  // edge sometimes 406s if no Accept header is present.
  esp_http_client_set_header(client, "Accept", "application/json");

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("WTHR", "GET HTTP %d (err=%d, %u bytes%s)", httpCode, err,
          static_cast<unsigned>(outBuf.size),
          outBuf.truncated ? " [truncated]" : "");

  if (err != ESP_OK || outBuf.allocFailed) {
    free(outBuf.data);
    outBuf.data = nullptr;
    return FetchResult::NetworkError;
  }
  FetchResult status = httpStatusToFetchResult(httpCode);
  if (status != FetchResult::Ok) {
    free(outBuf.data);
    outBuf.data = nullptr;
    return status;
  }
  return FetchResult::Ok;
}

// Wraps performGetOnce with retries on NetworkError. Empirically, DNS
// queries after a fresh TLS connection close (e.g. Todoist just finished)
// can return EAI_FAIL (202) for several seconds on the C3's lwIP stack —
// the resolver gets wedged and needs time to recover. We retry up to 3
// times with exponential backoff (1s, 2s, 4s) which covers the worst
// case seen in practice. RateLimited / ServerError / ParseError are not
// retried — they're authoritative answers from the far end.
FetchResult performGet(const char* url, ResponseBuffer& outBuf) {
  constexpr int kBackoffsMs[] = {1000, 2000, 4000};
  outBuf = ResponseBuffer{};
  FetchResult r = performGetOnce(url, outBuf);
  if (r != FetchResult::NetworkError) return r;
  for (int i = 0; i < 3; ++i) {
    LOG_DBG("WTHR", "NetworkError, retry %d/3 after %d ms", i + 1, kBackoffsMs[i]);
    vTaskDelay(kBackoffsMs[i] / portTICK_PERIOD_MS);
    outBuf = ResponseBuffer{};
    r = performGetOnce(url, outBuf);
    if (r != FetchResult::NetworkError) return r;
  }
  return r;
}

}  // namespace

FetchResult WeatherClient::geocodeCity(const char* nameUtf8, double& outLat,
                                      double& outLon, char* outCanonical,
                                      size_t canonicalSize) {
  if (canonicalSize == 0 || !nameUtf8 || nameUtf8[0] == '\0') {
    return FetchResult::ParseError;
  }
  outCanonical[0] = '\0';
  outLat = 0;
  outLon = 0;

  // Encode the user-typed name. Worst case: every byte expands 3×, so for
  // a 63-char input we need 189+1 bytes. 256 leaves headroom.
  char encoded[256];
  if (!urlEncode(nameUtf8, encoded, sizeof(encoded))) {
    LOG_ERR("WTHR", "URL-encode overflow for city name");
    return FetchResult::ParseError;
  }

  // Final URL: endpoint format (~80 chars) + encoded name. 384 covers any
  // reasonable city length including diacritics.
  char url[384];
  int n = snprintf(url, sizeof(url), kGeocodeEndpointFmt, encoded);
  if (n <= 0 || n >= static_cast<int>(sizeof(url))) {
    LOG_ERR("WTHR", "Geocode URL format failed");
    return FetchResult::NetworkError;
  }

  ResponseBuffer buf;
  FetchResult get = performGet(url, buf);
  if (get != FetchResult::Ok) return get;

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, buf.data, buf.size);
  free(buf.data);
  buf.data = nullptr;
  if (parseErr) {
    LOG_ERR("WTHR", "geocode parse: %s", parseErr.c_str());
    return FetchResult::ParseError;
  }
  // Zero results = unknown city / typo. Open-Meteo simply omits the
  // "results" key entirely in that case. Surface it as ParseError so the
  // caller can show a distinct "city not found" message rather than a
  // network failure.
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) {
    LOG_ERR("WTHR", "geocode: no results for '%s'", nameUtf8);
    return FetchResult::ParseError;
  }
  JsonObject first = results[0].as<JsonObject>();
  if (!first["latitude"].is<double>() || !first["longitude"].is<double>()) {
    LOG_ERR("WTHR", "geocode missing lat/lon in results[0]");
    return FetchResult::ParseError;
  }
  outLat = first["latitude"].as<double>();
  outLon = first["longitude"].as<double>();
  const char* canonical = first["name"] | nameUtf8;
  size_t len = strlen(canonical);
  if (len >= canonicalSize) len = canonicalSize - 1;
  memcpy(outCanonical, canonical, len);
  outCanonical[len] = '\0';
  LOG_DBG("WTHR", "Geocoded '%s' → %.4f,%.4f (%s)", nameUtf8, outLat, outLon,
          outCanonical);
  return FetchResult::Ok;
}

FetchResult WeatherClient::fetchForecast(double lat, double lon,
                                        TemperatureUnit unit,
                                        Forecast& out) {
  out.valid = false;

  char url[192];
  int n = snprintf(url, sizeof(url), kForecastEndpointFmt, lat, lon,
                   temperatureUnitToString(unit));
  if (n <= 0 || n >= static_cast<int>(sizeof(url))) {
    LOG_ERR("WTHR", "URL format failed");
    return FetchResult::NetworkError;
  }

  ResponseBuffer buf;
  FetchResult get = performGet(url, buf);
  if (get != FetchResult::Ok) return get;

  JsonDocument doc;
  auto parseErr = deserializeJson(doc, buf.data, buf.size);
  free(buf.data);
  buf.data = nullptr;
  if (parseErr) {
    LOG_ERR("WTHR", "forecast parse: %s", parseErr.c_str());
    return FetchResult::ParseError;
  }
  JsonObject daily = doc["daily"].as<JsonObject>();
  if (daily.isNull()) {
    LOG_ERR("WTHR", "forecast missing 'daily'");
    return FetchResult::ParseError;
  }
  // Each daily field is an array with forecast_days entries — we asked for 1,
  // so we read index 0 from each. If the array is missing or empty we bail
  // rather than ship a half-populated Forecast.
  JsonArray wc = daily["weather_code"].as<JsonArray>();
  JsonArray hi = daily["temperature_2m_max"].as<JsonArray>();
  JsonArray lo = daily["temperature_2m_min"].as<JsonArray>();
  if (wc.size() == 0 || hi.size() == 0 || lo.size() == 0) {
    LOG_ERR("WTHR", "forecast daily arrays empty");
    return FetchResult::ParseError;
  }

  out.wmoCode = static_cast<uint8_t>(wc[0].as<int>() & 0xFF);
  out.hi = roundTemp(hi[0].as<double>());
  out.lo = roundTemp(lo[0].as<double>());
  out.unit = unit;
  out.valid = true;
  LOG_DBG("WTHR", "Forecast: wmo=%u hi=%d lo=%d", out.wmoCode, out.hi, out.lo);
  return FetchResult::Ok;
}

// ----- WeatherTypes free functions -----

const char* wmoCodeToLabel(uint8_t code) {
  // WMO 4677 codes as exposed by Open-Meteo. Short labels chosen to fit on
  // the Daily dashboard (single line, no truncation in portrait).
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Showers";
    case 81: return "Heavy showers";
    case 82: return "Violent showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm w/ hail";
    default: return "\xE2\x80\x94";  // U+2014 em dash
  }
}

const uint8_t* wmoCodeToIcon(uint8_t code) {
  // Six visual buckets, mirroring wmoCodeToLabel's groupings. Codes
  // outside these buckets (e.g. unknown / malformed) fall through to
  // nullptr so the caller can render text-only without a placeholder.
  switch (code) {
    case 0:
    case 1:
      return WeatherSunIcon;
    case 2:
    case 3:
      return WeatherCloudIcon;
    case 45:
    case 48:
      return WeatherFogIcon;
    case 51: case 53: case 55:
    case 56: case 57:
    case 61: case 63: case 65:
    case 66: case 67:
    case 80: case 81: case 82:
      return WeatherRainIcon;
    case 71: case 73: case 75: case 77:
    case 85: case 86:
      return WeatherSnowIcon;
    case 95: case 96: case 99:
      return WeatherStormIcon;
    default:
      return nullptr;
  }
}

const char* temperatureUnitToString(TemperatureUnit u) {
  return (u == TemperatureUnit::Fahrenheit) ? "fahrenheit" : "celsius";
}

char temperatureUnitSuffix(TemperatureUnit u) {
  return (u == TemperatureUnit::Fahrenheit) ? 'F' : 'C';
}

}  // namespace weather
