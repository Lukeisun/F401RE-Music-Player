#pragma once
#include "sd_card.hpp"
#include <cstdint>
enum DISPLAY_STATE {
  STATE_DIR,
  STATE_FILE,
  STATE_DIR_READ,
  STATE_TO_PLAY,
  STATE_PLAYING
};
typedef struct gpio_ctx_t {
  uint8_t dir_cursor;
  uint8_t file_cursor;
  uint8_t window_start;
  uint8_t *dir_count;
  uint8_t *file_count;
  DISPLAY_STATE state;
  SDCard *sd;

  
} gpio_ctx_t;
