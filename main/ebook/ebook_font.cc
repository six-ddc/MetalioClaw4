// ebook_font.cc — 见头文件说明。

#include "ebook_font.h"

#include "book_store.h"

#include "sdkconfig.h"

#include "SdCardManager.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#if defined(CONFIG_LV_USE_FREETYPE) && defined(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE)
#define EBOOK_FONT_FT 1
#include "esp_lv_adapter.h"
#else
#define EBOOK_FONT_FT 0
#endif

namespace ebook_font {

namespace {

constexpr char TAG[] = "ebook_font";

bool SuffixIs(const char* name, size_t n, const char* ext) {
    size_t el = strlen(ext);
    if (n <= el) return false;
    return strcasecmp(name + n - el, ext) == 0;
}

bool IsFontName(const char* name, size_t* ext_len) {
    size_t n = strlen(name);
    if (SuffixIs(name, n, ".ttf")) {
        *ext_len = 4;
        return true;
    }
    if (SuffixIs(name, n, ".otf")) {
        *ext_len = 4;
        return true;
    }
    return false;
}

#if EBOOK_FONT_FT
// 已激活的 FT 字体（正文 + 标题）。
esp_lv_adapter_ft_font_handle_t s_body_h = nullptr;
esp_lv_adapter_ft_font_handle_t s_head_h = nullptr;
const lv_font_t* s_body = nullptr;
const lv_font_t* s_head = nullptr;
bool s_active = false;
char s_cur_file[64] = {0};
int s_cur_body_px = 0;
int s_cur_head_px = 0;

uint32_t s_fence_counter = 0;

// 退休字体（等 worker fence 到达再 deinit）。
struct Grave {
    esp_lv_adapter_ft_font_handle_t body;
    esp_lv_adapter_ft_font_handle_t head;
    uint32_t fence;
};
std::vector<Grave> s_graveyard;

struct LockGuard {
    bool ok;
    LockGuard() { ok = (esp_lv_adapter_lock(-1) == ESP_OK); }
    ~LockGuard() {
        if (ok) esp_lv_adapter_unlock();
    }
};

// 创建单个 FT 字体实例（常规体，bitmap 模式）。失败返回 nullptr。
const lv_font_t* CreateOne(const char* path, int px, esp_lv_adapter_ft_font_handle_t* out_h) {
    esp_lv_adapter_ft_font_config_t cfg =
        ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(path, static_cast<uint16_t>(px),
                                           ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    esp_lv_adapter_ft_font_handle_t h = nullptr;
    if (esp_lv_adapter_ft_font_init(&cfg, &h) != ESP_OK || h == nullptr) return nullptr;
    const lv_font_t* f = esp_lv_adapter_ft_font_get(h);
    if (f == nullptr) {
        esp_lv_adapter_ft_font_deinit(h);
        return nullptr;
    }
    *out_h = h;
    return f;
}

// 真正激活（已在锁内、已确认要变更）。成功置 s_active=true。
bool DoActivate(const char* filename, int body_px, int head_px, const lv_font_t* body_fb,
                const lv_font_t* head_fb) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", book_store::kBooksDir, filename);

    esp_lv_adapter_ft_font_handle_t bh = nullptr, hh = nullptr;
    const lv_font_t* body = CreateOne(path, body_px, &bh);
    if (body == nullptr) {
        ESP_LOGW(TAG, "activate body failed: %s", path);
        return false;
    }
    const lv_font_t* head = CreateOne(path, head_px, &hh);
    if (head == nullptr) {
        esp_lv_adapter_ft_font_deinit(bh);
        ESP_LOGW(TAG, "activate heading failed: %s", path);
        return false;
    }
    // 缺字兜底：FT 字体没有的字形自动落到对应内置点阵字体（测宽/渲染均跟随 fallback 链）。
    const_cast<lv_font_t*>(body)->fallback = body_fb;
    const_cast<lv_font_t*>(head)->fallback = head_fb;

    s_body_h = bh;
    s_head_h = hh;
    s_body = body;
    s_head = head;
    s_active = true;
    strlcpy(s_cur_file, filename, sizeof(s_cur_file));
    s_cur_body_px = body_px;
    s_cur_head_px = head_px;
    ESP_LOGI(TAG, "activated %s @ %d/%d", filename, body_px, head_px);
    return true;
}

// 把当前激活字体挪进 graveyard，返回新 fence id。调用前须持锁且 s_active。
uint32_t RetireCurrentLocked() {
    uint32_t fence = ++s_fence_counter;
    s_graveyard.push_back({s_body_h, s_head_h, fence});
    s_body_h = s_head_h = nullptr;
    s_body = s_head = nullptr;
    s_active = false;
    s_cur_file[0] = '\0';
    s_cur_body_px = s_cur_head_px = 0;
    return fence;
}
#endif  // EBOOK_FONT_FT

}  // namespace

void ScanFonts(std::vector<FontFile>& out) {
    out.clear();
    if (!SdCardManager::GetInstance().IsMounted()) return;
    DIR* dir = opendir(book_store::kBooksDir);
    if (dir == nullptr) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        size_t ext_len;
        if (!IsFontName(ent->d_name, &ext_len)) continue;
        FontFile f;
        f.filename = ent->d_name;
        f.display = f.filename.substr(0, f.filename.size() - ext_len);
        out.push_back(std::move(f));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(),
              [](const FontFile& a, const FontFile& b) { return a.display < b.display; });
}

bool Exists(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') return false;
    size_t ext_len;
    if (!IsFontName(filename, &ext_len)) return false;
    if (!SdCardManager::GetInstance().IsMounted()) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", book_store::kBooksDir, filename);
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

#if EBOOK_FONT_FT

uint32_t Configure(const char* filename, int body_px, int head_px, const lv_font_t* body_fallback,
                   const lv_font_t* head_fallback) {
    bool want = (filename != nullptr && filename[0] != '\0');
    LockGuard g;
    if (!g.ok) return 0;

    // 已是目标配置：无操作。
    if (want && s_active && strcmp(s_cur_file, filename) == 0 && s_cur_body_px == body_px &&
        s_cur_head_px == head_px) {
        return 0;
    }
    if (!want && !s_active) return 0;

    uint32_t fence = 0;
    if (s_active) fence = RetireCurrentLocked();
    if (want) DoActivate(filename, body_px, head_px, body_fallback, head_fallback);
    return fence;
}

bool Active() { return s_active; }
const lv_font_t* BodyFont() { return s_body; }
const lv_font_t* HeadingFont() { return s_head; }

void OnWorkerFence(uint32_t fence_id) {
    LockGuard g;
    if (!g.ok) return;
    for (auto it = s_graveyard.begin(); it != s_graveyard.end();) {
        if (it->fence <= fence_id) {
            if (it->body) esp_lv_adapter_ft_font_deinit(it->body);
            if (it->head) esp_lv_adapter_ft_font_deinit(it->head);
            it = s_graveyard.erase(it);
        } else {
            ++it;
        }
    }
}

#else  // FreeType 未启用：纯内置字体降级。

uint32_t Configure(const char*, int, int, const lv_font_t*, const lv_font_t*) { return 0; }
bool Active() { return false; }
const lv_font_t* BodyFont() { return nullptr; }
const lv_font_t* HeadingFont() { return nullptr; }
void OnWorkerFence(uint32_t) {}

#endif  // EBOOK_FONT_FT

}  // namespace ebook_font
