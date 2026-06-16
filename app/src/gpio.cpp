#include "gpio.hpp"
#include "zephyr/drivers/gpio.h"
#include "zephyr/kernel.h"
#include "zephyr/logging/log.h"
#include "zephyr/sys/clock.h"
LOG_MODULE_REGISTER(gpio);
void GPIO::callback(k_work *work) {
  struct k_work_delayable *dwork = k_work_delayable_from_work(work);
  GPIO *io = CONTAINER_OF(dwork, GPIO, work);
  auto active = gpio_pin_get_dt(&io->gpio);
  if (active < 0) {
    LOG_ERR("ERR GPIO\n");
    return;
  }
  io->active = active;
  if (io->cb) {
    io->cb(io->ctx, io->active);
  }
}
void GPIO::isr(const struct device *dev, struct gpio_callback *cb,
               uint32_t pins) {
  GPIO *io = CONTAINER_OF(cb, GPIO, cb_data);
  if (io->work_q) {
    k_work_reschedule_for_queue(io->work_q, &io->work, io->timeout);
  } else {
    k_work_reschedule(&io->work, io->timeout);
  }
}
GPIO::GPIO(const struct gpio_dt_spec &gpio, k_work_q *work_q,
           k_timeout_t timeout, gpio_event_cb_t cb, void *ctx)
    : gpio(gpio), work_q(work_q), timeout(timeout), cb(cb), ctx(ctx) {
   gpio_pin_configure_dt(&gpio, GPIO_INPUT);
  gpio_pin_interrupt_configure_dt(&gpio, (GPIO_INT_EDGE_BOTH));
  gpio_init_callback(&cb_data, &GPIO::isr, BIT(gpio.pin));
  gpio_add_callback(gpio.port, &cb_data);
  k_work_init_delayable(&this->work, &GPIO::callback);
}
