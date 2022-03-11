// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2020 The Pybricks Authors

#include <stdlib.h>

#include <pbdrv/clock.h>
#include <pbio/error.h>
#include <pbio/drivebase.h>
#include <pbio/math.h>
#include <pbio/servo.h>

#if PBDRV_CONFIG_NUM_MOTOR_CONTROLLER != 0

static pbio_error_t drivebase_adopt_settings(pbio_control_settings_t *s_distance, pbio_control_settings_t *s_heading, pbio_control_settings_t *s_left, pbio_control_settings_t *s_right) {

    // All rate/count acceleration limits add up, because distance state is two motors counts added
    s_distance->max_rate = s_left->max_rate + s_right->max_rate;
    s_distance->rate_tolerance = s_left->rate_tolerance + s_right->rate_tolerance;
    s_distance->count_tolerance = s_left->count_tolerance + s_right->count_tolerance;
    s_distance->stall_rate_limit = s_left->stall_rate_limit + s_right->stall_rate_limit;
    s_distance->integral_rate = s_left->integral_rate + s_right->integral_rate;
    s_distance->abs_acceleration = s_left->abs_acceleration + s_right->abs_acceleration;

    // Use the average PID of both motors
    s_distance->pid_kp = (s_left->pid_kp + s_right->pid_kp) / 2;
    s_distance->pid_ki = (s_left->pid_ki + s_right->pid_ki) / 2;
    s_distance->pid_kd = (s_left->pid_kd + s_right->pid_kd) / 2;

    // Maxima are bound by the least capable motor
    s_distance->max_torque = min(s_left->max_torque, s_right->max_torque);
    s_distance->stall_time = min(s_left->stall_time, s_right->stall_time);

    // Copy rate estimator usage, required to be the same on both motors
    if (s_left->use_estimated_rate != s_right->use_estimated_rate || s_left->use_estimated_count != s_right->use_estimated_count) {
        return PBIO_ERROR_INVALID_ARG;
    }
    s_distance->use_estimated_rate = s_left->use_estimated_rate;

    // Use the reported count for drive bases.
    s_distance->use_estimated_count = false;

    // By default, heading control is the same as distance control
    *s_heading = *s_distance;

    // Allow just slightly more torque for heading. While not technically
    // necessary under nominal circumstances, it gives the expected perceived
    // result of one wheel nearly stopping when you block the other.
    s_heading->max_torque *= 2;

    return PBIO_SUCCESS;
}

// Get the physical and estimated state of a drivebase
static pbio_error_t pbio_drivebase_get_state(pbio_drivebase_t *db, pbio_control_state_t *state_distance, pbio_control_state_t *state_heading) {

    // Get left servo state
    pbio_control_state_t state_left;
    pbio_error_t err = pbio_servo_get_state(db->left, &state_left);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Get right servo state
    pbio_control_state_t state_right;
    err = pbio_servo_get_state(db->right, &state_right);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Take sum to get distance state
    state_distance->count = state_left.count + state_right.count;
    state_distance->rate = state_left.rate + state_right.rate;

    state_distance->count_est = state_left.count_est + state_right.count_est;
    state_distance->rate_est = state_left.rate_est + state_right.rate_est;

    // Take difference to get heading state
    state_heading->count = state_left.count - state_right.count;
    state_heading->rate = state_left.rate - state_right.rate;

    state_heading->count_est = state_left.count_est - state_right.count_est;
    state_heading->rate_est = state_left.rate_est - state_right.rate_est;

    return PBIO_SUCCESS;
}

// Actuate a drivebase
static pbio_error_t pbio_drivebase_actuate(pbio_drivebase_t *db, pbio_actuation_t actuation, int32_t sum_control, int32_t dif_control) {

    switch (actuation) {
        // Coast and brake are both passed on to servo_actuate as-is.
        case PBIO_ACTUATION_COAST:
        case PBIO_ACTUATION_BRAKE: {
            pbio_drivebase_stop_control(db);
            pbio_drivebase_claim_servos(db, false);
            pbio_error_t err = pbio_servo_actuate(db->left, actuation, 0);
            if (err != PBIO_SUCCESS) {
                return err;
            }
            return pbio_servo_actuate(db->right, actuation, 0);
        }
        // Hold is achieved by driving 0 distance.
        case PBIO_ACTUATION_HOLD:
            return pbio_drivebase_drive_curve(db, 0, 0, db->control_distance.settings.max_rate, db->control_heading.settings.max_rate, PBIO_ACTUATION_HOLD);
        case PBIO_ACTUATION_VOLTAGE:
            return PBIO_ERROR_NOT_IMPLEMENTED;
        case PBIO_ACTUATION_TORQUE:
            return PBIO_ERROR_NOT_IMPLEMENTED;
        default:
            return PBIO_ERROR_INVALID_ARG;
    }
}

