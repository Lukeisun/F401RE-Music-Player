#include "mp_lvgl.hpp"
#include "lvgl_zephyr.h"

lv_lock_guard::lv_lock_guard() {
  lvgl_lock();
}
lv_lock_guard::~lv_lock_guard() {
  lvgl_unlock();
}
