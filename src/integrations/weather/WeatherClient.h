#pragma once

#include "WeatherTypes.h"

#include <cstddef>

namespace weather {

class WeatherClient {
 public:
  // City-name geocoding via Open-Meteo's free geocoding API
  // (https://geocoding-api.open-meteo.com/v1/search?name=...&count=1). On
  // success, writes the resolved lat/lon plus the canonical UTF-8 city
  // name returned by the API (e.g. user types "curitiba", canonical is
  // "Curitiba"). canonicalSize must be >= 1; outCanonical is always
  // NUL-terminated on Ok. nameUtf8 must be a NUL-terminated UTF-8 string
  // of length 1..63; non-ASCII characters are percent-encoded before being
  // placed in the URL. ParseError is returned when the API has zero
  // results (typo / unknown city) so the caller can distinguish that from
  // a network failure.
  static FetchResult geocodeCity(const char* nameUtf8, double& outLat,
                                 double& outLon, char* outCanonical,
                                 size_t canonicalSize);

  // Fetches today's daily forecast (high/low + WMO code) from Open-Meteo
  // for the given coordinates and unit. Synchronous; one HTTPS call.
  // `out.locationName` is NOT populated here — the caller (TodoistActivity)
  // copies it in from the cached config or from geolocateByIP. On any
  // non-Ok return, `out.valid` is left false.
  static FetchResult fetchForecast(double lat, double lon,
                                   TemperatureUnit unit,
                                   Forecast& out);
};

}  // namespace weather
