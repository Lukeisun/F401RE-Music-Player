#include "gpio.hpp"
#include "mp_lvgl.hpp"
#include "sd_card.hpp"
#include "types.hpp"
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#include <lvgl.h>
#include <lvgl_zephyr.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);
#define SELECT_NODE DT_NODELABEL(select_pin)
#define SCROLL_NODE DT_NODELABEL(scroll_pin)
#define WORKQ_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(workq_stack, WORKQ_STACK_SIZE);
#define MUSIC_WORKQ_STACK_SIZE 8192
K_THREAD_STACK_DEFINE(music_workq_stack, MUSIC_WORKQ_STACK_SIZE);
#define MAX_DISPLAY_LINES 8

// Directory File
// +4 len of disk mount pt +1 for / + 1 for null
static char selected_dir[MAX_FILE_NAME + 4 + 1];

void select_cb(bool long_press, void *ctx) {
  printk("select\n");
  int64_t press_start = k_uptime_get();
  static int64_t last_press = 0;
  auto gpio_ctx = static_cast<gpio_ctx_t *>(ctx);
  auto diff = (last_press - press_start);
  printk("%lldd\n", diff);
  switch (gpio_ctx->state) {
  case STATE_DIR:
    gpio_ctx->file_cursor = 0;
    gpio_ctx->window_start = 0;
    gpio_ctx->state = STATE_DIR_READ;
    break;
  case STATE_FILE:
    gpio_ctx->state = STATE_TO_PLAY;
    break;
  case STATE_TO_PLAY:
    break;
  default:
    break;
  }
  last_press = press_start;
}
void scroll_cb(bool long_press, void *ctx) {
  printk("scroll\n");
  auto gpio_ctx = static_cast<gpio_ctx_t *>(ctx);
  switch (gpio_ctx->state) {
  case STATE_DIR: {
    gpio_ctx->dir_cursor += 1;
    auto dir_count = *gpio_ctx->dir_count;
    auto dist = (int)(gpio_ctx->dir_cursor - gpio_ctx->window_start);
    gpio_ctx->dir_cursor %= dir_count;
    if (dist >= MAX_DISPLAY_LINES) {
      gpio_ctx->window_start += gpio_ctx->dir_cursor - (MAX_DISPLAY_LINES - 1);
    }
    if (dist < 0) {
      gpio_ctx->window_start = 0;
    }
    break;
  }
  case STATE_FILE: {
    gpio_ctx->file_cursor += 1;
    auto file_count = *gpio_ctx->file_count;
    auto dist = (int)(gpio_ctx->file_cursor - gpio_ctx->window_start);
    gpio_ctx->file_cursor %= file_count;
    if (dist >= MAX_DISPLAY_LINES) {
      gpio_ctx->window_start += gpio_ctx->file_cursor - (MAX_DISPLAY_LINES - 1);
    }
    if (dist < 0) {
      gpio_ctx->window_start = 0;
    }
    break;
  }
  case STATE_TO_PLAY:
    break;
  default:
    break;
  }
}
static const struct device *const longpress_dev =
    DEVICE_DT_GET(DT_PATH(longpress));