pbio_error_t pbio_drivebase_setup(pbio_drivebase_t *db, pbio_servo_t *left, pbio_servo_t *right, fix16_t wheel_diameter, fix16_t axle_track) {
    pbio_error_t err;

    // Attach servos
    db->left = left;
    db->right = right;

    // Stop any existing drivebase controls
    pbio_drivebase_stop_control(db);

    // Drivebase geometry
    if (wheel_diameter <= 0 || axle_track <= 0) {
        return PBIO_ERROR_INVALID_ARG;
    }

    // Assert that both motors have the same gearing
    if (left->control.settings.counts_per_unit != right->control.settings.counts_per_unit) {
        return PBIO_ERROR_INVALID_ARG;
    }

    // Reset both motors to a passive state
    err = pbio_drivebase_actuate(db, PBIO_ACTUATION_COAST, 0, 0);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Adopt settings as the average or sum of both servos, except scaling
    err = drivebase_adopt_settings(&db->control_distance.settings, &db->control_heading.settings, &db->left->control.settings, &db->right->control.settings);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Count difference between the motors for every 1 degree drivebase rotation
    db->control_heading.settings.counts_per_unit =
        fix16_mul(
            left->control.settings.counts_per_unit,
            fix16_div(
                fix16_mul(
                    axle_track,
                    fix16_from_int(2)
                    ),
                wheel_diameter
                )
            );

    // Sum of motor counts for every 1 mm forward
    db->control_distance.settings.counts_per_unit =
        fix16_mul(
            left->control.settings.counts_per_unit,
            fix16_div(
                fix16_mul(
                    fix16_from_int(180),
                    FOUR_DIV_PI
                    ),
                wheel_diameter
                )
            );

    return PBIO_SUCCESS;
}

// Claim servos so that they cannot be used independently
void pbio_drivebase_claim_servos(pbio_drivebase_t *db, bool claim) {
    // Stop control
    pbio_control_stop(&db->left->control);
    pbio_control_stop(&db->right->control);
    // Set claim status
    db->left->claimed = claim;
    db->right->claimed = claim;
}

pbio_error_t pbio_drivebase_stop(pbio_drivebase_t *db, pbio_actuation_t after_stop) {

    if (after_stop == PBIO_ACTUATION_HOLD) {

        // Get drive base state
        pbio_control_state_t state_distance;
        pbio_control_state_t state_heading;
        pbio_error_t err = pbio_drivebase_get_state(db, &state_distance, &state_heading);
        if (err != PBIO_SUCCESS) {
            return err;
        }

        // When holding, the control payload is the count to hold
        return pbio_drivebase_actuate(db, after_stop, state_distance.count, state_heading.count);

    } else {
        // Otherwise the payload is zero and control stops
        return pbio_drivebase_actuate(db, after_stop, 0, 0);
    }
}

void pbio_drivebase_stop_control(pbio_drivebase_t *db) {
    // Stop control so polling will stop
    pbio_control_stop(&db->control_distance);
    pbio_control_stop(&db->control_heading);
}

bool pbio_drivebase_is_busy(pbio_drivebase_t *db) {
    return !pbio_control_is_done(&db->control_distance) || !pbio_control_is_done(&db->control_heading);
}

