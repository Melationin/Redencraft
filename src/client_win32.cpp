// Lightweight local debug client. Rendering and editor controls do not run any
// Minecraft simulation logic: edits and ticks go through redstone::World.
#include "client_inventory.hpp"
#include "client_textures.hpp"
#include "redstone/entity.hpp"
#include "redstone/allocator.hpp"
#include "redstone/engine.hpp"
#include "redstone/piston.hpp"
#include "redstone/shapes.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gl/GL.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <windowsx.h>

namespace {
using namespace redstone;
constexpr double pi = 3.14159265358979323846;
struct V3 {
  double x{}, y{}, z{};
  V3 operator+(V3 b) const { return {x + b.x, y + b.y, z + b.z}; }
  V3 operator-(V3 b) const { return {x - b.x, y - b.y, z - b.z}; }
  V3 operator*(double s) const { return {x * s, y * s, z * s}; }
};
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 normalized(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }
std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              nullptr, 0);
  std::wstring out(n, L' ');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}
std::wstring shortName(const std::string &s) {
  return wide(s.starts_with("minecraft:") ? s.substr(10) : s);
}
struct Hit {
  Pos pos;
  Direction face;
  double distance;
};
struct DrawBlock {
  Pos pos;
  BlockState state;
};
enum Id {
  Run = 100,
  Step,
  Reset,
  Trigger,
  Palette,
  Facing,
  Place,
  Remove,
  Open,
  Speed,
  Plane,
  Trace,
  Status,
  Inspect,
  Logs
};
struct App {
  HWND window{}, view{}, palette{}, facing{}, status{}, inspect{}, logs{},
      runButton{}, speed{}, plane{}, trace{};
  HDC dc{};
  HGLRC gl{};
  HFONT font{};
  std::unique_ptr<World> world;
  client::Textures textures;
  redstone::client::Inventory inventory;
  std::vector<std::string> catalog = redstone::client::creativeCatalog();
  GLuint fontBase{};
  std::filesystem::path previewPath;
  std::vector<DrawBlock> blocks;
  std::vector<std::string> choices;
  std::vector<std::wstring> history;
  V3 focus{3, 0, 3}, camera{6.5, 2.8, 8.5};
  double yaw = 0.8, pitch = 0.65, distance = 19;
  int width = 800, height = 700, lastX{}, lastY{};
  bool dragging = false, running = false;
  bool mouseLocked = false, breakHeld = false, placeHeld = false;
  int wheelRemainder = 0;
  ULONGLONG lastFrame{}, lastBreak{}, lastPlace{};
  ULONGLONG lastTick{};
  std::optional<Hit> hover, selected;
  std::optional<Pos> planeCell;
  int layer = 0;
  void refresh();
  void tick();
  void demo();
  void render();
  void pick(int, int);
  void edit(bool);
  void load();
  void controls();
  void layout(int, int);
  V3 eye() const { return camera; }
  V3 forward() const {
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch)};
  }
  void lockMouse(bool);
  void toggleInventory();
  void syncItem();
  void frame();
  void hud();
  void inventoryClick(int, int);
};
App *app{};
void label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
  auto c = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h,
                         parent, nullptr, nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
}
HWND control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style,
             int id, int x, int y, int w, int h) {
  auto c = CreateWindowW(
      cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
  return c;
}
void App::controls() {
  label(window, L"REDSTONE / 26.1", 16, 16, 270, 26);
  runButton = control(window, L"BUTTON", L"运行 / Run", BS_PUSHBUTTON, Run, 16,
                      52, 128, 30);
  control(window, L"BUTTON", L"单步 / Step", BS_PUSHBUTTON, Step, 152, 52, 128,
          30);
  label(window, L"速度 (ticks/s)", 16, 96, 120, 22);
  speed = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST, Speed, 152, 91,
                  128, 180);
  for (auto s : {L"1", L"5", L"10", L"20"})
    SendMessageW(speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
  SendMessageW(speed, CB_SETCURSEL, 3, 0);
  control(window, L"BUTTON", L"新建演示场景", BS_PUSHBUTTON, Reset, 16, 130,
          128, 30);
  control(window, L"BUTTON", L"触发侦测器", BS_PUSHBUTTON, Trigger, 152, 130,
          128, 30);
  control(window, L"BUTTON", L"读取存档文件夹…", BS_PUSHBUTTON, Open, 16, 171,
          264, 30);
  label(window, L"方块", 16, 219, 264, 22);
  palette = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                    Palette, 16, 246, 264, 400);
  for (auto &item : catalog) {
    choices.push_back(item);
    auto s = shortName(item);
    SendMessageW(palette, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
  }
  SendMessageW(palette, CB_SETCURSEL, 0, 0);
  label(window, L"朝向由视线决定", 16, 287, 132, 22);
  facing = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST, Facing, 152, 282,
                   128, 200);
  for (auto s : {L"down", L"up", L"north", L"south", L"west", L"east"})
    SendMessageW(facing, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
  SendMessageW(facing, CB_SETCURSEL, 5, 0);
  EnableWindow(facing, FALSE);
  label(window, L"创造飞行：Space ↑ / Shift ↓", 16, 328, 264, 22);
  plane = control(window, L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                  Plane, 152, 323, 128, 27);
  // ES_NUMBER permits negative numbers entered programmatically; signed
  // keyboard entry is needed for Minecraft heights, so remove this filter.
  SetWindowLongPtrW(plane, GWL_STYLE,
                    GetWindowLongPtrW(plane, GWL_STYLE) & ~ES_NUMBER);
  ShowWindow(plane, SW_HIDE);
  control(window, L"BUTTON", L"进入 / 锁定鼠标", BS_PUSHBUTTON, Place, 16, 366,
          128, 32);
  control(window, L"BUTTON", L"背包 (E)", BS_PUSHBUTTON, Remove, 152, 366, 128,
          32);
  trace = control(window, L"BUTTON", L"记录更新轨迹", BS_AUTOCHECKBOX, Trace,
                  16, 412, 264, 24);
  inspect = control(window, L"EDIT",
                    L"左键选择方块\r\n右键拖动旋转，滚轮缩放\r\nWASD 平移视点",
                    ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER,
                    Inspect, 16, 450, 264, 130);
  status = control(window, L"STATIC", L"", 0, Status, 16, 592, 264, 50);
  logs = control(window, L"EDIT", L"",
                 ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_BORDER, Logs, 16,
                 650, 264, 120);
}
void App::layout(int w, int h) {
  width = std::max(1, w - 300);
  height = std::max(1, h);
  MoveWindow(view, 300, 0, width, height, TRUE);
  MoveWindow(logs, 16, 650, 264, std::max(40, h - 666), TRUE);
  if (mouseLocked)
    lockMouse(true);
}
void App::lockMouse(bool enabled) {
  if (enabled && inventory.inventoryOpen)
    return;
  if (enabled) {
    RECT bounds{};
    GetClientRect(view, &bounds);
    MapWindowPoints(view, nullptr, reinterpret_cast<POINT *>(&bounds), 2);
    ClipCursor(&bounds);
    if (!mouseLocked) {
      SetFocus(view);
      SetCapture(view);
      SetCursorPos((bounds.left + bounds.right) / 2,
                   (bounds.top + bounds.bottom) / 2);
      while (ShowCursor(FALSE) >= 0) {
      }
    }
  } else if (mouseLocked) {
    mouseLocked = false;
    ClipCursor(nullptr);
    ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {
    };
    breakHeld = placeHeld = false;
  }
  mouseLocked = enabled;
  InvalidateRect(view, nullptr, FALSE);
}
void App::syncItem() {
  auto it = std::find(choices.begin(), choices.end(), inventory.selectedItem());
  if (it != choices.end())
    SendMessageW(palette, CB_SETCURSEL, it - choices.begin(), 0);
  InvalidateRect(view, nullptr, FALSE);
}
void App::toggleInventory() {
  if (!inventory.inventoryOpen) {
    lockMouse(false);
    inventory.inventoryOpen = true;
    SetFocus(view);
  } else {
    inventory.inventoryOpen = false;
    lockMouse(true);
  }
  InvalidateRect(view, nullptr, FALSE);
}
void App::frame() {
  auto now = GetTickCount64();
  double elapsed = lastFrame ? std::min(.1, double(now - lastFrame) / 1000) : 0;
  lastFrame = now;
  if (!mouseLocked || GetForegroundWindow() != window)
    return;
  V3 forwardFlat{-std::sin(yaw), 0, -std::cos(yaw)},
      right{std::cos(yaw), 0, -std::sin(yaw)}, movement{};
  if (GetAsyncKeyState('W') & 0x8000)
    movement = movement + forwardFlat;
  if (GetAsyncKeyState('S') & 0x8000)
    movement = movement - forwardFlat;
  if (GetAsyncKeyState('D') & 0x8000)
    movement = movement + right;
  if (GetAsyncKeyState('A') & 0x8000)
    movement = movement - right;
  if (GetAsyncKeyState(VK_SPACE) & 0x8000)
    movement.y += 1;
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
    movement.y -= 1;
  if (dot(movement, movement) > 0)
    camera = camera +
             normalized(movement) *
                 (elapsed * ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 12 : 5));
  focus = camera + forward() * 5;
  pick(width / 2, height / 2);
  if (breakHeld && now - lastBreak >= 250) {
    lastBreak = now;
    edit(true);
  }
  if (placeHeld && now - lastPlace >= 200) {
    lastPlace = now;
    edit(false);
  }
}
void App::refresh() {
  blocks.clear();
  for (auto &[p, s] : world->allBlocks())
    if (world->isLoaded(p))
      blocks.push_back({p, s});
  for (auto &e : world->log) {
    std::wostringstream s;
    s << L"t=" << world->gameTime() << L" " << wide(e.kind) << L" (" << e.pos.x
      << L"," << e.pos.y << L"," << e.pos.z << L") " << wide(e.detail);
    history.push_back(s.str());
  }
  world->log.clear();
  if (history.size() > 120)
    history.erase(history.begin(), history.end() - 120);
  std::wstring text;
  for (auto &s : history)
    text += s + L"\r\n";
  SetWindowTextW(logs, text.c_str());
  SendMessageW(logs, EM_LINESCROLL, 0, 100000);
  std::wostringstream info;
  info << L"Tick " << world->gameTime() << L"  |  blocks " << blocks.size()
       << L"  |  entities " << world->entities.size()
       << L"\r\n计划刻 " << world->scheduledTickCount() << L"  方块事件 "
       << world->blockEventCount();
  SetWindowTextW(status, info.str().c_str());
  if (selected) {
    auto p = selected->pos;
    auto state = world->getBlock(p);
    std::wostringstream s;
    s << L"(" << p.x << L", " << p.y << L", " << p.z << L") "
      << shortName(state.name()) << L"\r\n";
    for (auto &[k, v] : state.properties())
      s << wide(k) << L" = " << wide(v) << L"\r\n";
    if (auto it = world->pistonEntities.find(p);
        it != world->pistonEntities.end())
      s << L"progress = " << it->second->progress << L"\r\n";
    SetWindowTextW(inspect, s.str().c_str());
  }
  InvalidateRect(view, nullptr, FALSE);
}
void App::tick() {
  try {
    world->tick();
    refresh();
  } catch (const std::exception &e) {
    running = false;
    SetWindowTextW(runButton, L"运行 / Run");
    MessageBoxW(window, wide(e.what()).c_str(), L"Tick error", MB_ICONERROR);
  }
}
void App::demo() {
  lockMouse(false);
  inventory.inventoryOpen = false;
  running = false;
  SetWindowTextW(runButton, L"运行 / Run");
  history.clear();
  EnableWindow(GetDlgItem(window, Trigger), TRUE);
  selected.reset();
  world = std::make_unique<World>();
  for (int x = -1; x <= 1; x++)
    for (int z = -1; z <= 1; z++)
      world->loadChunk(x, z);
  for (int x = -3; x <= 9; x++)
    for (int z = -2; z <= 8; z++)
      world->rawSetBlock({x, -1, z}, defaultState("smooth_stone"));
  world->rawSetBlock({4, 0, 4},
                     defaultState("observer").with("facing", "east"));
  world->rawSetBlock({2, 0, 4},
                     defaultState("sticky_piston").with("facing", "up"));
  world->rawSetBlock({2, 1, 4}, defaultState("slime_block"));
  world->rawSetBlock({3, 0, 4}, defaultState("smooth_stone"));
  world->rawSetBlock({5, 0, 4}, defaultState("glass"));
  world->rawSetBlock({-2, 0, 0}, defaultState("note_block"));
  int carpetIndex = 0;
  for (const auto &entry : blockRegistry())
    if (isWoolCarpet(defaultState(entry.name))) {
      world->rawSetBlock({carpetIndex % 8, 0, carpetIndex / 8},
                         defaultState(entry.name));
      ++carpetIndex;
    }
  world->rawSetBlock({-1, 0, 0}, defaultState("tnt"));
  world->rawSetBlock({7, 0, 6}, defaultState("honey_block"));
  focus = {3, 0, 3};
  yaw = 0.8;
  pitch = 0.65;
  distance = 19;
  camera = {6.5, 2.8, 8.5};
  yaw = .55;
  pitch = .25;
  syncItem();
  SendMessageW(trace, BM_SETCHECK, BST_UNCHECKED, 0);
  SetWindowTextW(
      inspect,
      L"点击视图锁定鼠标；Esc 释放。\r\n左键破坏，右键放置，中键取物\r\n滚轮或 "
      L"1–9 切换快捷栏\r\nE 背包，WASD 飞行\r\n空格上升，Shift 下降\r\nF "
      L"单步，P 运行/暂停");
  refresh();
}
std::array<float, 3> color(const BlockState &s) {
  const auto &n = s.name();
  if (n.find("grass") != std::string::npos)
    return {.35f, .65f, .29f};
  if (n.find("slime") != std::string::npos)
    return {.45f, .83f, .32f};
  if (n.find("honey") != std::string::npos)
    return {.95f, .65f, .18f};
  if (n.find("bedrock") != std::string::npos)
    return {.24f, .26f, .29f};
  if (n.find("obsidian") != std::string::npos)
    return {.22f, .15f, .32f};
  if (n.find("glass") != std::string::npos)
    return {.52f, .81f, .86f};
  if (n.find("redstone_block") != std::string::npos)
    return {.87f, .16f, .19f};
  if (n.find("tnt") != std::string::npos)
    return {.94f, .33f, .23f};
  if (n.find("observer") != std::string::npos)
    return s.get("powered") == "true" ? std::array<float, 3>{1, .3f, .23f}
                                      : std::array<float, 3>{.43f, .48f, .54f};
  if (n.find("piston") != std::string::npos)
    return n.find("sticky") != std::string::npos
               ? std::array<float, 3>{.55f, .66f, .33f}
               : std::array<float, 3>{.72f, .58f, .35f};
  if (n.find("note_block") != std::string::npos)
    return {.53f, .31f, .19f};
  if (n.find("terracotta") != std::string::npos)
    return {.85f, .76f, .69f};
  if (n.find("coral") != std::string::npos)
    return {.52f, .48f, .43f};
  return {.63f, .66f, .69f};
}
VoxelShape visibleShape(World &w, Pos p, const BlockState &s) {
  auto boxes = collisionShape(w, p, s);
  if (boxes.empty() && isRailBlock(s)) {
    boxes.push_back({0, 0, 0, 16,
                     railShapeIsSlope(railShapeOf(s)) ? 8.0 : 2.0, 16});
    return boxes;
  }
  if (boxes.empty() && isDeadCoralWallFan(s)) {
    auto d = directionFromString(s.get("facing", "north"));
    if (d == Direction::North)
      boxes.push_back({0, 4, 5, 16, 12, 16});
    else if (d == Direction::South)
      boxes.push_back({0, 4, 0, 16, 12, 11});
    else if (d == Direction::West)
      boxes.push_back({5, 4, 0, 16, 12, 16});
    else
      boxes.push_back({0, 4, 0, 11, 12, 16});
  }
  if (boxes.empty())
    boxes.push_back(
        {2, 0, 2, 14, 4, 14}); // Non-colliding coral remains selectable.
  return boxes;
}
void entityCube(const Aabb &b, const std::array<float, 3> &c) {
  glDisable(GL_TEXTURE_2D);
  const V3 v[8] = {{b.minX, b.minY, b.minZ}, {b.maxX, b.minY, b.minZ},
                   {b.maxX, b.maxY, b.minZ}, {b.minX, b.maxY, b.minZ},
                   {b.minX, b.minY, b.maxZ}, {b.maxX, b.minY, b.maxZ},
                   {b.maxX, b.maxY, b.maxZ}, {b.minX, b.maxY, b.maxZ}};
  constexpr int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                               {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
  constexpr float shade[6] = {.65f, .85f, .72f, .9f, 1, .5f};
  for (int f = 0; f < 6; f++) {
    glColor3f(c[0] * shade[f], c[1] * shade[f], c[2] * shade[f]);
    glBegin(GL_QUADS);
    for (int i : faces[f])
      glVertex3d(v[i].x, v[i].y, v[i].z);
    glEnd();
  }
}
void cube(const ShapeBox &b, Pos p, const std::array<float, 3> &c,
          bool outline = false, const BlockState *state = nullptr) {
  const double x0 = p.x + b.x0 / 16, y0 = p.y + b.y0 / 16, z0 = p.z + b.z0 / 16,
               x1 = p.x + b.x1 / 16, y1 = p.y + b.y1 / 16, z1 = p.z + b.z1 / 16;
  const V3 v[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                   {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
  if (outline) {
    glDisable(GL_TEXTURE_2D);
    glColor3fv(c.data());
    glBegin(GL_LINES);
    for (auto edge : std::array<std::array<int, 2>, 12>{{{0, 1},
                                                         {1, 2},
                                                         {2, 3},
                                                         {3, 0},
                                                         {4, 5},
                                                         {5, 6},
                                                         {6, 7},
                                                         {7, 4},
                                                         {0, 4},
                                                         {1, 5},
                                                         {2, 6},
                                                         {3, 7}}})
      for (int i : edge)
        glVertex3d(v[i].x, v[i].y, v[i].z);
    glEnd();
    return;
  }
  constexpr int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
                               {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
  constexpr Direction directions[6] = {Direction::North, Direction::South,
                                       Direction::West,  Direction::East,
                                       Direction::Up,    Direction::Down};
  constexpr float shade[6] = {.65f, .85f, .72f, .9f, 1, .5f};
  for (int f = 0; f < 6; f++) {
    auto face = directions[f];
    if (state) {
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D,
                    app->textures.get(client::faceTexture(*state, face)));
    } else
      glDisable(GL_TEXTURE_2D);
    V3 normal{};
    auto normalPos = Pos{}.relative(face);
    normal = {double(normalPos.x), double(normalPos.y), double(normalPos.z)};
    V3 vertical = (face == Direction::Up || face == Direction::Down)
                      ? V3{0, 0, -1}
                      : V3{0, 1, 0};
    if (state && state->hasProperty("facing")) {
      auto facing = directionFromString(state->get("facing"));
      auto fp = Pos{}.relative(facing);
      V3 front{double(fp.x), double(fp.y), double(fp.z)};
      if (state->name().find("piston") != std::string::npos &&
          std::abs(dot(front, normal)) < .5)
        vertical = front;
      else if (facing == Direction::Up || facing == Direction::Down)
        vertical = V3{0, 0, -front.y};
      if (std::abs(dot(vertical, normal)) > .5)
        vertical = {0, 1, 0};
    }
    auto horizontal = cross(vertical, normal);
    auto paint = [&](bool overlay) {
      float r = 1, g = 1, blue = 1;
      if (state && state->is(Blocks::GRASS_BLOCK) &&
          (face == Direction::Up || overlay)) {
        r = .57f;
        g = .77f;
        blue = .35f;
      }
      if (!state) {
        r = c[0];
        g = c[1];
        blue = c[2];
      }
      glColor3f(r * shade[f], g * shade[f], blue * shade[f]);
      glBegin(GL_QUADS);
      for (int i : faces[f]) {
        V3 local = v[i] - V3{p.x + .5, p.y + .5, p.z + .5};
        glTexCoord2d(.5 + dot(local, horizontal), .5 - dot(local, vertical));
        glVertex3d(v[i].x, v[i].y, v[i].z);
      }
      glEnd();
    };
    paint(false);
    if (state && state->is(Blocks::GRASS_BLOCK) &&
        face != Direction::Up && face != Direction::Down &&
        state->get("snowy") != "true") {
      glBindTexture(GL_TEXTURE_2D,
                    app->textures.get("grass_block_side_overlay"));
      glDepthFunc(GL_LEQUAL);
      paint(true);
      glDepthFunc(GL_LESS);
    }
  }
  glDisable(GL_TEXTURE_2D);
}
void App::inventoryClick(int x, int y) {
  constexpr int cell = 48, cols = 9;
  int hotX = (width - cols * cell) / 2, hotY = height - 72;
  if (x >= hotX && x < hotX + cols * cell && y >= hotY && y < hotY + cell) {
    inventory.selectSlot((x - hotX) / cell);
    syncItem();
    return;
  }
  int rows = (static_cast<int>(catalog.size()) + cols - 1) / cols;
  int left = (width - cols * cell) / 2,
      top = std::max(60, (height - (rows * cell + 90)) / 2) + 48;
  if (x >= left && x < left + cols * cell && y >= top &&
      y < top + rows * cell) {
    int index = ((y - top) / cell) * cols + (x - left) / cell;
    if (index < static_cast<int>(catalog.size())) {
      inventory.selectItem(catalog[index]);
      syncItem();
    }
  }
}
void App::hud() {
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0, width, height, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_TEXTURE_2D);
  if (!fontBase) {
    fontBase = glGenLists(96);
    auto old = SelectObject(dc, font);
    wglUseFontBitmapsA(dc, 32, 96, fontBase);
    SelectObject(dc, old);
  }
  auto text = [&](int x, int y, const std::string &s) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    glRasterPos2i(x, y + 15);
    glListBase(fontBase - 32);
    glCallLists(static_cast<GLsizei>(s.size()), GL_UNSIGNED_BYTE, s.data());
  };
  auto rect = [](int x, int y, int w, int h, float r, float g, float b,
                 float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2i(x, y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x, y + h);
    glEnd();
  };
  auto icon = [&](int x, int y, const std::string &item) {
    auto state = defaultState(item);
    auto texture = client::faceTexture(state, Direction::Up);
    if (item == "minecraft:piston")
      texture = "piston_top";
    if (item == "minecraft:sticky_piston")
      texture = "piston_top_sticky";
    if (item == "minecraft:observer")
      texture = "observer_front";
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, textures.get(texture));
    if (item == "minecraft:grass_block")
      glColor4f(.57f, .77f, .35f, 1);
    else
      glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2i(x, y);
    glTexCoord2f(1, 0);
    glVertex2i(x + 32, y);
    glTexCoord2f(1, 1);
    glVertex2i(x + 32, y + 32);
    glTexCoord2f(0, 1);
    glVertex2i(x, y + 32);
    glEnd();
    glDisable(GL_TEXTURE_2D);
  };
  constexpr int cell = 48, cols = 9;
  int hotX = (width - cols * cell) / 2, hotY = height - 72;
  if (inventory.inventoryOpen) {
    rect(0, 0, width, height, 0, 0, 0, .58f);
    int rows = (static_cast<int>(catalog.size()) + cols - 1) / cols;
    int top = std::max(60, (height - (rows * cell + 90)) / 2), left = hotX;
    rect(left - 16, top, cols * cell + 32, rows * cell + 80, .20f, .22f, .25f,
         .98f);
    text(left, top + 12, "Creative inventory  |  E / Esc: return");
    for (size_t i = 0; i < catalog.size(); i++) {
      int x = left + static_cast<int>(i % cols) * cell,
          y = top + 48 + static_cast<int>(i / cols) * cell;
      rect(x + 1, y + 1, 46, 46, .36f, .37f, .40f, 1);
      icon(x + 8, y + 8, catalog[i]);
    }
    text(left, top + rows * cell + 55,
         "Click item: assign to selected hotbar slot");
  } else {
    text(16, 14,
         mouseLocked
             ? "LMB break  RMB place  MMB pick  E inventory  Esc release"
             : "Click the view to capture mouse  |  E: inventory");
    glDisable(GL_TEXTURE_2D);
    glLineWidth(3);
    glColor4f(0, 0, 0, .8f);
    glBegin(GL_LINES);
    glVertex2i(width / 2 - 8, height / 2);
    glVertex2i(width / 2 + 8, height / 2);
    glVertex2i(width / 2, height / 2 - 8);
    glVertex2i(width / 2, height / 2 + 8);
    glEnd();
    glLineWidth(1);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_LINES);
    glVertex2i(width / 2 - 7, height / 2);
    glVertex2i(width / 2 + 7, height / 2);
    glVertex2i(width / 2, height / 2 - 7);
    glVertex2i(width / 2, height / 2 + 7);
    glEnd();
  }
  rect(hotX - 4, hotY - 4, cols * cell + 8, cell + 8, .12f, .12f, .14f, .94f);
  for (int i = 0; i < cols; i++) {
    int x = hotX + i * cell;
    if (i == inventory.selectedSlot)
      rect(x - 1, hotY - 1, 50, 50, .94f, .94f, .94f, 1);
    rect(x + 2, hotY + 2, 44, 44, .30f, .31f, .33f, .98f);
    icon(x + 8, hotY + 8, inventory.slots[i]);
  }
  auto name = inventory.selectedItem();
  if (name.starts_with("minecraft:"))
    name = name.substr(10);
  text(hotX, hotY - 26,
       std::to_string(inventory.selectedSlot + 1) + "  " + name);
  glColor4f(1, 1, 1, 1);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_ALPHA_TEST);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
}
void App::render() {
  if (!gl)
    return;
  wglMakeCurrent(dc, gl);
  glViewport(0, 0, width, height);
  glClearColor(.10f, .13f, .18f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, .1f);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  double top = .1 * std::tan(pi / 6);
  double aspect = double(width) / height;
  glFrustum(-top * aspect, top * aspect, -top, top, .1, 300);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  auto e = eye(), f = forward(), r = normalized(cross(f, {0, 1, 0})),
       u = cross(r, f);
  double m[16] = {r.x, u.x, -f.x, 0, r.y, u.y, -f.y, 0,
                  r.z, u.z, -f.z, 0, 0,   0,   0,    1};
  glMultMatrixd(m);
  glTranslated(-e.x, -e.y, -e.z);
  glColor3f(.23f, .29f, .36f);
  glBegin(GL_LINES);
  int cx = static_cast<int>(std::floor(focus.x)),
      cz = static_cast<int>(std::floor(focus.z));
  for (int i = -24; i <= 24; i++) {
    glVertex3d(cx + i, layer - .01, cz - 24);
    glVertex3d(cx + i, layer - .01, cz + 24);
    glVertex3d(cx - 24, layer - .01, cz + i);
    glVertex3d(cx + 24, layer - .01, cz + i);
  }
  glEnd();
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1, 1);
  for (auto &b : blocks) {
    V3 offset{b.pos.x - focus.x, b.pos.y - focus.y, b.pos.z - focus.z};
    if (dot(offset, offset) > 64 * 64)
      continue;
    auto shapes = visibleShape(*world, b.pos, b.state);
    auto display = b.state;
    if (auto it = world->pistonEntities.find(b.pos);
        it != world->pistonEntities.end())
      display = it->second->movedState;
    auto c = color(display);
    for (auto &box : shapes)
      cube(box, b.pos, c, false, &display);
    // Facing marks are intentionally schematic; no textures/assets required.
    if (b.state.hasProperty("facing")) {
      auto d = directionFromString(b.state.get("facing"));
      auto q = b.pos.relative(d);
      V3 v{double(q.x - b.pos.x), double(q.y - b.pos.y), double(q.z - b.pos.z)};
      V3 center{b.pos.x + .5, b.pos.y + .5, b.pos.z + .5};
      auto tip = center + v * .63;
      glLineWidth(3);
      glColor3f(1, .92f, .55f);
      glBegin(GL_LINES);
      glVertex3d(center.x, center.y, center.z);
      glVertex3d(tip.x, tip.y, tip.z);
      glEnd();
      glLineWidth(1);
    }
  }
  // TODO: entity models, item sprites, rail textures, and entity interpolation.
  // Simple entities are drawn as their collision boxes with a debug colour.
  for (auto &entity : world->entities) {
    if (entity->isRemoved())
      continue;
    auto box = entity->boundingBox();
    V3 offset{entity->position.x - focus.x, entity->position.y - focus.y,
              entity->position.z - focus.z};
    if (dot(offset, offset) > 64 * 64)
      continue;
    std::array<float, 3> c;
    if (auto *item = dynamic_cast<redstone::ItemEntity *>(entity.get()))
      c = item->item.empty() ? std::array<float, 3>{.8f, .8f, .8f}
                             : color(defaultState(item->item.id));
    else if (dynamic_cast<redstone::PrimedTnt *>(entity.get()))
      c = {.94f, .33f, .23f};
    else if (dynamic_cast<redstone::Minecart *>(entity.get()))
      c = {.42f, .48f, .55f};
    else
      c = {.63f, .66f, .69f};
    entityCube(box, c);
  }
  glDisable(GL_POLYGON_OFFSET_FILL);
  glLineWidth(2);
  if (hover)
    cube({-.04, -.04, -.04, 16.04, 16.04, 16.04}, hover->pos, {.8f, .9f, 1},
         true);
  glLineWidth(1);
  hud();
  if (!previewPath.empty()) {
    const int stride = (width * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(size_t(stride) * height);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
        std::swap(pixels[size_t(y) * stride + x * 3],
                  pixels[size_t(y) * stride + x * 3 + 2]);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    std::ofstream output(previewPath, std::ios::binary);
    output.write(reinterpret_cast<const char *>(&file), sizeof(file));
    output.write(reinterpret_cast<const char *>(&info), sizeof(info));
    output.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
    previewPath.clear();
  }
  SwapBuffers(dc);
}
std::optional<Hit> intersect(V3 origin, V3 ray, Pos p, const ShapeBox &box) {
  double lo = 0, hi = 300;
  Direction side = Direction::Up;
  double o[3] = {origin.x, origin.y, origin.z}, d[3] = {ray.x, ray.y, ray.z};
  double a[3] = {p.x + box.x0 / 16, p.y + box.y0 / 16, p.z + box.z0 / 16},
         b[3] = {p.x + box.x1 / 16, p.y + box.y1 / 16, p.z + box.z1 / 16};
  constexpr Direction negative[3] = {Direction::West, Direction::Down,
                                     Direction::North},
                      positive[3] = {Direction::East, Direction::Up,
                                     Direction::South};
  for (int axis = 0; axis < 3; axis++) {
    if (std::abs(d[axis]) < 1e-9) {
      if (o[axis] < a[axis] || o[axis] > b[axis])
        return {};
      continue;
    }
    double t0 = (a[axis] - o[axis]) / d[axis],
           t1 = (b[axis] - o[axis]) / d[axis];
    auto face = negative[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
      face = positive[axis];
    }
    if (t0 > lo) {
      lo = t0;
      side = face;
    }
    hi = std::min(hi, t1);
    if (lo > hi)
      return {};
  }
  return Hit{p, side, lo};
}
void App::pick(int x, int y) {
  auto e = eye(), f = forward(), r = normalized(cross(f, {0, 1, 0})),
       u = cross(r, f);
  auto ray = normalized(
      f +
      r * ((2. * x / width - 1) * double(width) / height * std::tan(pi / 6)) +
      u * ((1 - 2. * y / height) * std::tan(pi / 6)));
  hover.reset();
  planeCell.reset();
  for (auto &b : blocks)
    for (auto &shape : visibleShape(*world, b.pos, b.state))
      if (auto hit = intersect(e, ray, b.pos, shape);
          hit && hit->distance <= 5 &&
          (!hover || hit->distance < hover->distance))
        hover = hit;
  InvalidateRect(view, nullptr, FALSE);
}
void App::edit(bool remove) {
  if (inventory.inventoryOpen)
    return;
  pick(width / 2, height / 2);
  auto target = hover;
  if (remove) {
    if (target)
      world->setBlock(target->pos, defaultState("air"));
  } else {
    auto pos = target ? std::optional<Pos>(target->pos.relative(target->face))
                      : planeCell;
    if (!pos)
      return;
    if (pos->y < world->minY() || pos->y >= world->maxY())
      return;
    auto existing=world->getBlock(*pos);
    if(existing.name()!="minecraft:air"&&existing.name()!="minecraft:cave_air"&&existing.name()!="minecraft:void_air")return;
    int cx = pos->x >= 0 ? pos->x / 16 : -1 - ((-1 - pos->x) / 16),
        cz = pos->z >= 0 ? pos->z / 16 : -1 - ((-1 - pos->z) / 16);
    world->loadChunk(cx, cz);
    auto state = defaultState(inventory.selectedItem());
    if (isDeadCoralFan(state) || isDeadCoralWallFan(state)) {
      auto placed = world->coralPlacementState(
          *pos, state, target ? target->face : Direction::Up);
      if (!placed)
        return;
      state = *placed;
    } else if (state.hasProperty("facing")) {
      auto f = forward();
      Direction look;
      if (std::abs(f.y) > std::max(std::abs(f.x), std::abs(f.z)))
        look = f.y > 0 ? Direction::Up : Direction::Down;
      else if (std::abs(f.x) > std::abs(f.z))
        look = f.x > 0 ? Direction::East : Direction::West;
      else
        look = f.z > 0 ? Direction::South : Direction::North;
      // Observer looks with the player; piston head points toward the player.
      state =
          state.with("facing", directionName(state.is(Blocks::OBSERVER)
                                                 ? look
                                                 : opposite(look)));
    } else if (state.hasProperty("shape")) {
      auto f = forward();
      Direction look;
      if (std::abs(f.x) > std::abs(f.z))
        look = f.x > 0 ? Direction::East : Direction::West;
      else
        look = f.z > 0 ? Direction::South : Direction::North;
      state = state.with("shape",
                         look == Direction::East || look == Direction::West
                             ? "east_west"
                             : "north_south");
    }
    if (state.hasProperty("waterlogged"))
      state = state.with("waterlogged", "false");
    // TODO: port player BlockPlaceContext (replacement, head placement etc.).
    // This is an editor placement, not an emulation of player interactions.
    if (isWoolCarpet(state) &&
        isAirState(world->getBlock(pos->relative(Direction::Down))))
      return;
    if (isRailBlock(state) && !railCanSurvive(*world, *pos, state))
      return;
    // Observer.updateShape has scheduling side effects; placing a fresh
    // observer must not generate a pulse by speculative shape queries.
    if (isWallBlock(state) || state.is(Blocks::GRASS_BLOCK) ||
        state.is(Blocks::NOTE_BLOCK))
      state = world->updateFromNeighbourShapes(*pos, state);
    if (state.is(Blocks::REDSTONE_LAMP))
      state = state.with("lit", world->hasNeighborSignal(*pos) ? "true" : "false");
    world->setBlock(*pos, state);
  }
  selected = target;
  refresh();
}
void App::load() {
  lockMouse(false);
  inventory.inventoryOpen = false;
  running = false;
  SetWindowTextW(runButton, L"运行 / Run");
  BROWSEINFOW browse{};
  browse.hwndOwner = window;
  browse.lpszTitle = L"选择含 level.dat 的 26.1 存档文件夹";
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  auto pid = SHBrowseForFolderW(&browse);
  if (!pid)
    return;
  wchar_t folder[MAX_PATH]{};
  bool ok = SHGetPathFromIDListW(pid, folder) != FALSE;
  CoTaskMemFree(pid);
  if (!ok)
    return;
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  try {
    auto data =
        loadAnvilWorld(folder, defaultRegionDirectory(folder));
    auto next = std::make_unique<World>();
    importWorld(data, *next);
    world = std::move(next);
    world->traceUpdates = SendMessageW(trace, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(GetDlgItem(window, Trigger), FALSE);
    history.clear();
    selected.reset();
    hover.reset();
    if (!data.blocks.empty()) {
      auto p = data.blocks.begin()->first;
      for (auto &[q, s] : data.blocks)
        if (q.x == p.x && q.z == p.z && q.y > p.y)
          p = q;
      focus = {p.x + .5, p.y + 1., p.z + .5};
      camera = focus + V3{3, 2, 5};
      yaw = .55;
      pitch = .25;
      layer = p.y + 1;
      SetWindowTextW(plane, std::to_wstring(layer).c_str());
    }
    size_t unsupported = 0;
    for (auto &[p, s] : data.blocks)
      if (!isSupportedBlock(s.name()))
        unsupported++;
    history.push_back(L"读取 " + std::to_wstring(data.chunks.size()) +
                      L" chunks; 未支持方块 " + std::to_wstring(unsupported));
    history.push_back(
        L"流体刻、其他实体和方块实体未模拟；简单实体与铁轨已参与。只读存档。");
    refresh();
  } catch (const std::exception &e) {
    MessageBoxW(window, wide(e.what()).c_str(), L"存档读取失败", MB_ICONERROR);
  }
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}
LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!app)
    return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
  case WM_ERASEBKGND:
    return 1;
  case WM_GETDLGCODE:
    return wp == VK_TAB ? 0 : DLGC_WANTALLKEYS;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    app->render();
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_INPUT: {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size,
                        sizeof(RAWINPUTHEADER)) != UINT(-1) &&
        raw.header.dwType == RIM_TYPEMOUSE && app->mouseLocked &&
        !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
      app->yaw -= raw.data.mouse.lLastX * .0025;
      app->pitch =
          std::clamp(app->pitch + raw.data.mouse.lLastY * .0025, -1.55, 1.55);
      app->focus = app->camera + app->forward() * 5;
      app->pick(app->width / 2, app->height / 2);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  case WM_KILLFOCUS:
    app->lockMouse(false);
    return 0;
  case WM_CAPTURECHANGED:
    if (app->mouseLocked && GetCapture() != hwnd)
      app->lockMouse(false);
    return 0;
  case WM_MOUSEMOVE:
    app->lastX = GET_X_LPARAM(lp);
    app->lastY = GET_Y_LPARAM(lp);
    return 0;
  case WM_RBUTTONDOWN:
    if (app->inventory.inventoryOpen)
      return 0;
    if (!app->mouseLocked) {
      app->lockMouse(true);
      return 0;
    }
    app->placeHeld = true;
    app->lastPlace = GetTickCount64();
    app->edit(false);
    return 0;
  case WM_RBUTTONUP:
    app->placeHeld = false;
    return 0;
  case WM_MOUSEWHEEL:
    if (!app->inventory.inventoryOpen) {
      app->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
      int n = app->wheelRemainder / WHEEL_DELTA;
      app->wheelRemainder %= WHEEL_DELTA;
      app->inventory.scroll(n);
      app->syncItem();
    }
    return 0;
  case WM_LBUTTONDOWN:
    if (app->inventory.inventoryOpen) {
      app->inventoryClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    }
    if (!app->mouseLocked) {
      app->lockMouse(true);
      return 0;
    }
    app->breakHeld = true;
    app->lastBreak = GetTickCount64();
    app->edit(true);
    return 0;
  case WM_LBUTTONUP:
    app->breakHeld = false;
    return 0;
  case WM_MBUTTONDOWN:
    if (app->mouseLocked) {
      app->pick(app->width / 2, app->height / 2);
      if (app->hover) {
        auto s = app->world->getBlock(app->hover->pos);
        if (auto it = app->world->pistonEntities.find(app->hover->pos);
            it != app->world->pistonEntities.end())
          s = it->second->movedState;
        app->inventory.pickBlock(s);
        app->syncItem();
      }
    }
    return 0;
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) {
      if (app->inventory.inventoryOpen)
        app->toggleInventory();
      else
        app->lockMouse(false);
      return 0;
    }
    if (wp == 'E' && !(lp & (1LL << 30))) {
      app->toggleInventory();
      return 0;
    }
    if (wp >= '1' && wp <= '9') {
      app->inventory.selectSlot(static_cast<int>(wp - '1'));
      app->syncItem();
      return 0;
    }
    if (wp == 'P' && !(lp & (1LL << 30)))
      SendMessageW(app->window, WM_COMMAND, Run, 0);
    else if (wp == 'F')
      app->tick();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_ACTIVATEAPP:
    if (!wp && app)
      app->lockMouse(false);
    break;
  case WM_SIZE:
    if (app && app->view)
      app->layout(LOWORD(lp), HIWORD(lp));
    return 0;
  case WM_GETMINMAXINFO: {
    auto p = reinterpret_cast<MINMAXINFO *>(lp);
    p->ptMinTrackSize = {900, 780};
    return 0;
  }
  case WM_COMMAND:
    if (!app || !app->world)
      break;
    switch (LOWORD(wp)) {
    case Run:
      app->running = !app->running;
      app->lastTick = GetTickCount64();
      SetWindowTextW(app->runButton,
                     app->running ? L"暂停 / Pause" : L"运行 / Run");
      break;
    case Step:
      app->tick();
      break;
    case Reset:
      app->demo();
      break;
    case Trigger: {
      Pos p{5, 0, 4};
      app->world->setBlock(
          p, defaultState(app->world->getBlock(p).is(Blocks::GLASS)
                              ? "smooth_stone"
                              : "glass"));
      app->refresh();
      break;
    }
    case Place:
      app->lockMouse(true);
      break;
    case Remove:
      app->toggleInventory();
      break;
    case Palette:
      if (HIWORD(wp) == CBN_SELCHANGE) {
        auto i = SendMessageW(app->palette, CB_GETCURSEL, 0, 0);
        if (i >= 0 && static_cast<size_t>(i) < app->choices.size())
          app->inventory.selectItem(app->choices[i]);
        app->syncItem();
      }
      break;
    case Open:
      app->load();
      break;
    case Trace:
      app->world->traceUpdates =
          SendMessageW(app->trace, BM_GETCHECK, 0, 0) == BST_CHECKED;
      break;
    case Plane:
      if (HIWORD(wp) == EN_CHANGE) {
        wchar_t s[32]{};
        GetWindowTextW(app->plane, s, 32);
        app->layer =
            std::clamp(_wtoi(s), app->world->minY(), app->world->maxY() - 1);
        InvalidateRect(app->view, nullptr, FALSE);
      }
      break;
    }
    return 0;
  case WM_TIMER:
    if (app)
      app->frame();
    if (app && app->world && app->running && !IsIconic(hwnd)) {
      constexpr int rates[4] = {1, 5, 10, 20};
      auto i = SendMessageW(app->speed, CB_GETCURSEL, 0, 0);
      int interval = 1000 / rates[std::clamp(static_cast<int>(i), 0, 3)];
      auto now = GetTickCount64();
      int budget = 4;
      while (app->running &&
             now - app->lastTick >= static_cast<ULONGLONG>(interval) &&
             budget-- > 0) {
        app->lastTick += interval;
        app->tick();
      }
      if (now - app->lastTick > static_cast<ULONGLONG>(interval * 4))
        app->lastTick = now;
    }
    return 0;
  case WM_DESTROY:
    if (app)
      app->lockMouse(false);
    KillTimer(hwnd, 1);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  App state;
  app = &state;
  int argc{};
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool previewInventory =
      argv && argc == 3 && std::wstring_view(argv[1]) == L"--preview-inventory";
  if (argv && argc == 3 &&
      (std::wstring_view(argv[1]) == L"--preview" || previewInventory))
    state.previewPath = argv[2];
  if (argv)
    LocalFree(argv);
  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"RedstoneDebugClient";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  RegisterClassW(&wc);
  wc.lpfnWndProc = ViewProc;
  wc.lpszClassName = L"RedstoneViewport";
  wc.style = CS_OWNDC;
  wc.hbrBackground = nullptr;
  RegisterClassW(&wc);
  state.window = CreateWindowW(
      L"RedstoneDebugClient", L"Redstone 26.1 — Lightweight Debug Client",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 850, nullptr,
      nullptr, instance, nullptr);
  state.font =
      CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  DEFAULT_PITCH, L"Microsoft YaHei UI");
  state.controls();
  state.view = CreateWindowW(
      L"RedstoneViewport", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 300, 0,
      960, 800, state.window, nullptr, instance, nullptr);
  RAWINPUTDEVICE mouse{0x01, 0x02, 0, state.view};
  RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
  state.dc = GetDC(state.view);
  PIXELFORMATDESCRIPTOR pf{};
  pf.nSize = sizeof(pf);
  pf.nVersion = 1;
  pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pf.iPixelType = PFD_TYPE_RGBA;
  pf.cColorBits = 24;
  pf.cDepthBits = 24;
  pf.iLayerType = PFD_MAIN_PLANE;
  int pixel = ChoosePixelFormat(state.dc, &pf);
  if (!pixel || !SetPixelFormat(state.dc, pixel, &pf) ||
      !(state.gl = wglCreateContext(state.dc))) {
    MessageBoxW(state.window, L"无法创建系统 OpenGL 上下文", L"初始化失败",
                MB_ICONERROR);
    return 1;
  }
  RECT bounds{};
  GetClientRect(state.window, &bounds);
  state.layout(bounds.right, bounds.bottom);
  state.demo();
  if (previewInventory)
    state.inventory.inventoryOpen = true;
  if (!state.previewPath.empty()) {
    state.camera = {13, 10, 13};
    state.yaw = .8;
    state.pitch = .65;
    state.render();
  } else {
    SetTimer(state.window, 1, 16, nullptr);
    ShowWindow(state.window, show);
    UpdateWindow(state.window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.hwnd == state.view || !IsDialogMessageW(state.window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
  wglMakeCurrent(state.dc, state.gl);
  if (state.fontBase)
    glDeleteLists(state.fontBase, 96);
  state.textures.clear();
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(state.gl);
  ReleaseDC(state.view, state.dc);
  DeleteObject(state.font);
  CoUninitialize();
  app = nullptr;
  return 0;
}
