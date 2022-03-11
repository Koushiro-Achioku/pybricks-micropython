// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2020 The Pybricks Authors

#include <pbio/config.h>

#if PBIO_CONFIG_DCMOTOR

#include <inttypes.h>

#include <fixmath.h>

#include <pbdrv/config.h>
#include <pbdrv/motor.h>

#include <pbio/battery.h>
#include <pbio/dcmotor.h>

static pbio_dcmotor_t dcmotors[PBDRV_CONFIG_NUM_MOTOR_CONTROLLER];

static pbio_error_t pbio_dcmotor_setup(pbio_dcmotor_t *dcmotor, pbio_direction_t direction, bool is_servo) {

    pbio_error_t err;

    // Configure up motor ports if needed
    err = pbdrv_motor_setup(dcmotor->port, is_servo);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Coast the device
    err = pbio_dcmotor_coast(dcmotor);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Get device ID to ensure we are dealing with a supported device
    err = pbdrv_motor_get_id(dcmotor->port, &dcmotor->id);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Load settings for this motor
    dcmotor->max_voltage = pbio_dcmotor_get_max_voltage(dcmotor->id);

    // Set direction and state
    dcmotor->direction = direction;
    dcmotor->state = PBIO_DCMOTOR_COAST;

    return PBIO_SUCCESS;
}

pbio_error_t pbio_dcmotor_get(pbio_port_id_t port, pbio_dcmotor_t **dcmotor, pbio_direction_t direction, bool is_servo) {
    // Validate port
    if (port < PBDRV_CONFIG_FIRST_MOTOR_PORT || port > PBDRV_CONFIG_LAST_MOTOR_PORT) {
        return PBIO_ERROR_INVALID_PORT;
    }

    // Get pointer to dcmotor
    *dcmotor = &dcmotors[port - PBDRV_CONFIG_FIRST_MOTOR_PORT];
    (*dcmotor)->port = port;

    // Initialize and set up pwm properties
    return pbio_dcmotor_setup(*dcmotor, direction, is_servo);
}

pbio_error_t pbio_dcmotor_get_state(pbio_dcmotor_t *dcmotor, pbio_passivity_t *state, int32_t *voltage_now) {
    *state = dcmotor->state;
    *voltage_now = dcmotor->state < PBIO_DCMOTOR_DUTY_PASSIVE ? 0 : dcmotor->voltage_now;
    return PBIO_SUCCESS;
}

pbio_error_t pbio_dcmotor_coast(pbio_dcmotor_t *dcmotor) {
    dcmotor->state = PBIO_DCMOTOR_COAST;
    return pbdrv_motor_coast(dcmotor->port);
}

pbio_error_t pbio_dcmotor_brake(pbio_dcmotor_t *dcmotor) {
    dcmotor->state = PBIO_DCMOTOR_BRAKE;
    return pbdrv_motor_set_duty_cycle(dcmotor->port, 0);
}

pbio_error_t pbio_dcmotor_set_voltage(pbio_dcmotor_t *dcmotor, int32_t voltage) {

    // Cap voltage at the configured limit.
    if (voltage > dcmotor->max_voltage) {
        voltage = dcmotor->max_voltage;
    } else if (voltage < -dcmotor->max_voltage) {
        voltage = -dcmotor->max_voltage;
    }

    // Cache value so we can read it back without touching hardware again.
    dcmotor->voltage_now = voltage;

    // Convert voltage to duty cycle.
    int32_t duty_cycle = pbio_battery_get_duty_from_voltage(voltage);

    // Flip sign if motor is inverted.
    if (dcmotor->direction == PBIO_DIRECTION_COUNTERCLOCKWISE) {
        duty_cycle = -duty_cycle;
    }

    // Apply the duty cycle.
    pbio_error_t err = pbdrv_motor_set_duty_cycle(dcmotor->port, duty_cycle);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Set status for this motor.
    dcmotor->state = PBIO_DCMOTOR_CLAIMED;
    return PBIO_SUCCESS;
}

pbio_error_t pbio_dcmotor_set_voltage_passive(pbio_dcmotor_t *dcmotor, int32_t voltage) {
    // Call voltage setter that is also used for system purposes.
    pbio_error_t err = pbio_dcmotor_set_voltage(dcmotor, voltage);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Set state to passive since the user controls it now.
    dcmotor->state = PBIO_DCMOTOR_DUTY_PASSIVE;
    return PBIO_SUCCESS;
}

void pbio_dcmotor_get_settings(pbio_dcmotor_t *dcmotor, int32_t *max_voltage) {
    *max_voltage = dcmotor->max_voltage;
}

pbio_error_t pbio_dcmotor_set_settings(pbio_dcmotor_t *dcmotor, int32_t max_voltage) {
    // New maximum voltage must be positive and at or below hardware limit.
    if (max_voltage < 0 || max_voltage > pbio_dcmotor_get_max_voltage(dcmotor->id)) {
        return PBIO_ERROR_INVALID_ARG;
    }
    // Set the new value.
    dcmotor->max_voltage = max_voltage;
    return PBIO_SUCCESS;
}

#endif // PBIO_CONFIG_DCMOTOR
