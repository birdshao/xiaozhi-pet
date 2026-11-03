# OTA1 Pet v2 upgrade and attended first boot — 2026-11-02 UTC

**Result:** `PASS (boot identity)` — the Pet application booted from `ota_1`, emitted passive boot markers and entered Pet runtime after a physical BOOT long press. **Official OTA0 selector exactly restored** afterwards.

Performed locally on BIRDSHAO (Windows, board on COM10, native USB `303A:1001`) with esptool v4.8.1 (official Espressif win64 release), each Flash-writing step separately approved by the user in chat. Not run through the Runner workflows.

## Pre-write read-only audit

| Check | Result |
| --- | --- |
| Chip / Flash | ESP32-S3 (QFN56) rev v0.2, 8 MB PSRAM, 16 MB Flash |
| Partition table 0x8000 | byte-equal to original 16 MiB backup (`c3fe03b7…`, re-hashed) |
| OTADATA 0xD000–0xEFFF | byte-equal to original selector `e0920877…`; seq0=0x23 VALID, seq1=0x22 VALID, OTA0 selected |
| OTA0 full region | `verify_flash` digest matched the region inside the original backup |
| OTA1 full region | `verify_flash` digest matched private v1 snapshot `187e1061…` |
| NVS | differs from the 10-26 backup (normal runtime writes); current NVS saved privately before trial |

Full-region `read_flash` over USB-Serial-JTAG failed with `Corrupt data` after a few hundred KiB; use `verify_flash` (on-chip MD5) or chunked reads instead.

## Upgrade write (OTA1 only)

| Item | Value |
| --- | --- |
| Pet commit | 4b186632508000270e1f215036e8aab2fa622866 (firmware identical to main fa93e60) |
| Build | [app-only build #41076469802](https://github.com/birdshao/xiaozhi-pet/actions/runs/41076469802) |
| App size / SHA256 | 2,927,632 B / b3760e14d2f70794dde6a96f3532747e427846d1c92040a669b0c3605722fd9f |
| Image check | `image_info` checksum and validation hash valid; contains `PET_FIRMWARE_BOOT_MARKER` |
| Write | `write_flash --flash_mode keep … 0x410000`, then `verify_flash` OK |
| After write | OTADATA and OTA0 re-verified unchanged |

## Attended boot trial

Only OTADATA sector 0xE000 was written: seq=0x24, state NEW, CRC fixed; SHA256 `532fb829…` equal to the previously reviewed plan. Sector 0xD000 (OTA0 record) untouched.

Sanitized serial summary (60 s; raw lines not stored):

| Signal | Observed |
| --- | --- |
| `PET_FIRMWARE_BOOT_MARKER phase=delay5s` / `delay12s` | 1 / 1 (board-init marker missed: monitor attaches after reset) |
| `Ota: Running partition: ota_1` | 1 |
| `PET_RUNTIME_ON` after BOOT long press | 1 (~40 s) |
| `PET_STORY_SD_FALLBACK` | 1 (no TF story pack present) |
| `PET_EVENT_COMMITTED` | 0 (no click events performed) |
| Panic / watchdog markers | 0 / 0 |

Restore: original 0xE000 sector written back, full 8192-byte OTADATA `verify_flash` OK, next boot logged `Running partition: ota_0` with no Pet markers. NVS changed during the trial (pre `cb1c982e…`, post `d627d3ae…`); both copies kept privately, not restored.

## Current state

- OTA0: official v2.5.2, boot-selected.
- OTA1: Pet v2 `b3760e14…` (replaces v1 `b643437e…`; v1 full-region snapshot kept privately).
- Private local files (not uploaded): `%LOCALAPPDATA%\XiaozhiPet\OTA1Transactions\ota1-4b1866325080-transaction.json` (status `restored`), `otadata-0xE000-original-sector.bin`, `NVSBackups\nvs-{before,after}-4b18663-trial-20261102.bin`.

## Still not verified

ePaper Pet scene rendering quality/ghosting, single/double/triple click events and NVS commit, story audio and SD pages (needs the TF smoke card), voice path under Pet firmware, memory headroom, and whether the installed bootloader enables app rollback.
