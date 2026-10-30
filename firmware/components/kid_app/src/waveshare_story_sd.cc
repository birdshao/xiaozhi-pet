#include "kid_app/waveshare_story_sd.h"

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

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
}  // namespace

bool WaveshareStorySd::Initialize(const char* book_id) {
    if (ready_) return true;
    if (!book_id) return false;
    if (!mounted_) {
        esp_vfs_fat_sdmmc_mount_config_t config{};
        config.format_if_mount_failed = false;
        config.max_files = 3;
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
        const esp_err_t result =
            esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &config, &card_);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Optional SD mount unavailable: %s",
                     esp_err_to_name(result));
            return false;
        }
        mounted_ = true;
        ESP_LOGI(kTag, "SD mounted with official 4-bit GPIO mapping");
    }

    if (!file_.OpenBook(kBooks, book_id)) {
        ESP_LOGW(kTag, "Approved story .kpb absent; using reader placeholder");
        return false;
    }
    if (!reader_.Open(file_)) {
        file_.Close();
        ESP_LOGW(kTag, "Story package validation failed; no SD content displayed");
        return false;
    }
    ready_ = true;
    ESP_LOGI(kTag, "Offline story ready; pages=%u",
             static_cast<unsigned>(reader_.PageCount()));
    return true;
}

}  // namespace kid_app
