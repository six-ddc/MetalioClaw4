---
name: xingzhi-395-pitfalls
description: >-
  Documents xingzhi-395-app (ESP32-P4) pitfalls: LVGL thread safety, FreeRTOS
  task model, screen lifecycle, display routing. Use when modifying UI screens,
  Application, esp_lv_adapter, fixing Core0 100% CPU, task_wdt IDLE0, status bar,
  chat device state, camera worker, or adding cross-thread UI updates.
---

# xingzhi-395 项目避坑指南

修改 UI、Application、板级初始化或跨线程逻辑前**先读本文**。违反 LVGL 线程规则会导致 Core0 100%、`task_wdt: IDLE0`、UI 卡死。

详细线程表与文件索引见 [reference.md](reference.md)。

---

## 1. 线程模型（先搞清楚「你在哪个线程」）

| 线程 / 任务 | 典型入口 | 能否直接调 LVGL API |
|-------------|----------|---------------------|
| **LVGL 线程** | `esp_lv_adapter` 内部 task；`lv_timer` 回调；`LV_EVENT_*` 回调；`lv_async_call` 回调 | ✅ 可以 |
| **`app_main`** | `main/main.cc` → `Application::Start()` 同步流程（OTA、激活、协议初始化） | ❌ 不可以 |
| **`main_event_loop`** | `Application::MainEventLoop()`；`Application::Schedule()` 投递的任务 | ❌ 不可以 |
| **Worker 任务** | 网络/GPS/相机/AT/IOExpander monitor 等 `xTaskCreate` | ❌ 不可以 |
| **音频任务** | `audio_input`（Core0）、`audio_output`、`opus_codec` | ❌ 不可以 |

**误区**：「main 线程」在本项目里通常指 **`app_main` 任务**，不是 LVGL 线程。`main_event_loop` 也不是 LVGL 线程。

---

## 2. LVGL 黄金规则

### 2.1 禁止跨线程直接操作 LVGL

以下 API **只能在 LVGL 线程**调用：

- `lv_label_set_text` / `lv_obj_add_flag` / `lv_obj_invalidate` / `lv_screen_load` / `lv_timer_create` …
- 各 `*Screen::Create()` 内构建 UI 的代码（除已通过 async/lock 封装的静态 Refresh 入口外）

**已踩坑案例**（均已修复，勿再犯）：

| 入口 | 错误调用链 | 后果 |
|------|------------|------|
| 验证码 | `app_main` → `ShowActivationCode` → `HomeScreen::RefreshStatusBar` → `lv_label_set_text` | Core0 100%，10s WDT |
| 聊天状态 | `main_event_loop` → `SetDeviceState` → `ChatScreen::RefreshDeviceState` → `lv_label_set_text` | 同上 |

### 2.2 跨线程更新 UI 的两种标准写法

**首选：`lv_async_call`（fire-and-forget，不阻塞调用方）**

```cpp
static void OnUiUpdateAsync(void* /*user_data*/) {
    if (!Screen::IsActive()) return;
    DoActualLvglWork();
}

void Screen::RequestUiUpdate() {
    if (!IsActive()) return;
    lv_async_call(OnUiUpdateAsync, nullptr);
}
```

项目内正确范例：

- `HomeScreen::RefreshStatusBar()` → `lv_async_call(OnRefreshStatusBarAsync, …)`
- `ChatScreen::RefreshDeviceState()` → `lv_async_call(on_refresh_device_state_async, …)`
- `network_screen` / `gps_screen` / `bluetooth_screen` / `music_screen` 大量 worker → UI 路径

**备选：`esp_lv_adapter_lock` + `unlock`（调用方需持锁完成整段 UI 操作）**

```cpp
if (esp_lv_adapter_lock(-1) != ESP_OK) return;
if (is_screen_alive()) {
    // LVGL 操作
}
esp_lv_adapter_unlock();
```

项目内正确范例：`LVAdapterDisplay::SetChatMessage`、`openclaw_screen` worker 发气泡、`ota_screen` 全屏 overlay。

**选择建议**：

| 场景 | 推荐 |
|------|------|
| Application / 协议 / 状态机回调刷新 UI | `lv_async_call` |
| Worker 批量改 UI、需与 adapter 任务互斥 | `esp_lv_adapter_lock(-1)` |
| LVGL 定时器 / 事件回调里更新 | 直接写，已在 LVGL 线程 |
| 需要传递堆上数据 | `lv_async_call` + `new` 结构体，回调里 `delete`（见 `gps_screen` / `network_screen`） |

### 2.3 定时器 vs 外部 Refresh

- **`lv_timer_create(..., 500~1000ms, ...)`**：已在 LVGL 线程，可直接 `lv_label_set_text`。
- **从 Application 调用的 `RefreshXxx()` 静态方法**：必须 `lv_async_call`，不能假设在 LVGL 线程。

### 2.4 减少无效重绘

定时器或 Refresh 里更新 label 时**先比较内容/状态**，未变则跳过：

```cpp
if (st->last_text != new_text) {
    st->last_text = new_text;
    lv_label_set_text(lbl, new_text.c_str());
}
```

无条件每秒 `lv_label_set_text` 会反复 `lv_obj_invalidate`，加重 CPU 与撕裂风险。

---

## 3. Application 层雷点

### 3.1 `Application::Start()` 在 `app_main` 上长时间阻塞

`Start()` 同步执行：联网 → `CheckNewVersion`（含激活循环）→ `esp_srmodel_init` → 协议 `Start()`。期间 **Core0 的 IDLE 任务可能饿死**，若再叠加跨线程 LVGL 争锁会触发 WDT。

