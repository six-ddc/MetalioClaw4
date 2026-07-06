#include "camera_test.h"

#include "camera_screen/camera_screen.h"
#include "esp_log.h"
#include "screen_util.h"
#include "test_ui_common.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "CameraTest";

constexpr int kPreviewW = 720;
constexpr int kPreviewH = 600;
constexpr int kBottomH  = 120;

lv_obj_t* s_status_icon     = nullptr;
lv_obj_t* s_preview_mask    = nullptr;
lv_obj_t* s_preview_canvas  = nullptr;

void ClosePreviewDialog() {
    if (s_preview_mask == nullptr) {
        return;
    }
    CameraScreen::StopExternalPreview();
    lv_obj_delete(s_preview_mask);
    s_preview_mask = nullptr;
    s_preview_canvas = nullptr;
    ESP_LOGI(TAG, "preview dialog closed");
}

void OnPreviewConfirm(bool pass) {
    TestUiUpdateStatus(s_status_icon, pass);
    ESP_LOGI(TAG, "user confirm camera preview: %s", pass ? "pass" : "fail");
    ClosePreviewDialog();
}

void OnCloseClicked(lv_event_t* /*e*/) {
    ClosePreviewDialog();
}

void OnNoClicked(lv_event_t* /*e*/) {
    OnPreviewConfirm(false);
}

void OnYesClicked(lv_event_t* /*e*/) {
    OnPreviewConfirm(true);
}

lv_obj_t* CreateBottomButton(lv_obj_t* parent, const char* text,
                             uint32_t bg_color, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 140, 56);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(btn, true);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void OpenPreviewDialog() {
    if (s_preview_mask != nullptr) {
        return;
    }

    lv_obj_t* parent = TestUiGetScreen();
    if (parent == nullptr) {
        ESP_LOGW(TAG, "test screen not ready");
        return;
    }

    CameraScreen::PreviewBuffer preview_buf = {};
    if (!CameraScreen::PreparePreviewBuffer(&preview_buf)) {
        ESP_LOGE(TAG, "prepare preview buffer failed");
        return;
    }

    lv_obj_t* mask = lv_obj_create(parent);
    s_preview_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);

    lv_obj_t* preview_panel = lv_obj_create(mask);
    screen_strip_obj_chrome(preview_panel);
    lv_obj_set_size(preview_panel, kPreviewW, kPreviewH);
    lv_obj_set_pos(preview_panel, 0, 0);
    lv_obj_set_style_bg_color(preview_panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_remove_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(preview_panel);

    lv_obj_t* canvas = lv_canvas_create(preview_panel);
    s_preview_canvas = canvas;
    lv_canvas_set_buffer(canvas, preview_buf.data, preview_buf.width,
                         preview_buf.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, kPreviewW, kPreviewH);
    screen_make_input_passive(canvas);

    lv_obj_t* bottom = lv_obj_create(mask);
    screen_strip_obj_chrome(bottom);
    lv_obj_set_size(bottom, kTestPanelW, kBottomH);
    lv_obj_set_pos(bottom, 0, kPreviewH);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(kTestColorCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bottom, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bottom, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(bottom, 8, LV_PART_MAIN);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(bottom, true);

    lv_obj_t* question = lv_label_create(bottom);
    lv_label_set_text(question, "画面是否正常？");
    lv_obj_set_style_text_color(question, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(question, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_remove_flag(question, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn_row = lv_obj_create(bottom);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_size(btn_row, kTestPanelW - 32, 56);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    CreateBottomButton(btn_row, "否", kTestColorError, OnNoClicked);
    CreateBottomButton(btn_row, "是", kTestColorHigh, OnYesClicked);
    CreateBottomButton(btn_row, "关闭", kTestColorMuted, OnCloseClicked);

    if (CameraScreen::StartExternalPreview(canvas) != ESP_OK) {
        ESP_LOGE(TAG, "start external preview failed");
        ClosePreviewDialog();
        return;
    }

    ESP_LOGI(TAG, "preview dialog opened");
}

void OnRowClicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    OpenPreviewDialog();
}

}  // namespace

namespace CameraTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    lv_obj_t* row = TestUiCreateRowShell(list, "摄像头", &s_status_icon, &ctrl);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(row, true);
    screen_make_input_passive(ctrl);

    lv_obj_t* hint = lv_label_create(ctrl);
    lv_label_set_text(hint, "点击预览");
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
}

void OnLoad() {}

void OnUnload() {
    ClosePreviewDialog();
    s_status_icon = nullptr;
}

}  // namespace CameraTest
