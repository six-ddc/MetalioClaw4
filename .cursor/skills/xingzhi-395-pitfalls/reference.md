# xingzhi-395 避坑参考（线程映射与代码范例）

## 线程 → 常见调用源

```
app_main
├── Application::Start()
│   ├── board.StartNetwork()
│   ├── CheckNewVersion() / ShowActivationCode()
│   ├── esp_srmodel_init("model")
│   └── protocol_->Start()
└── （Start 返回后 app_main 基本空闲）

main_event_loop
├── xEventGroupWaitBits 驱动
├── protocol 收发 / Schedule 队列
├── Application::SetDeviceState()
├── Application::OnWakeWordDetected()
└── MAIN_EVENT_CLOCK_TICK → display->UpdateStatusBar()（LVAdapterDisplay 空实现）

LVGL 线程（esp_lv_adapter）
├── lv_timer 回调（home 状态栏 1s、chat 状态 500ms）
├── lv_async_call 回调
├── LV_EVENT_SCREEN_LOADED / UNLOADED / CLICKED ...
├── LVAdapterDisplay::SetupUI / boot→home 切换
└── screen_attach_lifecycle 回调

Worker 任务（均需 async/lock 才能碰 UI）
├── BootSimSlotQueryTask (home_screen)
├── camera_worker_task (camera_screen, Core1)
├── GPS / network / bluetooth / music / call AT workers
├── OpenClaw WebSocket worker
└── IOExpander monitor task
```

## 跨线程 UI：项目内标准模板

### 模板 A — Application 触发的 Screen Refresh（推荐）

```cpp
// xxx_screen.cc
namespace {
void on_refresh_async(void* /*user_data*/) {
    if (!XxxScreen::IsActive()) return;
    update_xxx_ui();  // 内部 lv_label_set_text 等
}
}

void XxxScreen::RefreshFromApp() {
    if (!IsActive()) return;
    lv_async_call(on_refresh_async, nullptr);
}

// application.cc — 从 SetDeviceState / CheckNewVersion 等调用
XxxScreen::RefreshFromApp();
```

**已落地实例**：

- `HomeScreen::RefreshStatusBar()` — `home_screen.cc:1751`
- `ChatScreen::RefreshDeviceState()` — `chat_screen.cc:470`

### 模板 B — Worker 持锁更新（OpenClaw / OTA / Display 路由）

```cpp
void worker_thread_func() {
    // ... 网络/解析 ...
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (Screen::IsActive()) {
        // lv_* 操作
    }
    esp_lv_adapter_unlock();
}
```

**已落地实例**：

- `LVAdapterDisplay::SetChatMessage` — 先判断前台屏，再加锁
- `openclaw_screen::post_bubble_from_worker`
- `ota_screen` 进度 overlay

### 模板 C — Worker + 堆数据 + lv_async_call（GPS / Network）

```cpp
struct AsyncMsg { std::string text; /* ... */ };

void worker() {
    auto* msg = new AsyncMsg{...};
    lv_async_call([](void* p) {
        std::unique_ptr<AsyncMsg> msg(static_cast<AsyncMsg*>(p));
        if (!Screen::IsActive()) return;
        apply_to_ui(msg.get());
    }, msg);
}
```

注意：回调里必须 `delete` 或 `unique_ptr` 释放；UNLOAD 时用 session id 丢弃过期回调（`gps_screen`）。

### 模板 D — 已在 LVGL 线程（无需 async）

```cpp
void on_timer(lv_timer_t* t) {
    update_label_if_changed();  // 可直接 lv_*
}

void on_btn_click(lv_event_t* e) {
    ChatScreen::RefreshDeviceState();  // 内部 lv_async_call，重复调用也安全
}
```

## 反例（禁止）

```cpp
// BAD: app_main / main_event_loop 直接 LVGL
void Application::SetDeviceState(...) {
    ChatScreen::RefreshDeviceState();  // 若内部直接 lv_label_set_text → 炸
}

void HomeScreen::RefreshStatusBar() {
    UpdateHomeStatusBar(s_home_status);  // BAD 若从 app_main 调用
}

// BAD: worker 阻塞等 UI
void on_screen_load() {
    start_worker();
    vTaskDelay(pdMS_TO_TICKS(2000));  // 等 worker → LVGL 锁饿死
    update_ui();
}

// BAD: 定时器无条件 set_text
void on_timer(lv_timer_t*) {
    lv_label_set_text(lbl, same_text_every_second);  // 无意义 invalidate
}
```

## Display 抽象层陷阱

| 方法 | xingzhi-395 实际行为 |
|------|----------------------|
| `LVAdapterDisplay::UpdateStatusBar` | 空函数；状态栏由 HomeScreen 自管 |
| `LVAdapterDisplay::SetStatus` | 空函数 |
| `LVAdapterDisplay::Lock` | 恒返回 `true`；**不能**依赖 Display 锁保护 LVGL |
| `LVAdapterDisplay::SetChatMessage` | 加 `esp_lv_adapter_lock`，仅 chat/digital_people 前台 |
| `LVAdapterDisplay::SetEmotion` | 加锁后转 `DigitalPeopleScreen::SetEmotion` |

在 xingzhi-395 上改「状态栏 / 通知」应直接改 `home_screen.cc`，不要改 `lvgl_display.cc`  expecting 它会显示。

## Screen 静态状态生命周期

典型模式（chat_screen）：

```cpp
lv_obj_t* s_screen = nullptr;

bool ChatScreen::IsActive() {
    return s_screen != nullptr && s_msg_list != nullptr;
}

void on_screen_unloaded(lv_event_t*) {
    if (s_state_timer) { lv_timer_delete(s_state_timer); s_state_timer = nullptr; }
    s_screen = nullptr;
    // 清所有静态 widget 指针
}
```

`HomeScreen::Create()` 会被多个子页 `on_swipe_back` **重复调用**重建；不要用全局假设「home 只创建一次」。

## Application 状态与唤醒词

- 聊天屏 / 数字人屏 LOAD：`EnableWakeWordDetection(true)`
- UNLOAD：`ForceReturnToIdle()` + `EnableWakeWordDetection(false)`
- 全局 idle 也会开唤醒词；多屏切换时注意 `LifecycleCallback` 成对注册/注销 PWR 键（`screen_register_pwr_key_toggle_chat`）

## OTA / 升级

- `CheckNewVersion` 在 `app_main` 阻塞；激活失败会 `vTaskDelay` 重试。
- OTA 下载大循环里曾注释 `esp_task_wdt_reset()`（`ota.cc`）；长下载可能 WDT，升级 UI 走 `OtaScreen` + lock/async。
- `LvglPauseGuard` 在 OTA 时 pause adapter（`ota.cc`）。

## 内存与 PSRAM

- 聊天气泡 `kMaxMessages = 12`，超出删最旧。
- 相机 canvas buffer 只分配一次，不随进出屏释放（`camera_screen.cc`）。
- LVGL triple buffer 需 3 张 panel FB（`xingzhi-395.cc` 注释）。

## WDT 配置

`sdkconfig`: `CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`，检查 IDLE0/IDLE1。跨线程 LVGL 争锁约 10s 触发 `task_wdt: - IDLE0 (CPU 0)`。

## 修改 UI 时的 grep 命令

```bash
# 找可能跨线程的 Refresh 入口
rg "Refresh[A-Z]|lv_label_set_text|lv_obj_" main/application.cc main/display/

# 找已正确使用 async 的文件（对照范例）
rg "lv_async_call|esp_lv_adapter_lock" main/display/screen/

# 找从 Application 调 Screen 的路径
rg "Screen::" main/application.cc
```
