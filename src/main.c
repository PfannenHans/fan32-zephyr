#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>

#define FAN0_MAX_RPM 1800
#define FAN0_MIN_RPM 0
#define FAN1_MAX_RPM 4000
#define FAN1_MIN_RPM 0

#define NUM_FANS (sizeof(fan_channels) / sizeof(fan_control))
#define FAN_TACHO_INT_PER_ROTATION 2
#define MS_TO_M (60 * 1000)
#define FAN_NO_SPIN_TIME (1 * 1000)
#define FAN_POTI_MAX_VOLTAGE 3000  // [mV]

#define FAN_CONTROL_CHANNEL(CHANNEL_NUMBER) \
    { \
        .fan_tacho = GPIO_DT_SPEC_GET(DT_NODELABEL(fan##CHANNEL_NUMBER##_tacho), gpios), \
        .fan_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(fan##CHANNEL_NUMBER##_pwm)), \
        .fan_cb_data = {0}, \
        .fan_poti = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), CHANNEL_NUMBER), \
        .rpm_max = FAN##CHANNEL_NUMBER##_MAX_RPM, \
        .rpm_min = FAN##CHANNEL_NUMBER##_MIN_RPM, \
        .rpm_measured = 0, \
        .rpm_target = 0, \
        .last_int = 0}

typedef struct {
    struct gpio_dt_spec fan_tacho;
    const struct pwm_dt_spec fan_pwm;
    struct gpio_callback fan_cb_data;
    const struct adc_dt_spec fan_poti;
    const uint16_t rpm_max;
    const uint16_t rpm_min;
    uint16_t rpm_measured;
    uint16_t rpm_target;
    uint32_t last_int;
} fan_control;

static fan_control fan_channels[] = {
    FAN_CONTROL_CHANNEL(0),
    FAN_CONTROL_CHANNEL(1)};
// static const struct pwm_dt_spec led = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led));
static const struct device* const temp_onboard = DEVICE_DT_GET(DT_NODELABEL(temperature_onboard));
static struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(led_0), gpios);

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void tacho_callback(const struct device* dev, struct gpio_callback* cb, uint32_t pins);
static bool tacho_init(void);
static bool temp_init(void);
static bool pwm_init(void);
static bool pwm_update(void);
static void check_potis(void);
static void check_no_spin(void);
static uint16_t temp_get(void);

int main(void) {
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    tacho_init();
    pwm_init();
    temp_init();
    while (true) {
        gpio_pin_toggle_dt(&led);
        check_no_spin();
        pwm_update();
        temp_get();
        for (size_t i = 0; i < NUM_FANS; i++) {
            LOG_INF("Fan%u speed: %urpm", i, fan_channels[i].rpm_measured);
        }
        k_msleep(1000);
    }
    return 0;
}

void tacho_callback(__unused const struct device* dev, __unused struct gpio_callback* cb, uint32_t pins) {
    // Unfortunately there is no frequency-counter hardware on the esp32-c3 (in contrast to the og esp32), so using interrupts instead
    for (size_t i = 0; i < NUM_FANS; i++) {
        if (BIT(fan_channels[i].fan_tacho.pin) == pins) {
            uint32_t current_tick = k_cyc_to_ms_near32(k_cycle_get_32());
            fan_channels[i].rpm_measured = (MS_TO_M / FAN_TACHO_INT_PER_ROTATION) / (current_tick - fan_channels[i].last_int);
            fan_channels[i].last_int = current_tick;
            return;
        }
    }
}

bool tacho_init(void) {
    LOG_INF("Initializing fan tacho ISRs...");
    gpio_is_ready_dt(&fan_channels[0].fan_tacho);
    gpio_pin_configure_dt(&fan_channels[0].fan_tacho, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&fan_channels[0].fan_tacho, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&fan_channels[0].fan_cb_data, tacho_callback, BIT(fan_channels[0].fan_tacho.pin));
    gpio_add_callback(fan_channels[0].fan_tacho.port, &fan_channels[0].fan_cb_data);
    gpio_is_ready_dt(&fan_channels[1].fan_tacho);
    gpio_pin_configure_dt(&fan_channels[1].fan_tacho, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&fan_channels[1].fan_tacho, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&fan_channels[1].fan_cb_data, tacho_callback, BIT(fan_channels[1].fan_tacho.pin));
    gpio_add_callback(fan_channels[1].fan_tacho.port, &fan_channels[1].fan_cb_data);
    return true;
}

