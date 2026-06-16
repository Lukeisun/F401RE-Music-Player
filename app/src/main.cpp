#include "gpio.hpp"
#include "sd_card.hpp"
#include "types.hpp"
#include "zephyr/display/cfb.h"
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);
#define SELECT_NODE DT_NODELABEL(select_pin)
#define SCROLL_NODE DT_NODELABEL(scroll_pin)
#define WORKQ_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(workq_stack, WORKQ_STACK_SIZE);
#define MUSIC_WORKQ_STACK_SIZE 8192
K_THREAD_STACK_DEFINE(music_workq_stack, MUSIC_WORKQ_STACK_SIZE);
#define MAX_DISPLAY_LINES 4

// Directory File
// +4 len of disk mount pt +1 for / + 1 for null
static char selected_dir[MAX_FILE_NAME + 4 + 1];
// uint32_t random_in_range(uint32_t min, uint32_t max) {
//   return min + (sys_rand32_get() % (max - min + 1));
// }

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
    // printk("%s\n", long_press ? "long" : "short");
    // if (long_press) {
    //   gpio_ctx->file_count = 0;
    //   gpio_ctx->window_start = 0;
    //   gpio_ctx->state = STATE_DIR;
    //   break;
    // }
    gpio_ctx->state = STATE_TO_PLAY;
    break;
  case STATE_TO_PLAY:
    // if (long_press) {
    //   gpio_ctx->state = STATE_FILE;
    //   break;
    // }
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
  cfb_framebuffer_init(display);
  cfb_framebuffer_invert(display);
  int height = cfb_get_display_parameter(display, CFB_DISPLAY_ROWS);
  int width = cfb_get_display_parameter(display, CFB_DISPLAY_COLS);
  uint8_t font_height, font_width;
  const auto FONT_IDX = 0;
  cfb_framebuffer_set_font(display, FONT_IDX);
  // for (int i = 0; i < cfb_get_numof_fonts(display); i++) {
  //   cfb_get_font_size(display, i, &font_width, &font_height);
  //   printk("Font %d: %dx%d\n", i, font_height, font_width);
  // }
  // printk("Rows: %d, Width: %d", height, width);
  cfb_get_font_size(display, FONT_IDX, &font_width, &font_height);
  const auto max_line_width = (uint8_t)width / font_width;
  static char fname[64];
  fname[max_line_width] = 0;

  while (1) {
    cfb_framebuffer_clear(display, true);
    switch (ctx.state) {
    case STATE_DIR:
      if (sd.dir_count != 0) {
        for (int z = 0; z < MIN(sd.dir_count, 4); z++) {
          auto index = (ctx.window_start + z) % sd.dir_count;
          memcpy(fname, sd.dir_list[index].name, max_line_width);
          cfb_print(display, fname, 0, z * font_height);
          if (index == ctx.dir_cursor) {
            cfb_invert_area(display, 0, z * font_height, width, font_height);
          }
        }
      }
      break;
    case STATE_DIR_READ: {
      sd.lsdir(ctx.dir_cursor);
      ctx.state = STATE_FILE;
      break;
    }
    case STATE_FILE:
      if (sd.file_count != 0) {
        for (int z = 0; z < MIN(sd.file_count, 4); z++) {
          auto index = (ctx.window_start + z) % sd.file_count;
          memcpy(fname, sd.file_list[index].name, max_line_width);
          cfb_print(display, fname, 0, z * font_height);
          if (index == ctx.file_cursor) {
            cfb_invert_area(display, 0, z * font_height, width, font_height);
          }
        }
      }
      break;
    case STATE_TO_PLAY:
      sd.read_file(ctx.dir_cursor, ctx.file_cursor);
      ctx.state = STATE_PLAYING;
      break;
    case STATE_PLAYING:
      cfb_print(display, "Now Playing", 0, 0);
    }
    cfb_framebuffer_finalize(display);
    k_sleep(K_MSEC(1000));
  }
  return 0;
}
