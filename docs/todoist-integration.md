# Todoist Integration

Pull your Todoist tasks onto the device and (optionally) render them as the
sleep-screen wallpaper, so the last thing you see when you put the reader
down is what you still owe yourself.

The integration is read-only: it fetches tasks over HTTPS and renders them.
Tasks are not created, edited, or completed from the device.

---

## Prerequisites

- An e-reader running CrossPoint Reader firmware that includes this branch
  (`feature/todoist-integration` or any release that merged it).
- A Todoist account with a personal API token.
- A WiFi network the device can reach (configured via the existing WiFi
  selection flow — Settings → WiFi).
- An SD card with the `/.crosspoint/` directory writable. The firmware
  creates the directory if it doesn't exist; nothing else is required.

---

## 1. Get a Todoist API token

1. Log in to <https://app.todoist.com/app/settings/integrations/developer>.
2. Copy the **API token** at the bottom of the page.

This is a personal token tied to your account. Treat it like a password:

- Don't paste it into screenshots, issues, or pull requests.
- The device stores it on the SD card in plain text. If you lend the SD
  card to someone, run **Settings → Todoist → Forget Todoist** first, or
  delete `/.crosspoint/todoist.json` manually.

---

## 2. Drop a config file on the SD card

The integration reads its config from `/.crosspoint/todoist.json` on the
SD card. The on-device Settings UI persists the same file, but on the
first run you typically place it manually — there is no on-device token
entry yet.

### Minimal config

The only required field is `api_token`. Everything else falls back to a
sensible default.

```json
{
  "api_token": "PASTE_YOUR_TOKEN_HERE"
}
```

### Full config (every supported field)

```json
{
  "api_token": "PASTE_YOUR_TOKEN_HERE",
  "sleep_screen_enabled": true,
  "activity_orientation": "portrait",
  "snapshot_orientation": "landscape_cw",
  "date_filter": "today",
  "overdue_filter": "last_7_days",
  "gmt_offset": -3
}
```

### Field reference

| Field | Type | Default | Allowed values |
|---|---|---|---|
| `api_token` | string | `""` | Your Todoist personal API token. Considered valid when ≥20 chars. |
| `sleep_screen_enabled` | bool | `false` | When `true` and a snapshot exists, the sleep screen renders the cached task list instead of the default rotation. |
| `activity_orientation` | string | `"portrait"` | `"portrait"`, `"portrait_inverted"`, `"landscape_cw"`, `"landscape_ccw"` — orientation used while you're viewing the activity. |
| `snapshot_orientation` | string | `"portrait"` | Same set as above. The orientation the saved sleep-screen BMP is rendered in. Only matters if `sleep_screen_enabled` is `true`. |
| `date_filter` | string | `"today"` | `"none"`, `"today"`, `"this_week"`, `"this_month"`. |
| `overdue_filter` | string | `"last_7_days"` | `"none"`, `"last_7_days"`, `"all"`. Combined additively with `date_filter`. |
| `gmt_offset` | int | `0` | Whole-hour offset from GMT, range `-12..+14`. Out-of-range values clamp on load. |

The file is bounded at 4 KB — parsing aborts and the integration disables
itself if the file is larger.

---

## 3. Build and flash the firmware

```bash
# Build firmware (default environment)
pio run

# Build and upload to the device
pio run -t upload

# Combined upload + serial monitor
pio run -t upload && pio device monitor
```

Filesystem images do not need to be uploaded — the integration only writes
to the SD card, never to SPIFFS.