bool temp_init(void) {
    LOG_INF("Initializing temperature sensors...");
    if (!device_is_ready(temp_onboard)) {
        LOG_INF("Could not initialize onboard temperature sensor!");
        return false;
    }
    return true;
}

uint16_t temp_get(void) {
    struct sensor_value temp;
    int res = sensor_sample_fetch(temp_onboard);
    if (res != 0) {
        LOG_WRN("sample_fetch() failed: %d", res);
        return res;
    }

    res = sensor_channel_get(temp_onboard, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    if (res != 0) {
        LOG_WRN("channel_get() failed: %d", res);
        return res;
    }

    LOG_INF("Temp: %d.%03ddegC", temp.val1, temp.val2);
    return (int16_t)(temp.val1 + temp.val2 / 100000);
}

bool pwm_init(void) {
    for (size_t i = 0; i < NUM_FANS; i++) {
        if (!pwm_is_ready_dt(&fan_channels[i].fan_pwm)) {
            LOG_ERR("Could not initialize FAN%u PWM!", i);
            return false;
        }
    }
    return pwm_update();
}

bool pwm_update(void) {
    check_potis();
    for (size_t i = 0; i < NUM_FANS; i++) {
        uint32_t pwm_pulse_length = (fan_channels[i].fan_pwm.period * fan_channels[i].rpm_target) / fan_channels[i].rpm_max;
        pwm_set_pulse_dt(&fan_channels[i].fan_pwm, pwm_pulse_length);
        LOG_DBG("Set Fan%u PWM pulse length to: %uns", i, pwm_pulse_length);
    }
    return true;
}

void check_potis(void) {
    uint16_t adc_readings[NUM_FANS] = {0};
    const struct adc_sequence_options options = {
        .callback = NULL,
        .extra_samplings = 0,
        .interval_us = 0,
        .user_data = NULL,
    };
    struct adc_sequence sequence = {
        .buffer = &adc_readings[0],
        /* buffer size in bytes, not number of samples */
        .buffer_size = sizeof(adc_readings[0]),
        .resolution = 12,
        .options = &options,
    };
    for (size_t i = 0; i < NUM_FANS; i++) {
        sequence.channels = BIT(fan_channels[i].fan_poti.channel_id);
        sequence.buffer = &adc_readings[i];
        adc_channel_setup_dt(&fan_channels[i].fan_poti);
        adc_read_dt(&fan_channels[0].fan_poti, &sequence);
    }
    for (size_t i = 0; i < NUM_FANS; i++) {
        int32_t tmp = adc_readings[i];
        adc_raw_to_millivolts_dt(&fan_channels[i].fan_poti, &tmp);
        LOG_DBG("Read Poti%u: %imV", i, tmp);
        fan_channels[i].rpm_target = (((uint32_t)fan_channels[i].rpm_max) * tmp) / FAN_POTI_MAX_VOLTAGE;
        if (fan_channels[i].rpm_target < fan_channels[i].rpm_min) {
            fan_channels[i].rpm_target = fan_channels[i].rpm_min;
        }
        LOG_DBG("Set Fan%u target RPM to: %u", i, fan_channels[i].rpm_target);
    }
}

void check_no_spin(void) {
    for (size_t i = 0; i < NUM_FANS; i++) {
        if (fan_channels[i].rpm_measured != 0) {
            uint32_t no_int_time = k_cyc_to_ms_near32(k_cycle_get_32()) - fan_channels[i].last_int;
            if (no_int_time > FAN_NO_SPIN_TIME) {
                LOG_INF("Fan%u: Stopped spinning!", i);
                fan_channels[i].rpm_measured = 0;
            }
        }
    }
}
