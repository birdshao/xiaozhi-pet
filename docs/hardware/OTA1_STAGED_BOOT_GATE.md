# BIRDSHAO: staged Pet OTA1 and first-boot gate (hardware evidence)

Status as of 2026-10-27 UTC: **Pet firmware installed and independently read back in the inactive OTA1 slot. Official upstream v2.5.2 in OTA0 remains boot-selected. Pet application has NOT been booted yet.**

## Immutable tested installation

| Item | Actual verification |
| --- | --- |
| Hardware | BIRDSHAO Windows host, Waveshare ESP32-S3-ePaper-3.97, 16 MiB Flash |
| Upstream | 78/xiaozhi-esp32 v2.5.2 commit 49b4380b3faa0ceadd28a3e78aaf0f311c46c57e |
| Pet revision | 6bf8d0106a11f502466d7abe80adb0a0eb0181dc |
| App-only build | GitHub Actions [#40265257677](https://github.com/birdshao/xiaozhi-pet/actions/runs/40265257677), 2,857,408 bytes |
| App SHA256 | b643437eda9ccff1f7d21bd8822a31253906613286b892b1f6b5d8caa4bb1dda |
| Target Flash | ota_1 at 0x410000, 0x3F0000-byte region |
| Successful physical stage | [smoothRobot/orpa-agent-runner #40450062505](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40450062505) |
| Transaction result | BIRDSHAO-only private journal status stage_verified; write and image SHA readback succeeded |
| Unchanged boot selection | original 8192-byte otadata was byte-for-byte equal before and after staging; official OTA0 selected |
| Preserved recovery | 16 MiB flash private backup on BIRDSHAO; original OTA1 region fully preserved with SHA256 ba7a2daec93f3b038bcff304d15466c4d4335d6143197eab09ae1bfde8f864b8 |
| Independent audit | Read-only live official OTA0 complete app SHA and staged OTA1 complete 2,857,408-byte app SHA both matched; original otadata retained, status STAGED_OTA1_READINESS_READ_ONLY_PASS |

The backup remains only on BIRDSHAO. Neither raw Flash nor NVS is uploaded to GitHub. The staged app uses **the original bootloader/partition table/NVS/shared assets**, which have not been replaced with a merged image.

## What is NOT verified by a successful Flash write

A passed build, checksum and two-slot installation **do not** prove the Pet firmware actually boots; the ePaper refresh, BOOT input, original UI restoration, audio concurrency or memory use must still be observed on the physical board. They also do not prove OTA automatic rollback is enabled.

Before selecting the new app, check whether the *installed* bootloader includes CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE; the existence of OTA slots alone does not imply the bootloader can automatically recover from a failed first boot. Follow [official ESP-IDF OTA/rollback documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/ota.html).

The existing generic Runner script xiaozhi_ota_trial.py was written for a different board baseline with an opposite OTA0/OTA1 selection and **must not be used** on BIRDSHAO's present slot layout.

## Remaining gate before physical first boot

1. Review, mock-test and provision an **independent exact-OTADATA restoration utility** that still works if the trial process crashes, USB disconnects or the app fails to start. This requires a separately validated recovery command. The old OTA1 restore workflow alone is not sufficient because it assumes unchanged OTADATA before doing anything.
2. Obtain separate physical permission for a short OTA1 boot-selection trial. Current authorization was only to stage bytes in inactive OTA1, without switching boot selection.
3. Reconfirm actual live OTA0/OTA1 app checksums, unique USB device, private backup and initial 8192-byte OTADATA immediately before any selector modification.
4. With supervision, observe serial *sanitized event summaries only*: boot selection, panic/reset/watchdog, startup duration and candidate version; never log Wi-Fi credentials or personal conversation audio.
5. Validate P1 Pet Demo BOOT button: long press idle enters pet preview, short press cycles emotions, long press exits, and original official UI/voice path can still be used. Test EPD BUSY timeout, ghosting, time to paint, memory headroom and audio compatibility; do not claim success without evidence.
6. Restore the exact original OTA0 selector and verify actual boot selection, ideally independently read back. If no reliable recovery path exists, do not initiate the boot experiment.

A first boot trial, Pet mode acceptance and any further NVS/user-data changes remain **unperformed**.

See also [P1 firmware/EPD integration](../P1_EPD_INTEGRATION.md) and the private hardware workflow document in smoothRobot/orpa-agent-runner: docs/xiaozhi-birdshao-ota1-stage.md.