static void longpress_cb(struct input_event *evt, void *user_data) {
  if (evt->sync == 0) {
    return;
  }
  bool long_press = false;
  if (0 != evt->value) {
    switch (evt->code) {
    case INPUT_KEY_A:
    case INPUT_KEY_B:
      long_press = false;
      break;
    case INPUT_KEY_X:
    case INPUT_KEY_Y:
      long_press = true;
      break;
    default:
      return;
    }
    switch (evt->code) {
    case INPUT_KEY_A:
    case INPUT_KEY_X:
      select_cb(long_press, user_data);
      break;
    case INPUT_KEY_B:
    case INPUT_KEY_Y:
      scroll_cb(long_press, user_data);
      break;
    default:
      return;
    }
  }
}
int main(void) {
  struct k_work_q workq;
  k_work_queue_init(&workq);
  k_work_queue_start(&workq, workq_stack, K_THREAD_STACK_SIZEOF(workq_stack),
                     K_PRIO_PREEMPT(10),
                     NULL); // priority 10, lower than main
  struct k_work_q music_workq;
  k_work_queue_init(&music_workq);
  k_work_queue_start(&music_workq, music_workq_stack,
                     K_THREAD_STACK_SIZEOF(music_workq_stack),
                     K_PRIO_PREEMPT(7), // high priority, cooperative
                     NULL);
  const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
  printk("init\n");
  SDCard sd{&workq, &music_workq};
  static struct gpio_ctx_t ctx{};
  INPUT_CALLBACK_DEFINE(longpress_dev, longpress_cb, &ctx);
  ctx = {0, 0, 0, &sd.dir_count, &sd.file_count, STATE_DIR, &sd};
  if (!device_is_ready(display)) {
    printk("Display not ready\n");
    return 0;
  }
  display_blanking_off(display);

  // --- LVGL UI setup ---
  static lv_obj_t *list_labels[MAX_DISPLAY_LINES];
  static lv_obj_t *now_playing_label;
  static lv_obj_t *total_duration_label;
  static char duration_str[9];
  static lv_obj_t *total_elapsed_label;
  static char elapsed_str[9];
  static lv_obj_t *progress_container;
  static lv_obj_t *progress_bar;
  static lv_style_t style_normal;
  static lv_style_t style_highlight;

  {
    lv_lock_guard guard;

    // Styles
    lv_style_init(&style_normal);
    lv_style_set_text_color(&style_normal, lv_color_white());
    lv_style_set_bg_opa(&style_normal, LV_OPA_TRANSP);
    lv_style_set_pad_ver(&style_normal, 4);
    lv_style_set_pad_hor(&style_normal, 4);

    lv_style_init(&style_highlight);
    lv_style_set_text_color(&style_highlight, lv_color_black());
    lv_style_set_bg_color(&style_highlight, lv_color_white());
    lv_style_set_bg_opa(&style_highlight, LV_OPA_COVER);
    lv_style_set_pad_ver(&style_highlight, 4);
    lv_style_set_pad_hor(&style_highlight, 4);

    // Black background
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    // Create n list labels
    for (int i = 0; i < MAX_DISPLAY_LINES; i++) {
      list_labels[i] = lv_label_create(lv_screen_active());
      lv_label_set_text(list_labels[i], "");
      lv_obj_set_width(list_labels[i], 240);
      lv_obj_set_pos(list_labels[i], 0, i * 40);
      lv_obj_add_style(list_labels[i], &style_normal, 0);
      lv_label_set_long_mode(list_labels[i], LV_LABEL_LONG_CLIP);
    }

    // Now playing label (hidden initially)
    now_playing_label = lv_label_create(lv_screen_active());
    lv_label_set_text(now_playing_label, "Now Playing");
    lv_obj_align(now_playing_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_style(now_playing_label, &style_normal, 0);
    lv_obj_add_flag(now_playing_label, LV_OBJ_FLAG_HIDDEN);

    // Progress container: elapsed | bar | total, aligned to bottom
    progress_container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(progress_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(progress_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(progress_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(progress_container, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(progress_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_container, 0, 0);
    lv_obj_set_style_pad_all(progress_container, 2, 0);
    lv_obj_add_flag(progress_container, LV_OBJ_FLAG_HIDDEN);

    // Measure max time label width dynamically from font metrics
    lv_point_t time_size;
    lv_text_get_size(&time_size, "00:00:00", lv_font_get_default(), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t time_label_width = time_size.x + 8; // +8 for pad_hor (4px each side)

    // Elapsed time label (inside container)
    total_elapsed_label = lv_label_create(progress_container);
    lv_label_set_text_static(total_elapsed_label, elapsed_str);
    lv_obj_add_style(total_elapsed_label, &style_normal, 0);
    lv_obj_set_width(total_elapsed_label, time_label_width);

    // Progress bar (inside container, fills remaining space)
    progress_bar = lv_bar_create(progress_container);
    lv_obj_set_flex_grow(progress_bar, 1);
    lv_obj_set_height(progress_bar, 8);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, lv_color_make(40, 40, 40), 0);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(),
                              LV_PART_INDICATOR);

    // Total duration label (inside container)
    total_duration_label = lv_label_create(progress_container);
    lv_label_set_text_static(total_duration_label, duration_str);
    lv_obj_add_style(total_duration_label, &style_normal, 0);
    lv_obj_set_width(total_duration_label, time_label_width);

    lv_timer_handler();
  }

  // --- Main loop ---
  DISPLAY_STATE last_state = STATE_DIR;
  uint8_t last_dir_cursor = 0xFF;
  uint8_t last_file_cursor = 0xFF;
  uint8_t last_window_start = 0xFF;

  while (1) {
    {
      lv_lock_guard guard;

      switch (ctx.state) {
      case STATE_DIR:
        if (ctx.state != last_state || ctx.dir_cursor != last_dir_cursor ||
            ctx.window_start != last_window_start) {
          // Show list labels, hide now playing
          for (int i = 0; i < MAX_DISPLAY_LINES; i++) {
            lv_obj_clear_flag(list_labels[i], LV_OBJ_FLAG_HIDDEN);
          }
          lv_obj_add_flag(now_playing_label, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(progress_container, LV_OBJ_FLAG_HIDDEN);

          for (int z = 0; z < MAX_DISPLAY_LINES; z++) {
            if (z < sd.dir_count) {
              auto index = (ctx.window_start + z) % sd.dir_count;
              lv_label_set_text(list_labels[z], sd.dir_list[index].name);
              if (index == ctx.dir_cursor) {
                lv_obj_remove_style(list_labels[z], &style_normal, 0);
                lv_obj_add_style(list_labels[z], &style_highlight, 0);
              } else {
                lv_obj_remove_style(list_labels[z], &style_highlight, 0);
                lv_obj_add_style(list_labels[z], &style_normal, 0);
              }
            } else {
              lv_label_set_text(list_labels[z], "");
              lv_obj_remove_style(list_labels[z], &style_highlight, 0);
              lv_obj_add_style(list_labels[z], &style_normal, 0);
            }
          }
          last_dir_cursor = ctx.dir_cursor;
          last_window_start = ctx.window_start;
        }
        break;

      case STATE_DIR_READ:
        sd.lsdir(ctx.dir_cursor);
        ctx.state = STATE_FILE;
        last_file_cursor = 0xFF; // force redraw
        break;

      case STATE_FILE:
        if (ctx.state != last_state || ctx.file_cursor != last_file_cursor ||
            ctx.window_start != last_window_start) {
          for (int i = 0; i < MAX_DISPLAY_LINES; i++) {
            lv_obj_clear_flag(list_labels[i], LV_OBJ_FLAG_HIDDEN);
          }
          lv_obj_add_flag(now_playing_label, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(progress_container, LV_OBJ_FLAG_HIDDEN);

          for (int z = 0; z < MAX_DISPLAY_LINES; z++) {
            if (z < sd.file_count) {
              auto index = (ctx.window_start + z) % sd.file_count;
              lv_label_set_text(list_labels[z], sd.file_list[index].name);
              if (index == ctx.file_cursor) {
                lv_obj_remove_style(list_labels[z], &style_normal, 0);
                lv_obj_add_style(list_labels[z], &style_highlight, 0);
              } else {
                lv_obj_remove_style(list_labels[z], &style_highlight, 0);
                lv_obj_add_style(list_labels[z], &style_normal, 0);
              }
            } else {
              lv_label_set_text(list_labels[z], "");
              lv_obj_remove_style(list_labels[z], &style_highlight, 0);
              lv_obj_add_style(list_labels[z], &style_normal, 0);
            }
          }
          last_file_cursor = ctx.file_cursor;
          last_window_start = ctx.window_start;
        }
        break;

      case STATE_TO_PLAY:
        sd.read_file(ctx.dir_cursor, ctx.file_cursor);
        ctx.state = STATE_PLAYING;
      case STATE_PLAYING:
        if (ctx.state != last_state) {
          // Hide list, show now playing
          for (int i = 0; i < MAX_DISPLAY_LINES; i++) {
            lv_obj_add_flag(list_labels[i], LV_OBJ_FLAG_HIDDEN);
          }
          k_sem_take(&sd.header_ready, K_FOREVER);
          sd.time_str(duration_str, atomic_get(&sd.total_time));
          lv_label_set_text_static(total_duration_label, duration_str);
          lv_obj_clear_flag(now_playing_label, LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(progress_container, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(now_playing_label,
                            sd.file_list[ctx.file_cursor].name);
        }
        {
          uint32_t elapsed = atomic_get(&sd.elapsed_time);
          uint32_t total = atomic_get(&sd.total_time);
          sd.time_str(elapsed_str, elapsed);
          lv_label_set_text_static(total_elapsed_label, elapsed_str);
          if (total > 0) {
            int pct = (int)((elapsed * 100) / total);
            if (pct > 100)
              pct = 100;
            lv_bar_set_value(progress_bar, pct, LV_ANIM_OFF);
          }
        }
        break;
      }

      last_state = ctx.state;
      lv_timer_handler();
    }
    k_msleep(100);
  }
  return 0;
}
