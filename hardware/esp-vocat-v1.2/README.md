# ESP-VoCat v1.2 Official Baseline

This directory is the first public hardware baseline for `P-Ray Voice Stack`.

## Scope

- official board: `ESP-VoCat v1.2`
- toolchain target: `ESP-IDF 5.5.4`
- default transport for this snapshot: `v2 async`
- public release policy: one board baseline only

## Extracted Firmware Contents

The firmware snapshot under `firmware/` keeps only the minimum public-safe baseline taken from the internal reference project:

- `CMakeLists.txt`
- `dependencies.lock`
- `partitions.csv`
- `sdkconfig.defaults`
- `main/` source files for trigger, capture, upload, polling, and playback
- `main/idf_component.yml` for ESP-IDF managed dependencies

## Intentionally Omitted

- build output directories
- local `sdkconfig` state files
- bundled prompt audio assets from `spiffs/`
- committed Wi-Fi credentials, device IDs, or server URLs
- additional board profiles or public multi-board support

## Required Config Before Flash

Set these values via build flags or local configuration before you try a real board:

- `DEMO_WIFI_SSID`
- `DEMO_WIFI_PASSWORD`
- `DEMO_SERVER_BASE_URL`
- `DEMO_DEVICE_ID`

All four remain empty in source control on purpose.

For the current public snapshot, the simplest first bring-up is to edit your local working copy of `firmware/main/config.h` and leave those edits uncommitted:

```c
// local-only example; do not commit real values
#define DEMO_WIFI_SSID "your-ssid"
#define DEMO_WIFI_PASSWORD "your-password"
#define DEMO_SERVER_BASE_URL "http://your-server:8000"
#define DEMO_DEVICE_ID "esp-vocat-v1p2-001"
```

## Current Public Snapshot Notes

- The firmware source is extracted for a first public baseline, not a broad hardware SDK
- The default trigger path is currently GPIO/button based in this public snapshot
- Touch-path tuning and richer hardware polish stay for later tasks
- A full `idf.py build` may still require a prepared `ESP-IDF 5.5.4` environment and component downloads