pbio_error_t pbio_drivebase_update(pbio_drivebase_t *db) {

    // If passive, then exit
    if (db->control_heading.type == PBIO_CONTROL_NONE || db->control_distance.type == PBIO_CONTROL_NONE) {
        return PBIO_SUCCESS;
    }

    // Get current time
    int32_t time_now = pbdrv_clock_get_us();

    // Get drive base state
    pbio_control_state_t state_distance;
    pbio_control_state_t state_heading;
    pbio_error_t err = pbio_drivebase_get_state(db, &state_distance, &state_heading);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Get reference and torque signals
    pbio_trajectory_reference_t ref_distance;
    pbio_trajectory_reference_t ref_heading;
    int32_t sum_torque, dif_torque;
    pbio_actuation_t sum_actuation, dif_actuation;
    pbio_control_update(&db->control_distance, time_now, &state_distance, &ref_distance, &sum_actuation, &sum_torque);
    pbio_control_update(&db->control_heading, time_now, &state_heading, &ref_heading, &dif_actuation, &dif_torque);

    // If either controller coasts, coast both, thereby also stopping control.
    if (sum_actuation == PBIO_ACTUATION_COAST || dif_actuation == PBIO_ACTUATION_COAST) {
        return pbio_drivebase_actuate(db, PBIO_ACTUATION_COAST, 0, 0);
    }
    // If either controller brakes, brake both, thereby also stopping control.
    if (sum_actuation == PBIO_ACTUATION_BRAKE || dif_actuation == PBIO_ACTUATION_BRAKE) {
        return pbio_drivebase_actuate(db, PBIO_ACTUATION_BRAKE, 0, 0);
    }

    // The leading controller is able to pause when it stalls. The following controller does not do its own stall,
    // but follows the leader. This ensures they complete at exactly the same time.

    // Check which controller is the follower, if any.
    if (pbio_control_type_is_follower(&db->control_distance)) {
        // Distance control follows, so make it copy heading control pause state
        err = pbio_control_copy_integrator_pause_state(&db->control_heading, &db->control_distance, time_now, state_distance.count, ref_distance.count);
    } else if (pbio_control_type_is_follower(&db->control_heading)) {
        // Heading control follows, so make it copy distance control pause state
        err = pbio_control_copy_integrator_pause_state(&db->control_distance, &db->control_heading, time_now, state_heading.count, ref_heading.count);
    }
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // The left servo drives at a torque and speed of sum / 2 + dif / 2
    int32_t feed_forward_left = pbio_observer_get_feedforward_torque(&db->left->observer, ref_distance.rate / 2 + ref_heading.rate / 2, ref_distance.acceleration / 2 + ref_heading.acceleration / 2);
    err = pbio_servo_actuate(db->left, sum_actuation, sum_torque / 2 + dif_torque / 2 + feed_forward_left);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // The right servo drives at a torque and speed of sum / 2 - dif / 2
    int32_t feed_forward_right = pbio_observer_get_feedforward_torque(&db->right->observer, ref_distance.rate / 2 - ref_heading.rate / 2, ref_distance.acceleration / 2 - ref_heading.acceleration / 2);
    return pbio_servo_actuate(db->right, dif_actuation, sum_torque / 2 - dif_torque / 2 + feed_forward_right);
}

static pbio_error_t pbio_drivebase_drive_counts_relative(pbio_drivebase_t *db, int32_t sum, int32_t sum_rate, int32_t dif, int32_t dif_rate, pbio_actuation_t after_stop) {

    // Claim both servos for use by drivebase
    pbio_drivebase_claim_servos(db, true);

    // Get current time
    int32_t time_now = pbdrv_clock_get_us();

    // Get drive base state
    pbio_control_state_t state_distance;
    pbio_control_state_t state_heading;
    pbio_error_t err = pbio_drivebase_get_state(db, &state_distance, &state_heading);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Start controller that controls the sum of both motor counts
    err = pbio_control_start_relative_angle_control(&db->control_distance, time_now, &state_distance, sum, sum_rate, after_stop);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Start controller that controls the difference between both motor counts
    err = pbio_control_start_relative_angle_control(&db->control_heading, time_now, &state_heading, dif, dif_rate, after_stop);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // At this point, the two trajectories may have different durations, so they won't complete at the same time
    // To account for this, we re-compute the shortest trajectory to have the same duration as the longest.

    // First, find out which controller takes the lead
    pbio_control_t *control_leader;
    pbio_control_t *control_follower;
    if (db->control_distance.trajectory.t3 > db->control_heading.trajectory.t3) {
        // Distance control takes the longest, so it will take the lead
        control_leader = &db->control_distance;
        control_follower = &db->control_heading;
    } else {
        // Heading control takes the longest, so it will take the lead
        control_leader = &db->control_heading;
        control_follower = &db->control_distance;
    }

    // Revise follower trajectory so it takes as long as the leader, achieved
    // by picking a lower speed and accelerations that makes the times match.
    pbio_trajectory_stretch(&control_follower->trajectory, control_leader->trajectory.t1, control_leader->trajectory.t2, control_leader->trajectory.t3);

    // The follower trajector holds until the leader trajectory says otherwise
    control_follower->after_stop = PBIO_ACTUATION_HOLD;
    control_follower->type = PBIO_CONTROL_ANGLE_FOLLOW;

    return PBIO_SUCCESS;
}

