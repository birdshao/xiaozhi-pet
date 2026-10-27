# OTA1 first boot observation — 2026-10-27 UTC

**Result:** `INCONCLUSIVE` (positive Pet firmware identity not captured), **official OTA0 selector verified restored**.

Hardware trial: [Runner workflow #40465709897](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40465709897), manual input `TRIAL_OTA1_20S`. The 20-second time-boxed trial was dispatched for the already staged Pet OTA1 app. This is **not** an app flashing failure and it is **not proof of Pet app startup**.

## Actual sanitized evidence

| Signal | Observed |
| --- | --- |
| Candidate previously installed at OTA1 0x410000, SHA256 | `b643437eda9ccff1f7d21bd8822a31253906613286b892b1f6b5d8caa4bb1dda` (independently read back in earlier audit) |
| Original OTA0 application live SHA256 before trial | Matched official pinned app |
| Raw serial lines | 119 lines read; **not stored in this report** |
| Panic marker | 0 |
| Watchdog marker | 0 |
| Generic upstream firmware version 2.5.2 | 1 |
| Positive OTA1 bootloader marker | 0 |
| Pet runtime event marker | 0 |
| Serial monitor errors | 0 |
| Trial process | Nonzero exit due to `Boot trial INCONCLUSIVE: no positive OTA1 boot marker` |
| Rescue performed in finally | `ORIGINAL_OTA0_SELECTOR_EXACTLY_RESTORED` |
| Private trial journal | `restored` |
| Subsequent independent on-device selector read | `READ_ONLY_SELECTOR_STATE=original_ota0_selected`, exit 0 |
| Original Pet installation journal | Still `stage_verified` |

The physical audit was run through Desktop Commander after the GitHub trial exited, confirming the original 8192-byte selector and official OTA0 selection. It did not write Flash.

## Why the evidence is insufficient

The original Pet build's Waveshare board adapter emits `PET_RUNTIME_ON` only after a BOOT long press, and `PET_EVENT_COMMITTED` after a Pet event. It does not emit a passive Pet identity marker at boot. The original Runner monitor searched for `pet_preview_on/off`, which the pinned firmware never emits; it also searched for a bootloader line containing OTA1 offset `0x410000`, but that can occur before serial monitoring attaches following the esptool hard reset. A generic `2.5.2` version line cannot distinguish the official OTA0 from the Pet OTA1 build. Consequently, the experiment cannot establish which application executed even though 119 serial lines were captured.

The recovery succeeded, but **EPD Pet rendering, BOOT long/short press, audio interaction and initial Pet runtime startup have not been verified on hardware**.

## Follow-up implementation

- Runner [PR #34](https://github.com/smoothRobot/orpa-agent-runner/pull/34): recognize actual `PET_RUNTIME_ON/OFF`, `PET_EVENT_COMMITTED` and any future passive `PET_FIRMWARE_BOOT_MARKER`, with unit tests and sanitized counters. Generic app version remains insufficient.
- Firmware [PR #25](https://github.com/birdshao/xiaozhi-pet/pull/25): proposed build-only addition of `PET_FIRMWARE_BOOT_MARKER` at board setup and +5/+12 seconds, to improve capture by a monitor that attaches after reset. This is source code for a future firmware candidate, **not** present in the currently flashed OTA1 image.
- **Do not blindly retry:** the existing trial journal is intentionally single-use and marked `restored`. Do not delete or bypass it. Future trial needs a separately versioned, pinned candidate and an independently reviewed recovery and authorization plan.
- **No unauthorized write:** The current verified installed OTA1 image, official OTA0, original OTADATA, NVS and assets must be retained. No additional Flash writes or boot-selector changes are implied by this report.

No raw Flash, NVS, serial console text, Wi-Fi credentials, children's audio or secrets were collected into this document.
