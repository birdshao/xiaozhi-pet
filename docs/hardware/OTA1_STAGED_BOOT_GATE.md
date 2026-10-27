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


## BIRDSHAO boot selector and separate rescue implemented (2026-10-27 UTC)

Runner [PR #32](https://github.com/smoothRobot/orpa-agent-runner/pull/32) is merged; main CI [#40459816348](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40459816348) is successful. The selector plan, independent rescue, read-only audit, and short first-boot controller now exist as **manually gated tools**, not as proof of a boot experiment.

The planned first-trial change is restricted to **the second 4 KiB OTADATA sector (0xE000)**. The original first sector (0xD000), seq=0x23, state VALID, and official OTA0 selection is preserved intact. The original second sector has seq=0x22/state VALID; a planned trial second sector would have seq=0x24/state NEW, and CRC adjusted. The proposed sector SHA256 is 532fb82918593b50c1066f48a2da827232b0f845a68794e5524729a76bf51734.

The exact plan was run **read-only on BIRDSHAO** against the actual full-backup OTADATA, and current live OTADATA was separately read and compared with all original 8192 bytes:
- original seq0=0x23, seq1=0x22;
- original selects official OTA0, planned second sector selects OTA1;
- first OTA0 recovery sector is unchanged;
- live selector remains original official OTA0;
- script exited 0, no Flash write or boot-slot switch.

The merged script [xiaozhi_birdshao_boot_control.py](https://github.com/smoothRobot/orpa-agent-runner/blob/main/scripts/xiaozhi_birdshao_boot_control.py) was also run on the physical board in **audit** mode only, reporting READ_ONLY_SELECTOR_STATE=original_ota0_selected, NO_FLASH_WRITE_OR_BOOT_SWITCH, exit 0.

The future workflow is [BIRDSHAO independent boot selector](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-birdshao-boot-selector.yml) and has three different action-specific confirmation values. Its trial mode plans a 20-second observation before a separately callable restoration of exactly the original second OTADATA sector and checks that the original full 8192-byte selector is back.

**Important constraints:** the installed bootloader's rollback flag remains unverified; a Python finally clause cannot survive a PC/board power loss; serial observations can miss early boot messages; unknown/torn selector bytes trigger fail-closed manual recovery instead of an unchecked overwrite. No physical boot trial has yet been authorized or conducted. The first run must be attended and can still fail or require manual recovery.

For a future **separately approved** 20-second first boot: select main, mode TRIAL_OTA1_20S, and confirm TRIAL_STAGED_OTA1_AND_RESTORE_OTA0. These choices modify only OTADATA sector 0xE000 and do not rewrite OTA0 or OTA1 applications. Choose neither without explicit permission to change the boot selection.

## Upgrade readiness: verified private v1 OTA1 full-partition recovery (2026-10-27 UTC)

The **new passive Pet firmware** on main was built successfully in [app-only build #40467690855](https://github.com/birdshao/xiaozhi-pet/actions/runs/40467690855): Pet commit b2c2106f1ae4b27bd0a6fb5d079cedff3b65f8fc, app size 2,858,144 bytes, SHA256 5820c2a18759411739705929b13ccd6f3c4721254552d1b1e698c1d692cf3d78. Binary strings verify the passive Pet boot diagnostic markers at board init, +5 seconds and +12 seconds. This build has **not** been flashed.

The versioned full OTA1 snapshot tool in [Runner PR #36](https://github.com/smoothRobot/orpa-agent-runner/pull/36) was run read-only on physical BIRDSHAO and exited successfully. It confirmed the original 16MiB private backup, original official OTA0 and untouched OTADATA; verified the actual installed v1 Pet app SHA; and stored the **full 4,128,768-byte currently installed OTA1 region** privately. This is different from the original pre-Pet v2.2.38 OTA1 preserved inside the original 16 MiB backup.

The newly saved private full-region rollback snapshot SHA256 is:

187e1061aea3bdbd4f27ef3aed8c83ad30f47edb57eed5e9b03c4d026159c5eb

Local-only location: %LOCALAPPDATA%\XiaozhiPet\OTA1UpgradeBackups\ota1-before-b2c2106f1ae4-full4128768.bin, with matching local .bin.json metadata; independently rehashed. The private snapshot and the original full 16MiB backup **were not uploaded** to GitHub.

**Current state:** OTA0 official remains selected; OTA1 still contains the **old** pinned Pet app (b643437e...), not the new firmware; the original first-boot trial journal remains restored. No second app write, new slot switch or second boot trial has taken place.

A future upgrade needs a separate version-pinned, independently reviewed and explicitly authorized workflow that verifies the just-recorded old full OTA1 digest, only writes the new app into inactive OTA1, readback-verifies the new SHA, and preserves original OTA0/OTADATA/NVS/assets. An independent restore must use this *current Pet v1 full OTA1 snapshot* rather than the even older pre-Pet OTA1 region. Never delete/reuse old single-use flash/boot journals.
