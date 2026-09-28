/*
 * sdcard.c -- Micro-SD card (SPI3) + FATFS mount for small_tv.
 *
 * Standard ESP-IDF SDSPI + VFS-FAT recipe (see IDF example storage/sd_card/sdspi).
 * Mount point is SD_MOUNT_POINT ("/sdcard"), consumed by LVGL's POSIX fs
 * driver (letter 'S') so lv_image can read "S:/Scenes/Holo3D/frameNNN.bin".
 */
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdcard.h"
#include "app_config.h"

static const char *TAG = "sdcard";
static bool s_mounted = false;

bool sdcard_is_mounted(void) { return s_mounted; }

bool sdcard_init(void)
{
    ESP_LOGI(TAG, "initializing SD card on SPI3 (CLK=%d MOSI=%d MISO=%d CS=%d)",
             PIN_SD_CLK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_HZ / 1000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 16384,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) -- boot animation will be skipped",
                 esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return false;
    }

    sdmmc_card_print_info(stdout, card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    return true;
}
