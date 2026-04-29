# Hardware V2

This is the single official public hardware baseline for the repository. Follow this path only after the `local-v2` browser demo is already clear.

## Official Baseline

- board: `ESP-VoCat v1.2`
- firmware path: `hardware/esp-vocat-v1.2/firmware/`
- transport scope for this task: `v2 async`
- public hardware policy: one board baseline only

## What This Task Publishes

- a public-safe ESP-IDF firmware snapshot for `ESP-VoCat v1.2`
- the minimal docs needed for a first public hardware entry
- build-time config placeholders instead of committed Wi-Fi or server secrets

## What This Task Does Not Publish

- additional boards
- a `v3` public hardware path
- bundled prompt audio assets or other non-essential launch artifacts

## Before You Build

0. Start the server from the repository root with `python3 -m uvicorn server.app:app --reload`. The board needs a reachable API server.
1. Install `ESP-IDF 5.5.4`
2. Confirm you are using `ESP-VoCat v1.2`
3. Set `DEMO_WIFI_SSID`, `DEMO_WIFI_PASSWORD`, `DEMO_SERVER_BASE_URL`, and `DEMO_DEVICE_ID`. See [`hardware/esp-vocat-v1.2/README.md`](../../hardware/esp-vocat-v1.2/README.md) for a concrete local config example.
4. Review [`hardware/esp-vocat-v1.2/README.md`](../../hardware/esp-vocat-v1.2/README.md) for the extracted baseline contents

## Current First-Release Notes

- The public baseline keeps one official board only: `ESP-VoCat v1.2`
- The extracted firmware defaults to the `v2 async` cloud path
- The public snapshot currently uses the GPIO/button trigger path by default
- Touch tuning, richer prompts, and later transport work stay outside Task 4
