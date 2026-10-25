#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <cstdint>
#include <cstddef>

class Display;

// 壁纸管理：把控制页上传的 RGB565 原图存进专用 flash 分区，
// 开机校验后作为 LVGL 背景图挂到当前主题上。
class Wallpaper {
public:
    static Wallpaper& GetInstance();

    // 开机调用：校验分区内容并应用为背景
    void LoadAndApply(Display* display);

    // 写入流程（HTTP 流式上传用）
    bool BeginWrite();                                   // 擦除分区并写入文件头
    bool WriteChunk(const uint8_t* data, size_t len);    // 顺序写入像素数据
    bool EndWrite(Display* display);                     // 校验总长并应用
    void AbortWrite();

    // 删除壁纸，恢复纯色背景
    bool Clear(Display* display);

    bool has_wallpaper() const { return mapped_data_ != nullptr; }
    size_t expected_data_size() const;

private:
    Wallpaper() = default;

    bool MapPartition();     // mmap 分区并校验头
    void Apply(Display* display);
    void Remove(Display* display);

    const uint8_t* mapped_data_ = nullptr;   // 指向像素数据（跳过文件头）
    uint32_t mmap_handle_ = 0;
    size_t write_offset_ = 0;
    bool writing_ = false;
};

#endif // WALLPAPER_H