- 不要在 `Start()` 路径新增同步 LVGL 操作；UI 通知走 `lv_async_call` 或等 `main_event_loop` 跑起来后 `Schedule`。
- `esp_srmodel_init("model")` 较重，避免在 `Start()` 里重复调用。

### 3.2 用 `Schedule()` 访问协议 / 状态，不是 LVGL

```cpp
Application::Schedule([this]() { /* 跑在 main_event_loop */ });
```

`Schedule` **不会**帮你切到 LVGL 线程。在 Schedule 回调里仍不能碰 LVGL。

### 3.3 `SetDeviceState()` 副作用

会调 `display->SetStatus` / `SetEmotion` / `SetChatMessage`（经 `LVAdapterDisplay` 加锁，安全）以及 `ChatScreen::RefreshDeviceState()`（已 async 化）。**新增**按状态刷 UI 的逻辑时，沿用 `ChatScreen::RefreshDeviceState` 模式。

### 3.4 `SendMcpMessage` 线程检测

已在 `main_event_loop` 则同步发送，否则 `Schedule`。仿照此模式处理其它「必须 main loop 执行」的逻辑。

---

## 4. Display / Screen 层雷点

### 4.1 `LVAdapterDisplay` 是 xingzhi-395 的实际 Display

- `UpdateStatusBar()` / `SetStatus()` / `Lock()` 在本工程多为 **空实现或恒 true**。
- 主屏状态栏在 **`HomeScreen::UpdateHomeStatusBar`**（`home_screen.cc`），不是 `LvglDisplay::UpdateStatusBar`。
- 聊天气泡仅在前台 `ChatScreen` 或 `DigitalPeopleScreen` 时由 `SetChatMessage` 路由；其它屏 **直接丢弃**，勿指望后台缓存。

### 4.2 Screen 生命周期

- 用 `screen_attach_lifecycle(scr, cb)` 注册 LOAD/UNLOAD。
- LOAD/UNLOAD 回调在 **LVGL 线程**触发（`screen_util.cc`）。
- UNLOAD 时必须：删 `lv_timer`、清静态 `s_screen` / `s_*` 指针，防止 `lv_async_call` 回调访问野指针（见 `call_screen`、`chat_screen` 注释）。

### 4.3 `IsActive()` 守卫

跨线程 UI 入口先检查：

```cpp
if (!ChatScreen::IsActive()) return;
```

屏幕不在前台时不更新、不堆积消息（聊天屏设计如此）。

### 4.4 相机屏：禁止在 LVGL 线程同步等 worker

`camera_screen.cc` 头部注释：**绝不在主线程同步等待 camera worker**，否则 adapter 拿不到锁，UI 卡 1s+。Worker 用 `xTaskCreatePinnedToCore`，UI 更新用短超时 `esp_lv_adapter_lock(20)`。

### 4.5 IOExpander / 按键

`IOExpander` monitor task 检测长按后必须 `lv_async_call` 到 UI（见 `home_screen` 电源键、`IOExpander.hpp` 注释）。

---

## 5. 音频与 Core 分配

- `audio_input` ** pinned Core0**，priority 8。
- AFE wake word 偏好 Core1（`afe_wake_word.cc` `afe_perferred_core = 1`）。
- Core0 100% 不一定是 LVGL：也可能是 `app_main` 阻塞 + 音频 + 其它 Core0 任务。看 WDT 报 **`Tasks currently running: CPU 0: main`** 还是别的 task 名。

---

## 6. 新增 UI 功能检查清单

提交前逐项确认：

```
[ ] 调用链起点线程是谁？（app_main / main_event_loop / worker / LVGL）
[ ] 是否调用了 lv_* / lvgl 屏幕静态 Refresh？若在非 LVGL 线程 → lv_async_call 或 esp_lv_adapter_lock
[ ] Refresh 入口是否 IsActive() 守卫？
[ ] 定时更新是否缓存旧值，避免重复 set_text？
[ ] UNLOAD 是否清理 timer 与静态指针？
[ ] Worker 是否阻塞等待 LVGL？（应 async，不 vTaskDelay 等 UI）
[ ] 是否在 Application::Start 同步路径加了重 CPU / 大内存操作？
```

---

## 7. 调试 CPU 100% / WDT

1. 看 WDT 日志 **`Tasks currently running: CPU 0: ???`** 确定占满 CPU 的任务名。
2. 用 `build/xiaozhi.map` 或 `addr2line -e build/xiaozhi.elf <PC>` 解回溯。
3. 若回溯含 `lv_obj_invalidate` / `lv_inv_area` / `lv_label_set_text` 且 task 为 `main` 或 `main_event_loop` → 几乎一定是跨线程 LVGL。
4. 板级 CPU 日志在 `xingzhi-395.cc` 独立 task，每秒打印 Core0/Core1。

---

## 8. 相关文件速查

| 主题 | 文件 |
|------|------|
| 启动与激活 | `main/application.cc` |
| 主屏状态栏 | `main/display/screen/home_screen/home_screen.cc` |
| 聊天状态 | `main/display/screen/chat_screen/chat_screen.cc` |
| Display 路由 | `main/display/lv_adapter_display.cc` |
| 生命周期工具 | `main/display/screen/screen_util.{h,cc}` |
| 相机 worker | `main/display/screen/camera_screen/camera_screen.cc` |
| OpenClaw 加锁范例 | `main/display/screen/openclaw_screen/openclaw_screen.cc` |
| 板级 CPU 监控 | `main/boards/xingzhi-395/xingzhi-395.cc` |

更多线程→文件映射见 [reference.md](reference.md)。
