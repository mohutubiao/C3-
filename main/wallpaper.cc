#include "wallpaper.h"
#include "display.h"
#include "lvgl_theme.h"
#include "lvgl_image.h"
#include "board.h"

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <cstring>
#include <memory>

#define TAG "Wallpaper"
#define WALLPAPER_PARTITION_LABEL "wallpaper"
#define WALLPAPER_MAGIC 0x31505057  // "WPP1"

// 文件头：magic + width + height（小端，共 8 字节）
struct WallpaperHeader {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
};

// 映射到 flash 的壁纸图（数据不释放）
class WallpaperImage : public LvglImage {
public:
    WallpaperImage(const uint8_t* data, int width, int height) {
        memset(&image_dsc_, 0, sizeof(image_dsc_));
        image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        image_dsc_.header.w = width;
        image_dsc_.header.h = height;
        image_dsc_.header.stride = width * 2;
        image_dsc_.data_size = (uint32_t)width * height * 2;
        image_dsc_.data = data;
    }
    const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }

private:
    lv_img_dsc_t image_dsc_;
};

Wallpaper& Wallpaper::GetInstance() {
    static Wallpaper instance;
    return instance;
}
size_t Wallpaper::expected_data_size() const {
    // 240x240 RGB565
    return 240 * 240 * 2;
}

bool Wallpaper::MapPartition() {
    if (mapped_data_ != nullptr) {
        return true;
    }
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WALLPAPER_PARTITION_LABEL);
    if (part == nullptr) {
        ESP_LOGE(TAG, "wallpaper partition not found");
        return false;
    }

    // 读取文件头
    WallpaperHeader header;
    if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK) {
        return false;
    }
    if (header.magic != WALLPAPER_MAGIC || header.width != 240 || header.height != 240) {
        return false;  // 没有有效壁纸
    }

    const void* map_ptr;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                           &map_ptr, &mmap_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "mmap wallpaper partition failed");
        return false;
    }
    mapped_data_ = static_cast<const uint8_t*>(map_ptr) + sizeof(WallpaperHeader);
    ESP_LOGI(TAG, "wallpaper loaded (%ux%u RGB565)", header.width, header.height);
    return true;
}

void Wallpaper::Apply(Display* display) {
    if (display == nullptr) {
        return;
    }
    auto theme = display->GetTheme();
    if (theme == nullptr) {
        return;
    }
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    if (mapped_data_ == nullptr) {
        lvgl_theme->set_background_image(nullptr);
    } else {
        // 每次重新包装（数据在 flash，不释放；仅释放包装对象）
        lvgl_theme->set_background_image(
            std::make_shared<WallpaperImage>(mapped_data_, 240, 240));
    }
    display->SetTheme(theme);  // 重新应用主题（内部会刷新背景图）
}

void Wallpaper::Remove(Display* display) {
    if (display != nullptr && display->GetTheme() != nullptr) {
        static_cast<LvglTheme*>(display->GetTheme())->set_background_image(nullptr);
        display->SetTheme(display->GetTheme());
    }
    mapped_data_ = nullptr;
}

void Wallpaper::LoadAndApply(Display* display) {
    if (MapPartition()) {
        Apply(display);
    }
}

bool Wallpaper::BeginWrite() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WALLPAPER_PARTITION_LABEL);
    if (part == nullptr) {
        ESP_LOGE(TAG, "wallpaper partition not found");
        return false;
    }

    size_t total = sizeof(WallpaperHeader) + expected_data_size();
    size_t sector = esp_partition_get_main_flash_sector_size();
    size_t erase_size = (total + sector - 1) / sector * sector;
    if (erase_size > part->size) {
        ESP_LOGE(TAG, "wallpaper too large for partition");
        return false;
    }

    // 写壁纸前先摘掉当前背景，避免绘制时访问正在擦写的 flash
    // （映射地址不变，写完后重新应用即可）
    auto display = Board::GetInstance().GetDisplay();
    if (mapped_data_ != nullptr && display != nullptr) {
        Remove(display);
    }

    ESP_LOGI(TAG, "erasing %u bytes for new wallpaper", (unsigned)erase_size);
    if (esp_partition_erase_range(part, 0, erase_size) != ESP_OK) {
        return false;
    }

    WallpaperHeader header = {
        .magic = WALLPAPER_MAGIC,
        .width = 240,
        .height = 240,
    };
    if (esp_partition_write(part, 0, &header, sizeof(header)) != ESP_OK) {
        return false;
    }

    write_offset_ = sizeof(WallpaperHeader);
    writing_ = true;
    return true;
}

bool Wallpaper::WriteChunk(const uint8_t* data, size_t len) {
    if (!writing_) {
        return false;
    }
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WALLPAPER_PARTITION_LABEL);
    if (part == nullptr) {
        return false;
    }
    if (write_offset_ + len > sizeof(WallpaperHeader) + expected_data_size()) {
        ESP_LOGE(TAG, "wallpaper data overflow");
        return false;
    }
    if (esp_partition_write(part, write_offset_, data, len) != ESP_OK) {
        return false;
    }
    write_offset_ += len;
    return true;
}

bool Wallpaper::EndWrite(Display* display) {
    if (!writing_) {
        return false;
    }
    writing_ = false;
    if (write_offset_ != sizeof(WallpaperHeader) + expected_data_size()) {
        ESP_LOGE(TAG, "wallpaper size mismatch: %u != %u",
                 (unsigned)write_offset_, (unsigned)(sizeof(WallpaperHeader) + expected_data_size()));
        return false;
    }
    if (!MapPartition()) {
        return false;
    }
    Apply(display);
    ESP_LOGI(TAG, "wallpaper saved and applied");
    return true;
}

void Wallpaper::AbortWrite() {
    writing_ = false;
    write_offset_ = 0;
}

bool Wallpaper::Clear(Display* display) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WALLPAPER_PARTITION_LABEL);
    if (part == nullptr) {
        return false;
    }
    if (mapped_data_ != nullptr && display != nullptr) {
        Remove(display);
    }
    size_t sector = esp_partition_get_main_flash_sector_size();
    esp_partition_erase_range(part, 0, sector);  // 只擦头部即可让校验失效
    ESP_LOGI(TAG, "wallpaper cleared");
    return true;
}