If you have a serial port set in `platformio.local.ini`, the upload picks
it up automatically. Otherwise pass it explicitly:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem2101  # macOS
pio run -t upload --upload-port COM7                  # Windows
```

For a release build (smaller binary, no debug logging):

```bash
pio run -e gh_release -t upload
```

---

## 4. First run on device

1. Insert the SD card with `/.crosspoint/todoist.json` in place.
2. Power on the device, connect to WiFi via Settings → WiFi.
3. From the home screen menu, open **Todoist**.
4. The activity brings up WiFi, syncs NTP, and fetches your tasks. Expect
   2–8 seconds before the list appears, dominated by TLS handshake.
5. Press **Confirm** to refresh, **Up/Down** to scroll, **Back** to exit.

After a successful fetch with `sleep_screen_enabled: true`, the device
also writes:

- `/.crosspoint/todoist_sleep.bmp` — the rendered task list as a bitmap
- `/.crosspoint/todoist_sleep.meta` — JSON sidecar with capture time and
  orientation

When the device sleeps, `SleepActivity` checks for these files and, if
the recorded orientation matches the one configured in `snapshot_orientation`,
renders the cached BMP as the sleep wallpaper.

---

## 5. Configure on device

**Settings → Todoist** exposes the same fields stored in `todoist.json`,
each with atomic write-through to the file:

| Row | What it does |
|---|---|
| Sleep screen | Toggles `sleep_screen_enabled`. |
| Activity orientation | Cycles `activity_orientation` (P → LCW → PI → LCCW). |
| Sleep screen orientation | Cycles `snapshot_orientation`. |
| Date filter | Cycles `date_filter` (None / Today / This week / This month). |
| Overdue | Cycles `overdue_filter` (None / Last 7 days / All). |
| Timezone | Cycles `gmt_offset` from −12 through +14 and wraps. |
| Forget Todoist | Removes `todoist.json`, the snapshot BMP and meta, and any orphaned `.tmp` files. **No confirmation prompt — single press wipes the token.** |

The token itself can only be set by editing `todoist.json` on the SD card.
On-device token entry is on the v2 wishlist.

---

## File reference

All Todoist state lives under `/.crosspoint/` on the SD card.

| Path | Purpose |
|---|---|
| `/.crosspoint/todoist.json` | Config, including the API token. |
| `/.crosspoint/todoist.json.tmp` | Atomic-write tmp file. Cleaned up on success or on the next `forget()`. |
| `/.crosspoint/todoist_sleep.bmp` | Last successful task-list render (sleep-screen source). |
| `/.crosspoint/todoist_sleep.meta` | Capture time + orientation for the BMP. |
| `/.crosspoint/todoist_sleep.meta.tmp` | Atomic-write tmp for the meta. |

---

## Troubleshooting

### "No Todoist token configured"

`todoist.json` is missing, unparseable, or `api_token` is empty / shorter
than 20 chars. Check the file with `cat`/`type` and verify it's valid JSON.

### "Invalid token"

Todoist returned 401/403. The token was revoked, mistyped, or has trailing
whitespace. Regenerate it on the Todoist developer settings page and
replace `api_token`.

### "WiFi unavailable"

WiFi failed to come up within the activity's timeout. Reconnect via
Settings → WiFi and retry.

### "Could not fetch tasks"

Generic HTTPS or parse failure. Check the serial monitor for the
underlying error:

```bash
pio device monitor
```

The most common one is `MBEDTLS_ERR_SSL_ALLOC_FAILED (-0x7F00)` —
mbedTLS couldn't claim the ~32 KB it needs for the handshake. Free heap
by closing other activities first; the integration already releases the
home-screen cover buffer before connecting.

### Sleep screen doesn't show my tasks

Check, in order:

1. `sleep_screen_enabled` is `true` in the config.
2. A successful fetch has happened since the last `forget()` (the BMP
   exists at `/.crosspoint/todoist_sleep.bmp`).
3. The device's current sleep-screen orientation matches `snapshot_orientation`
   from the meta file — the firmware refuses to render a portrait BMP on a
   landscape sleep screen and silently falls back to the default rotation.

The serial log prints which precondition failed when sleep is entered:

```
TDST: tryRenderTodoistSleepScreen: <reason>
```

### Stale snapshot

The header on the activity reads `Updated dd/mm - HH:MM` so you can spot a
stale capture at a glance. Press **Confirm** to refresh; a fresh fetch
overwrites both the BMP and the meta.

---

## Privacy and data flow

- Task titles and due dates leave Todoist, hit the device over TLS, and
  are stored unencrypted on the SD card (in the BMP for the sleep
  screen, in heap-only structs while the activity is running).
- Nothing is uploaded anywhere else.
- `Forget Todoist` removes every Todoist-owned file from the SD card.
  WiFi credentials and unrelated settings are not touched.
