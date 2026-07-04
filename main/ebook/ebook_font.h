// ebook_font.h
// 用户 FreeType 字体管理：从 /sdcard/books/ 扫描 TTF/OTF，创建/切换 lv_freetype 字体
// （正文 + 标题两个尺寸实例），并以 fence + graveyard 机制安全释放旧字体。
//
// 线程模型：Configure/Retire/字体创建都在 LVGL 线程调用（事件回调，已持递归适配器锁）；
// OnWorkerFence 在 ebook_worker 线程持锁回调。测宽本身线程安全 —— lv_freetype 对每个 FT_Face
// 有内部 face_lock，串行化度量(worker)与位图渲染(draw 线程)，故 paginator 可像内置字体一样
// 直接 lv_font_get_glyph_width(ft_font)，无需外层缓存或加锁。
//
// ⚠️ 生命周期铁律：正被 worker 测宽 / 被 lv_label 引用的 FT 字体不可释放。切字体/字号时把旧
// 字体挪进 graveyard 并记 fence id；宿主在"重排版 LOAD 命令之后"入队同 id 的 worker fence，
// 待 worker FIFO 排空到该 fence（旧字体的测宽命令全部完成、且新结果已 remount 换掉旧 label）
// 才真正 deinit。见 ebook_screen 的 ApplyFontSettings。
//
// bitmap 渲染模式下 lv_freetype 不合成粗/斜体（真·粗斜体需 OUTLINE + vector graphics，flash
// 预算不允许），故这里只按尺寸建常规体；EPUB 强调样式由 reader_view 以合成方式呈现。

#ifndef EBOOK_FONT_H
#define EBOOK_FONT_H

#include "lvgl.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ebook_font {

// 一个可选字体文件。display = 去扩展名的文件名（供菜单展示）。
struct FontFile {
    std::string filename;  // 带扩展名，如 "霞鹜文楷.ttf"
    std::string display;   // 去扩展名，如 "霞鹜文楷"
};

// 扫描 /sdcard/books 下 *.ttf/*.otf（按 display 升序）。SD 未挂载 → 空。cheap（一次 opendir）。
void ScanFonts(std::vector<FontFile>& out);

// 该文件名是否存在于 /sdcard/books 且后缀为 ttf/otf。
bool Exists(const char* filename);

// 应用目标字体配置（正文 body_px + 标题 head_px），fallback 为对应内置点阵字体（缺字兜底）。
//   - filename 空 / FreeType 未启用 / 与当前已激活配置相同：不动，返回 0。
//   - 配置变化：旧 FT 字体（若有）挪进 graveyard 并分配一个 fence id，随后尝试激活新字体
//     （filename 非空时）；无论新字体成败都返回该 fence id（>0 表示需要宿主入队 worker fence
//     来释放旧字体）。首次激活（无旧字体）返回 0。
// 激活失败（文件缺失/解析失败）→ Active() 保持 false，宿主据此走内置字体。
uint32_t Configure(const char* filename, int body_px, int head_px, const lv_font_t* body_fallback,
                   const lv_font_t* head_fallback);

// 当前是否有激活的 FT 字体（供 BuildMetrics 判定走 FT 还是内置路径）。
bool Active();
const lv_font_t* BodyFont();     // Active() 为真时非空
const lv_font_t* HeadingFont();  // Active() 为真时非空

// worker 线程（持锁）回调：释放 graveyard 中 fence 标记 <= fence_id 的旧字体。
void OnWorkerFence(uint32_t fence_id);

}  // namespace ebook_font

#endif  // EBOOK_FONT_H
