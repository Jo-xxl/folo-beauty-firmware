#pragma once

#include <cstddef>
#include <cstdint>

enum class PortraitState;  // defined in pixel_portrait.h

struct BeautyImage {
    const uint8_t* data;
    size_t size;
    const char* name;
};

// 设备状态 -> 默认表情图（永不返回 nullptr）。
const BeautyImage* BeautyForState(PortraitState state);

// 情绪名 -> 表情图（未知情绪返回 nullptr）。
const BeautyImage* BeautyByName(const char* name);