pbio_error_t pbio_drivebase_drive_curve(pbio_drivebase_t *db, int32_t radius, int32_t angle_or_distance, int32_t drive_speed, int32_t turn_rate, pbio_actuation_t after_stop) {

    int32_t arc_angle, arc_length;
    if (radius == PBIO_RADIUS_INF) {
        // For infinite radius, we want to drive straight, and
        // the angle_or_distance input is interpreted as distance.
        arc_angle = 0;
        arc_length = angle_or_distance;
    } else {
        // In the normal case, angle_or_distance is interpreted as the angle,
        // signed by the radius. Arc length is computed accordingly.
        arc_angle = radius < 0 ? -angle_or_distance : angle_or_distance;
        arc_length = (10 * abs(angle_or_distance) * radius) / 573;
    }

    // Convert arc length and speed to motor counts based on drivebase geometry
    int32_t relative_sum = pbio_control_user_to_counts(&db->control_distance.settings, arc_length);
    int32_t sum_rate = pbio_control_user_to_counts(&db->control_distance.settings, drive_speed);

    // Convert arc angle and speed to motor counts based on drivebase geometry
    int32_t relative_dif = pbio_control_user_to_counts(&db->control_heading.settings, arc_angle);
    int32_t dif_rate = pbio_control_user_to_counts(&db->control_heading.settings, turn_rate);

    return pbio_drivebase_drive_counts_relative(db, relative_sum, sum_rate, relative_dif, dif_rate, after_stop);
}

static pbio_error_t pbio_drivebase_drive_counts_forever(pbio_drivebase_t *db, int32_t sum_rate, int32_t dif_rate) {

    // Claim both servos for use by drivebase
    pbio_drivebase_claim_servos(db, true);

    // Get current time
    int32_t time_now = pbdrv_clock_get_us();

    // Get drive base state
    pbio_control_state_t state_distance;
    pbio_control_state_t state_heading;
    pbio_error_t err = pbio_drivebase_get_state(db, &state_distance, &state_heading);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // Initialize both controllers
    err = pbio_control_start_timed_control(&db->control_distance, time_now, &state_distance, DURATION_FOREVER, sum_rate, pbio_control_on_target_never, PBIO_ACTUATION_COAST);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    err = pbio_control_start_timed_control(&db->control_heading, time_now, &state_heading, DURATION_FOREVER, dif_rate, pbio_control_on_target_never, PBIO_ACTUATION_COAST);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    return PBIO_SUCCESS;
}

pbio_error_t pbio_drivebase_drive_forever(pbio_drivebase_t *db, int32_t speed, int32_t turn_rate) {
    return pbio_drivebase_drive_counts_forever(db,
        pbio_control_user_to_counts(&db->control_distance.settings, speed),
        pbio_control_user_to_counts(&db->control_heading.settings, turn_rate)
        );
}

pbio_error_t pbio_drivebase_get_state_user(pbio_drivebase_t *db, int32_t *distance, int32_t *drive_speed, int32_t *angle, int32_t *turn_rate) {

    // Get drive base state
    pbio_control_state_t state_distance;
    pbio_control_state_t state_heading;
    pbio_error_t err = pbio_drivebase_get_state(db, &state_distance, &state_heading);
    if (err != PBIO_SUCCESS) {
        return err;
    }
    *distance = pbio_control_counts_to_user(&db->control_distance.settings, state_distance.count);
    *drive_speed = pbio_control_counts_to_user(&db->control_distance.settings, state_distance.rate);
    *angle = pbio_control_counts_to_user(&db->control_heading.settings, state_heading.count);
    *turn_rate = pbio_control_counts_to_user(&db->control_heading.settings, state_heading.rate);
    return PBIO_SUCCESS;
}

pbio_error_t pbio_drivebase_get_drive_settings(pbio_drivebase_t *db, int32_t *drive_speed, int32_t *drive_acceleration, int32_t *turn_rate, int32_t *turn_acceleration) {

    pbio_control_settings_t *sd = &db->control_distance.settings;
    pbio_control_settings_t *sh = &db->control_heading.settings;

    *drive_speed = pbio_control_counts_to_user(sd, sd->max_rate);
    *drive_acceleration = pbio_control_counts_to_user(sd, sd->abs_acceleration);
    *turn_rate = pbio_control_counts_to_user(sh, sh->max_rate);
    *turn_acceleration = pbio_control_counts_to_user(sh, sh->abs_acceleration);

    return PBIO_SUCCESS;
}

pbio_error_t pbio_drivebase_set_drive_settings(pbio_drivebase_t *db, int32_t drive_speed, int32_t drive_acceleration, int32_t turn_rate, int32_t turn_acceleration) {

    pbio_control_settings_t *sd = &db->control_distance.settings;
    pbio_control_settings_t *sh = &db->control_heading.settings;

    sd->max_rate = pbio_control_user_to_counts(sd, drive_speed);
    sd->abs_acceleration = pbio_control_user_to_counts(sd, drive_acceleration);
    sh->max_rate = pbio_control_user_to_counts(sh, turn_rate);
    sh->abs_acceleration = pbio_control_user_to_counts(sh, turn_acceleration);

    return PBIO_SUCCESS;
}

#endif // PBDRV_CONFIG_NUM_MOTOR_CONTROLLER
