#include "kid_app/waveshare_story_sd.h"

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_story_sd";
constexpr char kMount[] = "/sdcard";
constexpr char kBooks[] = "/sdcard/books";

// Pin definitions are from official Waveshare 3.97 04_SD_Test BSP.
// EPD SPI is on 9/10/11/12, ES8311 I2S is on 13/14/21/47/48.
constexpr int kClk = 16;
constexpr int kCmd = 17;
constexpr int kD0 = 15;
constexpr int kD1 = 7;
constexpr int kD2 = 8;
constexpr int kD3 = 18;

// Read-only card diagnostics: what the board actually sees on the card.
// 2026-11-05: packs were "absent" although the card mounted; this tells a
// wrong name/folder apart from a file-name support problem.
void LogDirectory(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        ESP_LOGW(kTag, "SD_DIR path=%s open_errno=%d", path, errno);
        return;
    }
    int shown = 0;
    while (const dirent* entry = readdir(dir)) {
        if (++shown > 24) {
            ESP_LOGI(kTag, "SD_DIR path=%s more=1", path);
            break;
        }
        char full[32 + sizeof(entry->d_name)];  // "/sdcard/books/" + longest name
        std::snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat info {};
        const bool ok = stat(full, &info) == 0;
        ESP_LOGI(kTag, "SD_ENTRY path=%s name=%s dir=%d size=%ld", path, entry->d_name,
                 ok && S_ISDIR(info.st_mode) ? 1 : 0, ok ? static_cast<long>(info.st_size) : -1L);
    }
    closedir(dir);
}

void LogFileNameSupport() {
#if defined(CONFIG_FATFS_LFN_NONE)
    ESP_LOGW(kTag, "SD_LFN mode=none max=0");
#elif defined(CONFIG_FATFS_LFN_HEAP)
    ESP_LOGI(kTag, "SD_LFN mode=heap max=%d", CONFIG_FATFS_MAX_LFN);
#elif defined(CONFIG_FATFS_LFN_STACK)
    ESP_LOGI(kTag, "SD_LFN mode=stack max=%d", CONFIG_FATFS_MAX_LFN);
#else
    ESP_LOGW(kTag, "SD_LFN mode=unknown");
#endif
}
}  // namespace

bool WaveshareStorySd::Initialize(const char* book_id) {
    if (ready_) return true;
    if (!book_id) return false;
    missing_ = mounted_ ? BookMissing::NoBook : BookMissing::NoCard;
    if (!mounted_) {
        esp_vfs_fat_sdmmc_mount_config_t config{};
        config.format_if_mount_failed = false;
        // Book + narration + sound pack (V2-08) + game pack (V4-06).
        config.max_files = 4;
        config.allocation_unit_size = 16 * 1024;
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_DEFAULT; // conservative 20 MHz
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = static_cast<gpio_num_t>(kClk);
        slot.cmd = static_cast<gpio_num_t>(kCmd);
        slot.d0 = static_cast<gpio_num_t>(kD0);
        slot.d1 = static_cast<gpio_num_t>(kD1);
        slot.d2 = static_cast<gpio_num_t>(kD2);
        slot.d3 = static_cast<gpio_num_t>(kD3);
        sdmmc_card_t* card = nullptr;
        const esp_err_t result =
            esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &config, &card);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Optional SD mount unavailable: %s",
                     esp_err_to_name(result));
            return false;
        }
        mounted_ = true;
        ESP_LOGI(kTag, "SD mounted with official 4-bit GPIO mapping");
        LogFileNameSupport();
        LogDirectory(kMount);
        LogDirectory(kBooks);
    }

    if (!file_.OpenBook(kBooks, book_id)) {
        char path[96];
        std::snprintf(path, sizeof(path), "%s/%s.kpb", kBooks, book_id);
        struct stat info {};
        const int found = stat(path, &info);
        ESP_LOGW(kTag, "SD_BOOK_ABSENT path=%s stat=%d errno=%d size=%ld", path, found,
                 found == 0 ? 0 : errno, found == 0 ? static_cast<long>(info.st_size) : -1L);
        ESP_LOGW(kTag, "Approved story .kpb absent; using reader placeholder");
        missing_ = BookMissing::NoBook;
        return false;
    }
    if (!reader_.Open(file_)) {
        file_.Close();
        ESP_LOGW(kTag, "Story package validation failed; no SD content displayed");
        missing_ = BookMissing::BadBook;
        return false;
    }
    ready_ = true;
    missing_ = BookMissing::None;
    ESP_LOGI(kTag, "Offline story ready; pages=%u",
             static_cast<unsigned>(reader_.PageCount()));
    return true;
}

}  // namespace kid_app
