#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

// A simple C wrapper that Rust can call
int host_pin_init() {
    return gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
}

int host_pin_toggle() {
    gpio_pin_toggle_dt(&led);
}

int host_dev_msleep(int ms) {
    int32_t conv_ms = (int32_t)ms;
    int32_t conv_ret = (int32_t)k_msleep(conv_ms);
    return conv_ret
}