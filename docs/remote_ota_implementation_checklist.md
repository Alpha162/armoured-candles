# Remote OTA + Auto-Versioning Implementation Checklist

This is a concrete coding checklist for `armoured-candles.ino`, mapped to the existing function names and sections in your firmware.

## 0) Scope and design assumptions

- Keep existing browser upload OTA (`/api/update`) intact as a manual fallback.
- Add **optional** remote OTA polling driven by GUI config.
- Poll a manifest (`.json`) over HTTP(S), compare to `FW_VERSION`, then stream OTA if newer and valid.
- Add a GitHub Action to bump build/patch number on merges.

---

## 1) Add new persisted config fields (globals + defaults)

In the **global config block** (near `cfgSSID`, `cfgRefreshMin`, etc), add:

```cpp
bool     cfgRemoteOtaEnabled = false;
char     cfgRemoteOtaUrl[192] = "";          // manifest URL
int      cfgRemoteOtaCheckMin = 60;          // poll interval
bool     cfgRemoteOtaAutoApply = true;       // false => notify only
char     cfgRemoteOtaChannel[12] = "stable";
bool     cfgRemoteOtaAllowDowngrade = false;
```

Add runtime timing/state near other state globals:

```cpp
unsigned long lastRemoteOtaCheckMs = 0;
unsigned long remoteOtaBackoffUntilMs = 0;
int      remoteOtaConsecutiveFails = 0;
String   remoteOtaLastError = "";
String   remoteOtaLastSeenVersion = "";
```

---

## 2) Extend NVS load/save (`loadConfig()` / `saveConfig()`)

In `saveConfig()` add writes:

```cpp
prefs.putBool("rota_en", cfgRemoteOtaEnabled);
prefs.putString("rota_url", cfgRemoteOtaUrl);
prefs.putInt("rota_min", cfgRemoteOtaCheckMin);
prefs.putBool("rota_auto", cfgRemoteOtaAutoApply);
prefs.putString("rota_chan", cfgRemoteOtaChannel);
prefs.putBool("rota_down", cfgRemoteOtaAllowDowngrade);
```

In `loadConfig()` add reads with defaults and bounds:

```cpp
cfgRemoteOtaEnabled = prefs.getBool("rota_en", false);
copyBounded(cfgRemoteOtaUrl, sizeof(cfgRemoteOtaUrl), prefs.getString("rota_url", "").c_str());
cfgRemoteOtaCheckMin = constrain(prefs.getInt("rota_min", 60), 5, 1440);
cfgRemoteOtaAutoApply = prefs.getBool("rota_auto", true);
copyBounded(cfgRemoteOtaChannel, sizeof(cfgRemoteOtaChannel), prefs.getString("rota_chan", "stable").c_str());
cfgRemoteOtaAllowDowngrade = prefs.getBool("rota_down", false);
```

Validation helpers to add near existing input validators:

- `bool isValidHttpUrl(const char* s)`
- `bool isValidChannel(const char* s)`

---

## 3) Add fields to API status/config handlers

### `handleStatus()`

Add to JSON response:

```cpp
doc["remoteOtaEnabled"] = cfgRemoteOtaEnabled;
doc["remoteOtaUrl"] = canViewSensitive ? cfgRemoteOtaUrl : "";
doc["remoteOtaCheckMin"] = cfgRemoteOtaCheckMin;
doc["remoteOtaAutoApply"] = cfgRemoteOtaAutoApply;
doc["remoteOtaChannel"] = cfgRemoteOtaChannel;
doc["remoteOtaAllowDowngrade"] = cfgRemoteOtaAllowDowngrade;
doc["remoteOtaConsecutiveFails"] = remoteOtaConsecutiveFails;
doc["remoteOtaLastError"] = remoteOtaLastError;
doc["remoteOtaLastSeenVersion"] = remoteOtaLastSeenVersion;
```

### `handleConfigPost()`

Parse and validate new optional keys:

- `remoteOtaEnabled` (bool)
- `remoteOtaUrl` (string, must be `http://` or `https://`, bounded)
- `remoteOtaCheckMin` (int, constrain 5–1440)
- `remoteOtaAutoApply` (bool)
- `remoteOtaChannel` (`stable|beta|edge` or your custom set)
- `remoteOtaAllowDowngrade` (bool)

After updating fields, call `saveConfig()` (already present in flow).

---

## 4) Add GUI controls in `HTML_PAGE`

In firmware update section, add new controls:

- Toggle: **Enable Remote OTA Polling**
- Input: **Manifest URL**
- Number input: **Check every X minutes**
- Toggle: **Auto-apply when newer found**
- Select/text: **Channel**
- Toggle: **Allow Downgrade** (advanced)

### JS wiring

In existing `loadConfig()` frontend logic:

- read values from `/api/status`
- set new controls from response

In `saveConfig()` frontend payload:

- include all new remote OTA keys in JSON body

Add light client validation for URL + interval before POST.

---

## 5) Manifest model + parser

Add structs near your other small structs:

```cpp
struct RemoteManifest {
  char version[24];
  char url[192];
  char sha256[65];
  size_t size;
  char board[32];
  char channel[12];
  bool valid;
};
```

Add helper functions:

- `bool fetchRemoteManifest(RemoteManifest& out)`
- `bool parseManifestJson(const String& payload, RemoteManifest& out)`
- `bool isManifestForThisDevice(const RemoteManifest& m)`

Recommended manifest fields:

```json
{
  "board": "xiao-esp32s3-epd75",
  "channel": "stable",
  "version": "1.3.0",
  "url": "https://updates.example.com/firmware-1.3.0.bin",
  "size": 1432576,
  "sha256": "...64 hex..."
}
```

---

## 6) Version comparison helper

Add:

- `int compareSemver(const char* a, const char* b)`

Behavior:

- returns `<0` if `a < b`, `0` equal, `>0` if `a > b`
- support optional leading `v` in both strings
- compare numeric major/minor/patch (ignore suffixes initially)

Use it against `FW_VERSION`.

---

## 7) Remote OTA execution path

Add core function:

- `bool performRemoteOta(const RemoteManifest& m)`

Implementation outline:

1. `HTTPClient http; http.begin(m.url);`
2. `GET`, verify `200`.
3. Optional verify `Content-Length == m.size`.
4. `Update.begin(UPDATE_SIZE_UNKNOWN)`.
5. Stream body in chunks:
   - read from `WiFiClient* stream = http.getStreamPtr();`
   - write via `Update.write(...)`
   - update `otaProgressPct`, `otaActive`, `otaNeedsRender`
6. Compute SHA-256 while streaming (mbedtls/Hash helper) and compare to `m.sha256`.
7. `Update.end(true)` and reboot if successful.
8. On any failure: `Update.abort()`, set `otaFailed=true`, preserve error string.

Important:

- Reuse existing OTA display/progress state variables so UI and EPD progress remain consistent.
- Do not break manual `/api/update` path.

---

## 8) Scheduler in `loop()`

Add periodic check near your existing timed tasks:

```cpp
void maybeRunRemoteOtaCheck() {
  if (!cfgRemoteOtaEnabled) return;
  if (otaActive) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (remoteOtaBackoffUntilMs && (long)(now - remoteOtaBackoffUntilMs) < 0) return;
  if (now - lastRemoteOtaCheckMs < (unsigned long)cfgRemoteOtaCheckMin * 60000UL) return;
  lastRemoteOtaCheckMs = now;

  RemoteManifest m = {};
  if (!fetchRemoteManifest(m) || !m.valid) { /* set fail/backoff */ return; }
  remoteOtaLastSeenVersion = m.version;

  if (!isManifestForThisDevice(m)) return;
  if (strcmp(m.channel, cfgRemoteOtaChannel) != 0) return;

  int cmp = compareSemver(FW_VERSION, m.version);
  if (cmp > 0 && !cfgRemoteOtaAllowDowngrade) return; // device newer
  if (cmp == 0) return; // equal

  if (cfgRemoteOtaAutoApply) {
    performRemoteOta(m);
  } else {
    // optional: set a "remote update available" status field
  }
}
```

Call `maybeRunRemoteOtaCheck();` from `loop()`.

Backoff suggestion:

- On fail: exponential up to 6h (`5m, 15m, 30m, 60m, ...`) via `remoteOtaBackoffUntilMs`.

---

## 9) Security recommendations (minimum + better)

Minimum:

- Require SHA-256 in manifest and verify it.
- Restrict by `board` and channel.
- Disable downgrade by default.

Better:

- HTTPS only + CA validation.
- Signed manifest (Ed25519 signature field) and embedded public key in firmware.

---

## 10) TrueNAS SCALE serving layout (simple)

Suggested static files in one dataset:

- `/firmware/manifest.json`
- `/firmware/armoured-candles-<version>.bin`

Serve via NGINX app/container. Device polls manifest URL only.

---

## 11) GitHub auto-build-number bump on merged PRs

Create workflow `.github/workflows/bump-version.yml`:

1. Trigger on push to `main`.
2. Skip bot commits to avoid loops.
3. Parse `FW_VERSION` in `armoured-candles.ino` (or use `version.txt`).
4. Increment patch/build component.
5. Commit + push (`github-actions[bot]`).

Then build pipeline should inject the bumped version into firmware builds.

---

## 12) Suggested implementation order (low risk)

1. Add config fields + NVS persistence.
2. Add `/api/status` + `/api/config` support.
3. Add GUI controls and wire save/load.
4. Implement manifest fetch + semver compare (no update yet).
5. Add remote OTA apply path + hash verification.
6. Enable scheduler + backoff.
7. Add CI version bump workflow.

---

## 13) Quick acceptance test matrix

- Remote OTA disabled: no polling activity.
- Bad manifest URL: no crash, visible error, backoff active.
- Equal version: no update.
- Newer version + auto-apply on: update, reboot, new `fwVersion` shown.
- Wrong `board`: ignored.
- Bad hash: update aborted, no reboot.
- Manual browser OTA still works.

