#pragma once
#include "zephyr/drivers/gpio.h"
#include "zephyr/kernel.h"
#include "zephyr/sys/clock.h"
using gpio_event_cb_t = void (*)(void *context, bool active);
class GPIO {
private:
  struct gpio_callback cb_data;
  const struct gpio_dt_spec &gpio;
  struct k_work_q *work_q = nullptr;
  k_timeout_t timeout = K_MSEC(25);
  bool active = false;
  static void callback(k_work *work);
  static void isr(const struct device *dev, struct gpio_callback *cb,
                  uint32_t pins);
  gpio_event_cb_t cb = nullptr;
  void *ctx = nullptr;

public:
  struct k_work_delayable work;
  GPIO(const struct gpio_dt_spec &gpio, k_work_q *work_q, k_timeout_t timeout,
       gpio_event_cb_t cb, void *ctx);
};
