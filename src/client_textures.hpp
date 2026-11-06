#pragma once
#include "redstone/engine.hpp"
// Windows' OpenGL 1.1 header requires the Win32 calling-convention macros.
// clang-format off
#include <windows.h>
#include <gl/GL.h>
#include <wincodec.h>
// clang-format on
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace redstone::client {
template <class T> struct ReleaseCom {
  void operator()(T *p) const {
    if (p)
      p->Release();
  }
};
class Textures {
  std::map<std::string, GLuint> loaded_;
  std::filesystem::path root_;

public:
  Textures() {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    root_ = std::filesystem::path(exe).parent_path() / L"assets/textures/block";
    if (!std::filesystem::exists(root_))
      root_ = std::filesystem::path(REDSTONE_ASSET_DIR) / "textures/block";
  }
  void clear() {
    for (auto &[name, id] : loaded_)
      glDeleteTextures(1, &id);
    loaded_.clear();
  }
  GLuint get(const std::string &name) {
    if (auto it = loaded_.find(name); it != loaded_.end())
      return it->second;
    UINT width = 16, height = 16;
    std::vector<BYTE> pixels(16 * 16 * 4);
    bool ok = false;
    IWICImagingFactory *rawFactory{};
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&rawFactory)))) {
      std::unique_ptr<IWICImagingFactory, ReleaseCom<IWICImagingFactory>>
          factory(rawFactory);
      IWICBitmapDecoder *rawDecoder{};
      if (SUCCEEDED(factory->CreateDecoderFromFilename(
              (root_ / (name + ".png")).c_str(), nullptr, GENERIC_READ,
              WICDecodeMetadataCacheOnLoad, &rawDecoder))) {
        std::unique_ptr<IWICBitmapDecoder, ReleaseCom<IWICBitmapDecoder>>
            decoder(rawDecoder);
        IWICBitmapFrameDecode *rawFrame{};
        if (SUCCEEDED(decoder->GetFrame(0, &rawFrame))) {
          std::unique_ptr<IWICBitmapFrameDecode,
                          ReleaseCom<IWICBitmapFrameDecode>>
              frame(rawFrame);
          IWICFormatConverter *rawConverter{};
          if (SUCCEEDED(factory->CreateFormatConverter(&rawConverter))) {
            std::unique_ptr<IWICFormatConverter,
                            ReleaseCom<IWICFormatConverter>>
                converter(rawConverter);
            UINT w{}, h{};
            if (SUCCEEDED(frame->GetSize(&w, &h)) && w && h && w <= 4096 &&
                h <= 4096 &&
                SUCCEEDED(converter->Initialize(
                    frame.get(), GUID_WICPixelFormat32bppRGBA,
                    WICBitmapDitherTypeNone, nullptr, 0,
                    WICBitmapPaletteTypeCustom))) {
              // Animated PNG strips use the first square frame in this client.
              width = w;
              height = std::min(w, h);
              pixels.resize(size_t(width) * height * 4);
              WICRect rect{0, 0, static_cast<INT>(width),
                           static_cast<INT>(height)};
              ok = SUCCEEDED(converter->CopyPixels(
                  &rect, width * 4, static_cast<UINT>(pixels.size()),
                  pixels.data()));
            }
          }
        }
      }
    }
    if (!ok) {
      width = height = 16;
      pixels.resize(16 * 16 * 4);
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          int i = (y * 16 + x) * 4;
          bool check = ((x / 4) ^ (y / 4)) & 1;
          pixels[i] = check ? 255 : 35;
          pixels[i + 1] = 0;
          pixels[i + 2] = check ? 255 : 35;
          pixels[i + 3] = 255;
        }
    }
    GLuint id{};
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    loaded_[name] = id;
    return id;
  }
};
inline std::string faceTexture(const redstone::BlockState &s,
                               redstone::Direction d) {
  using namespace redstone;
  std::string n = s.name().starts_with("minecraft:") ? s.name().substr(10) : s.name();
  if (isWoolCarpet(s))
    return n.substr(0, n.size() - 7) + "_wool";
  if (isDeadCoralWallFan(s)) {
    auto pos = n.rfind("_wall_fan");
    n.replace(pos, 9, "_fan");
    return n;
  }
  if (n == "grass_block")
    return d == Direction::Up         ? "grass_block_top"
           : d == Direction::Down     ? "dirt"
           : s.get("snowy") == "true" ? "grass_block_snow"
                                      : "grass_block_side";
  if (n == "cobblestone_wall")
    return "cobblestone";
  if (n == "stone_brick_wall")
    return "stone_bricks";
  if (n == "redstone_lamp")
    return s.get("lit") == "true" ? "redstone_lamp_on" : "redstone_lamp";
  if (n == "honey_block")
    return d == Direction::Up     ? "honey_block_top"
           : d == Direction::Down ? "honey_block_bottom"
                                  : "honey_block_side";
  if (n == "tnt")
    return d == Direction::Up     ? "tnt_top"
           : d == Direction::Down ? "tnt_bottom"
                                  : "tnt_side";
  if (n == "rail" || n == "powered_rail" || n == "detector_rail" ||
      n == "activator_rail") {
    auto shape = s.get("shape", "north_south");
    bool curved = shape == "south_east" || shape == "south_west" ||
                  shape == "north_west" || shape == "north_east";
    if (n == "rail")
      return curved ? "rail_corner" : "rail";
    bool powered = s.get("powered") == "true";
    return n + (powered ? "_on" : "");
  }
  if (n == "observer") {
    auto facing = directionFromString(s.get("facing", "south"));
    if (d == facing)
      return "observer_front";
    if (d == opposite(facing))
      return s.get("powered") == "true" ? "observer_back_on" : "observer_back";
    bool vertical = facing == Direction::Up || facing == Direction::Down;
    return ((vertical && (d == Direction::North || d == Direction::South)) ||
            (!vertical && (d == Direction::Up || d == Direction::Down)))
               ? "observer_top"
               : "observer_side";
  }
  if (n == "piston" || n == "sticky_piston" || n == "piston_head") {
    auto facing = directionFromString(s.get("facing", "north"));
    if (d == facing) {
      if (s.get("extended") == "true")
        return "piston_inner";
      return n == "sticky_piston" || s.get("type") == "sticky"
                 ? "piston_top_sticky"
                 : "piston_top";
    }
    if (d == opposite(facing))
      return n == "piston_head" ? "piston_top" : "piston_bottom";
    return "piston_side";
  }
  return n;
}
} // namespace redstone::client
