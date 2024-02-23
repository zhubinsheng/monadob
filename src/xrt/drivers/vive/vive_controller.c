// Copyright 2016 Philipp Zabel
// Copyright 2020-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vive Controller prober and driver code
 * @author Christoph Haag <christoph.gaag@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 *
 * Portions based on the VRPN Razer Hydra driver,
 * originally written by Rylie Pavlik and available under the BSL-1.0.
 */

#include "xrt/xrt_defines.h"
#include "xrt/xrt_prober.h"

#include "math/m_imu_3dof.h"
#include "math/m_relation_history.h"

#include "util/u_var.h"

#include "os/os_hid.h"
#include "os/os_threading.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_space.h"
#include "math/m_predict.h"

#include "util/u_json.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_trace_marker.h"

#include "vive/vive_config.h"
#include "vive/vive_bindings.h"
#include "vive/vive_poses.h"

#include "vive.h"
#include "vive_protocol.h"
#include "vive_controller.h"
#include "util/u_hand_simulation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_LINUX
#include <unistd.h>
#include <math.h>
#endif


/*
 *
 * Defines & structs.
 *
 */

enum vive_controller_input_index
{
	// common inputs
	VIVE_CONTROLLER_INDEX_AIM_POSE = 0,
	VIVE_CONTROLLER_INDEX_GRIP_POSE,
	VIVE_CONTROLLER_INDEX_SYSTEM_CLICK,
	VIVE_CONTROLLER_INDEX_TRIGGER_CLICK,
	VIVE_CONTROLLER_INDEX_TRIGGER_VALUE,
	VIVE_CONTROLLER_INDEX_TRACKPAD,
	VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH,

	// Vive Wand specific inputs
	VIVE_CONTROLLER_INDEX_SQUEEZE_CLICK,
	VIVE_CONTROLLER_INDEX_MENU_CLICK,
	VIVE_CONTROLLER_INDEX_TRACKPAD_CLICK,

	// Valve Index specific inputs
	VIVE_CONTROLLER_INDEX_THUMBSTICK,
	VIVE_CONTROLLER_INDEX_A_CLICK,
	VIVE_CONTROLLER_INDEX_B_CLICK,
	VIVE_CONTROLLER_INDEX_THUMBSTICK_CLICK,
	VIVE_CONTROLLER_INDEX_THUMBSTICK_TOUCH,
	VIVE_CONTROLLER_INDEX_SYSTEM_TOUCH,
	VIVE_CONTROLLER_INDEX_A_TOUCH,
	VIVE_CONTROLLER_INDEX_B_TOUCH,
	VIVE_CONTROLLER_INDEX_SQUEEZE_VALUE,
	VIVE_CONTROLLER_INDEX_SQUEEZE_FORCE,
	VIVE_CONTROLLER_INDEX_TRIGGER_TOUCH,
	VIVE_CONTROLLER_INDEX_TRACKPAD_FORCE,

	VIVE_CONTROLLER_HAND_TRACKING,

	VIVE_CONTROLLER_MAX_INDEX,
};



#define DEFAULT_HAPTIC_FREQ 150.0f
#define MIN_HAPTIC_DURATION 0.05f

// Debug define(s), always off.
#undef WATCHMAN2_PRINT_HID


/*
 *
 * Helper functions.
 *
 */

static inline struct vive_controller_device *
vive_controller_device(struct xrt_device *xdev)
{
	assert(xdev);
	struct vive_controller_device *ret = (struct vive_controller_device *)xdev;
	return ret;
}

static inline void
get_pose(struct vive_controller_device *d,
         enum xrt_input_name name,
         uint64_t at_timestamp_ns,
         struct xrt_space_relation *out_relation)
{
	struct xrt_space_relation imu_relation = {0};
	imu_relation.relation_flags = XRT_SPACE_RELATION_BITMASK_ALL;
	m_relation_history_get(d->fusion.relation_hist, at_timestamp_ns, &imu_relation);
	imu_relation.relation_flags = XRT_SPACE_RELATION_BITMASK_ALL; // Needed after history_get

	// Get the offset to the pose (this is from libsurvive's reporting position currently)
	struct xrt_pose pose_offset = XRT_POSE_IDENTITY;
	vive_poses_get_pose_offset(d->base.name, d->base.device_type, name, &pose_offset);

	// We want this to make grip the center of rotation.
	struct xrt_pose grip = XRT_POSE_IDENTITY;
	enum xrt_input_name grip_name = XRT_INPUT_INDEX_GRIP_POSE; //! @todo Vive poses only have index poses.
	vive_poses_get_pose_offset(d->base.name, d->base.device_type, grip_name, &grip);

	// Build proper relation.
	struct xrt_relation_chain chain = {0};
	m_relation_chain_push_pose(&chain, &pose_offset);
	m_relation_chain_push_inverted_pose_if_not_identity(&chain, &grip);
	m_relation_chain_push_relation(&chain, &imu_relation);
	m_relation_chain_push_pose_if_not_identity(&chain, &d->offset);

	// And resolve it.
	struct xrt_space_relation relation = {0};
	m_relation_chain_resolve(&chain, &relation);

	relation.linear_velocity = (struct xrt_vec3){0, 0, 0};

	*out_relation = relation;
}


/*
 *
 * Member functions.
 *
 */

static void
vive_controller_device_destroy(struct xrt_device *xdev)
{
	struct vive_controller_device *d = vive_controller_device(xdev);

	os_thread_helper_destroy(&d->controller_thread);

	// Now that the thread is not running we can destroy the lock.
	os_mutex_destroy(&d->lock);

	os_mutex_destroy(&d->fusion.mutex);
	m_relation_history_destroy(&d->fusion.relation_hist);
	m_imu_3dof_close(&d->fusion.i3dof);

	if (d->controller_hid)
		os_hid_destroy(d->controller_hid);

	free(d);
}

static void
vive_controller_device_wand_update_inputs(struct xrt_device *xdev)
{
	struct vive_controller_device *d = vive_controller_device(xdev);

	os_mutex_lock(&d->lock);

	uint8_t buttons = d->state.buttons;

	/*
	int i = 8;
	while(i--) {
	        putchar('0' + ((buttons >> i) & 1));
	}
	printf("\n");
	*/

	uint64_t now = os_monotonic_get_ns();

	/* d->state.buttons is bitmask of currently pressed buttons.
	 * (index n) nth bit in the bitmask -> input "name"
	 */
	const int button_index_map[] = {VIVE_CONTROLLER_INDEX_TRIGGER_CLICK,  VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH,
	                                VIVE_CONTROLLER_INDEX_TRACKPAD_CLICK, VIVE_CONTROLLER_INDEX_SYSTEM_CLICK,
	                                VIVE_CONTROLLER_INDEX_SQUEEZE_CLICK,  VIVE_CONTROLLER_INDEX_MENU_CLICK};

	int button_count = ARRAY_SIZE(button_index_map);
	for (int i = 0; i < button_count; i++) {

		bool pressed = (buttons >> i) & 1;
		bool last_pressed = (d->state.last_buttons >> i) & 1;

		if (pressed != last_pressed) {
			struct xrt_input *input = &d->base.inputs[button_index_map[i]];

			input->timestamp = now;
			input->value.boolean = pressed;

			VIVE_DEBUG(d, "button %d %s\n", i, pressed ? "pressed" : "released");
		}
	}
	d->state.last_buttons = d->state.buttons;


	struct xrt_input *trackpad_input = &d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD];
	trackpad_input->timestamp = now;
	trackpad_input->value.vec2.x = d->state.trackpad.x;
	trackpad_input->value.vec2.y = d->state.trackpad.y;
	VIVE_TRACE(d, "Trackpad: %f, %f", d->state.trackpad.x, d->state.trackpad.y);


	struct xrt_input *trigger_input = &d->base.inputs[VIVE_CONTROLLER_INDEX_TRIGGER_VALUE];
	trigger_input->timestamp = now;
	trigger_input->value.vec1.x = d->state.trigger;
	VIVE_TRACE(d, "Trigger: %f", d->state.trigger);

	os_mutex_unlock(&d->lock);
}

static void
vive_controller_device_index_update_inputs(struct xrt_device *xdev)
{
	XRT_TRACE_MARKER();

	struct vive_controller_device *d = vive_controller_device(xdev);

	os_mutex_lock(&d->lock);
	uint8_t buttons = d->state.buttons;

	/*
	int i = 8;
	while(i--) {
	        putchar('0' + ((buttons >> i) & 1));
	}
	printf("\n");
	*/

	bool was_trackpad_touched = d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH].value.boolean;

	uint64_t now = os_monotonic_get_ns();

	/* d->state.buttons is bitmask of currently pressed buttons.
	 * (index n) nth bit in the bitmask -> input "name"
	 */
	const int button_index_map[] = {VIVE_CONTROLLER_INDEX_TRIGGER_CLICK,    VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH,
	                                VIVE_CONTROLLER_INDEX_THUMBSTICK_CLICK, VIVE_CONTROLLER_INDEX_SYSTEM_CLICK,
	                                VIVE_CONTROLLER_INDEX_A_CLICK,          VIVE_CONTROLLER_INDEX_B_CLICK};

	int button_count = ARRAY_SIZE(button_index_map);
	for (int i = 0; i < button_count; i++) {

		bool pressed = (buttons >> i) & 1;
		bool last_pressed = (d->state.last_buttons >> i) & 1;

		if (pressed != last_pressed) {
			struct xrt_input *input = &d->base.inputs[button_index_map[i]];

			input->timestamp = now;
			input->value.boolean = pressed;

			VIVE_DEBUG(d, "button %d %s\n", i, pressed ? "pressed" : "released");
		}
	}
	d->state.last_buttons = d->state.buttons;

	bool is_trackpad_touched = d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH].value.boolean;

	/* trackpad and thumbstick position are the same usb events.
	 * report trackpad position when trackpad has been touched last, and
	 * thumbstick position when trackpad touch has been released
	 */
	struct xrt_input *thumb_input;

	// after releasing trackpad, next 0,0 position still goes to trackpad
	if (is_trackpad_touched || was_trackpad_touched)
		thumb_input = &d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD];
	else
		thumb_input = &d->base.inputs[VIVE_CONTROLLER_INDEX_THUMBSTICK];
	thumb_input->timestamp = now;
	thumb_input->value.vec2.x = d->state.trackpad.x;
	thumb_input->value.vec2.y = d->state.trackpad.y;

	const char *component = is_trackpad_touched || was_trackpad_touched ? "Trackpad" : "Thumbstick";
	VIVE_TRACE(d, "%s: %f, %f", component, d->state.trackpad.x, d->state.trackpad.y);


	struct xrt_input *trigger_input = &d->base.inputs[VIVE_CONTROLLER_INDEX_TRIGGER_VALUE];

	trigger_input->timestamp = now;
	trigger_input->value.vec1.x = d->state.trigger;

	VIVE_TRACE(d, "Trigger: %f", d->state.trigger);


	/* d->state.touch is bitmask of currently touched buttons.
	 * (index n) nth bit in the bitmask -> input "name"
	 */
	const int touched_button_index_map[] = {0,
	                                        0,
	                                        0,
	                                        VIVE_CONTROLLER_INDEX_SYSTEM_TOUCH,
	                                        VIVE_CONTROLLER_INDEX_A_TOUCH,
	                                        VIVE_CONTROLLER_INDEX_B_TOUCH,
	                                        VIVE_CONTROLLER_INDEX_THUMBSTICK_TOUCH};
	int touch_button_count = ARRAY_SIZE(touched_button_index_map);
	uint8_t touch_buttons = d->state.touch;
	for (int i = 0; i < touch_button_count; i++) {

		bool touched = (touch_buttons >> i) & 1;
		bool last_touched = (d->state.last_touch >> i) & 1;

		if (touched != last_touched) {
			struct xrt_input *input = &d->base.inputs[touched_button_index_map[i]];

			input->timestamp = now;
			input->value.boolean = touched;

			VIVE_DEBUG(d, "button %d %s\n", i, touched ? "touched" : "untouched");
		}
	}
	d->state.last_touch = d->state.touch;

	d->base.inputs[VIVE_CONTROLLER_INDEX_SQUEEZE_FORCE].value.vec1.x = (float)d->state.squeeze_force / UINT8_MAX;
	d->base.inputs[VIVE_CONTROLLER_INDEX_SQUEEZE_FORCE].timestamp = now;
	if (d->state.squeeze_force > 0) {
		VIVE_DEBUG(d, "Squeeze force: %f\n", (float)d->state.squeeze_force / UINT8_MAX);
	}

	d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD_FORCE].value.vec1.x = (float)d->state.trackpad_force / UINT8_MAX;
	d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD_FORCE].timestamp = now;
	if (d->state.trackpad_force > 0) {
		VIVE_DEBUG(d, "Trackpad force: %f\n", (float)d->state.trackpad_force / UINT8_MAX);
	}

	os_mutex_unlock(&d->lock);
}

static void
vive_controller_get_hand_tracking(struct xrt_device *xdev,
                                  enum xrt_input_name name,
                                  uint64_t requested_timestamp_ns,
                                  struct xrt_hand_joint_set *out_value,
                                  uint64_t *out_timestamp_ns)
{
	XRT_TRACE_MARKER();

	struct vive_controller_device *d = vive_controller_device(xdev);

	if (name != XRT_INPUT_GENERIC_HAND_TRACKING_LEFT && name != XRT_INPUT_GENERIC_HAND_TRACKING_RIGHT) {
		VIVE_ERROR(d, "unknown input name for hand tracker");
		return;
	}

	enum xrt_hand hand = d->config.variant == CONTROLLER_INDEX_LEFT ? XRT_HAND_LEFT : XRT_HAND_RIGHT;

	float thumb_curl = 0.0f;
	//! @todo place thumb preciely on the button that is touched/pressed
	if (d->base.inputs[VIVE_CONTROLLER_INDEX_A_TOUCH].value.boolean ||
	    d->base.inputs[VIVE_CONTROLLER_INDEX_B_TOUCH].value.boolean ||
	    d->base.inputs[VIVE_CONTROLLER_INDEX_THUMBSTICK_TOUCH].value.boolean ||
	    d->base.inputs[VIVE_CONTROLLER_INDEX_TRACKPAD_TOUCH].value.boolean) {
		thumb_curl = 1.0;
	}

	struct u_hand_tracking_curl_values values = {
	    .little = (float)d->state.pinky_finger_handle / UINT8_MAX,
	    .ring = (float)d->state.ring_finger_handle / UINT8_MAX,
	    .middle = (float)d->state.middle_finger_handle / UINT8_MAX,
	    .index = (float)d->state.index_finger_trigger / UINT8_MAX,
	    .thumb = thumb_curl,
	};

	struct xrt_space_relation hand_relation;
	get_pose(d, name, requested_timestamp_ns, &hand_relation);

	u_hand_sim_simulate_for_valve_index_knuckles(&values, hand, &hand_relation, out_value);

	// This is the truth - we pose-predicted or interpolated all the way up to `at_timestamp_ns`.
	*out_timestamp_ns = requested_timestamp_ns;

	out_value->is_active = true;
}

static void
vive_controller_device_get_tracked_pose(struct xrt_device *xdev,
                                        enum xrt_input_name name,
                                        uint64_t at_timestamp_ns,
                                        struct xrt_space_relation *out_relation)
{
	struct vive_controller_device *d = vive_controller_device(xdev);

	// U_LOG_D("input name %d %d", name, XRT_INPUT_VIVE_GRIP_POSE);
	if (name != XRT_INPUT_VIVE_AIM_POSE && name != XRT_INPUT_VIVE_GRIP_POSE && name != XRT_INPUT_INDEX_AIM_POSE &&
	    name != XRT_INPUT_INDEX_GRIP_POSE) {
		VIVE_ERROR(d, "unknown input name");
		return;
	}

	get_pose(d, name, at_timestamp_ns, out_relation);
}

static int
vive_controller_haptic_pulse(struct vive_controller_device *d, const union xrt_output_value *value)
{
	float duration_seconds;
	if (value->vibration.duration_ns == XRT_MIN_HAPTIC_DURATION) {
		VIVE_TRACE(d, "Haptic pulse duration: using %f minimum", MIN_HAPTIC_DURATION);
		duration_seconds = MIN_HAPTIC_DURATION;
	} else {
		duration_seconds = time_ns_to_s(value->vibration.duration_ns);
	}

	VIVE_TRACE(d, "Haptic pulse amp %f, %fHz, %fs", value->vibration.amplitude, value->vibration.frequency,
	           duration_seconds);
	float frequency = value->vibration.frequency;

	if (frequency == XRT_FREQUENCY_UNSPECIFIED) {
		VIVE_TRACE(d, "Haptic pulse frequency unspecified, setting to %fHz", DEFAULT_HAPTIC_FREQ);
		frequency = DEFAULT_HAPTIC_FREQ;
	}


	/* haptic pulse for Vive Controller:
	 * desired_frequency = 1000 * 1000 / (high + low).
	 * => (high + low) = 1000 * 1000 / desired_frequency
	 * repeat = desired_duration_in_seconds * desired_frequency.
	 *
	 * I think:
	 * Lowest amplitude: 1, high+low-1
	 * Highest amplitude: (high+low)/2, / (high+low)/2
	 */

	float high_plus_low = 1000.f * 1000.f / frequency;
	uint16_t pulse_low = (uint16_t)(value->vibration.amplitude * high_plus_low / 2.);

	/* Vive Controller doesn't vibrate with value == 0.
	 * Not sure if this actually happens, but let's fix it anyway. */
	if (pulse_low == 0)
		pulse_low = 1;

	uint16_t pulse_high = high_plus_low - pulse_low;

	uint16_t repeat_count = duration_seconds * frequency;

	const struct vive_controller_haptic_pulse_report report = {
	    .id = VIVE_CONTROLLER_COMMAND_REPORT_ID,
	    .command = VIVE_CONTROLLER_HAPTIC_PULSE_COMMAND,
	    .len = 7,
	    .zero = 0x00,
	    .pulse_high = __cpu_to_le16(pulse_high),
	    .pulse_low = __cpu_to_le16(pulse_low),
	    .repeat_count = __cpu_to_le16(repeat_count),
	};

	return os_hid_set_feature(d->controller_hid, (uint8_t *)&report, sizeof(report));
}

static void
vive_controller_device_set_output(struct xrt_device *xdev,
                                  enum xrt_output_name name,
                                  const union xrt_output_value *value)
{
	struct vive_controller_device *d = vive_controller_device(xdev);

	if (name != XRT_OUTPUT_NAME_VIVE_HAPTIC && name != XRT_OUTPUT_NAME_INDEX_HAPTIC) {
		VIVE_ERROR(d, "Unknown output\n");
		return;
	}

	bool pulse = value->vibration.amplitude > 0.01;
	if (!pulse) {
		return;
	}

	os_mutex_lock(&d->lock);
	vive_controller_haptic_pulse(d, value);
	os_mutex_unlock(&d->lock);
}


/*
 *
 * Misc functions.
 *
 */

static void
controller_handle_battery(struct vive_controller_device *d, struct vive_controller_battery_sample *sample)
{
	uint8_t charge_percent = sample->battery & VIVE_CONTROLLER_BATTERY_CHARGE_MASK;
	bool charging = sample->battery & VIVE_CONTROLLER_BATTERY_CHARGING;
	VIVE_DEBUG(d, "Charging %d, percent %d\n", charging, charge_percent);
	d->state.charging = charging;
	d->state.battery = charge_percent;
}

static void
controller_handle_buttons(struct vive_controller_device *d, struct vive_controller_button_sample *sample)
{
	d->state.buttons = sample->buttons;
}

static void
controller_handle_touch_position(struct vive_controller_device *d, struct vive_controller_touch_sample *sample)
{
	int16_t x = __le16_to_cpu(sample->touch[0]);
	int16_t y = __le16_to_cpu(sample->touch[1]);
	d->state.trackpad.x = (float)x / INT16_MAX;
	d->state.trackpad.y = (float)y / INT16_MAX;
	if (d->state.trackpad.x != 0 || d->state.trackpad.y != 0)
		VIVE_TRACE(d, "Trackpad %f,%f\n", d->state.trackpad.x, d->state.trackpad.y);
}

static void
controller_handle_analog_trigger(struct vive_controller_device *d, struct vive_controller_trigger_sample *sample)
{
	d->state.trigger = (float)sample->trigger / UINT8_MAX;
	VIVE_TRACE(d, "Trigger %f\n", d->state.trigger);
}


static void
vive_controller_handle_imu_sample(struct vive_controller_device *d, struct watchman_imu_sample *sample)
{
	XRT_TRACE_MARKER();

	uint64_t now_ns = os_monotonic_get_ns();

	/* ouvrt: "Time in 48 MHz ticks, but we are missing the low byte" */
	uint32_t time_raw = d->last_ticks | (sample->timestamp_hi << 8);
	ticks_to_ns(time_raw, &d->imu.last_sample_ticks, &d->imu.last_sample_ts_ns);

	int16_t acc[3] = {
	    __le16_to_cpu(sample->acc[0]),
	    __le16_to_cpu(sample->acc[1]),
	    __le16_to_cpu(sample->acc[2]),
	};

	int16_t gyro[3] = {
	    __le16_to_cpu(sample->gyro[0]),
	    __le16_to_cpu(sample->gyro[1]),
	    __le16_to_cpu(sample->gyro[2]),
	};

	float scale = (float)d->config.imu.acc_range / 32768.0f;
	struct xrt_vec3 acceleration = {
	    scale * d->config.imu.acc_scale.x * acc[0] - d->config.imu.acc_bias.x,
	    scale * d->config.imu.acc_scale.y * acc[1] - d->config.imu.acc_bias.y,
	    scale * d->config.imu.acc_scale.z * acc[2] - d->config.imu.acc_bias.z,
	};

	scale = (float)d->config.imu.gyro_range / 32768.0f;
	struct xrt_vec3 angular_velocity = {
	    scale * d->config.imu.gyro_scale.x * gyro[0] - d->config.imu.gyro_bias.x,
	    scale * d->config.imu.gyro_scale.y * gyro[1] - d->config.imu.gyro_bias.y,
	    scale * d->config.imu.gyro_scale.z * gyro[2] - d->config.imu.gyro_bias.z,
	};

	VIVE_TRACE(d, "ACC  %f %f %f", acceleration.x, acceleration.y, acceleration.z);
	VIVE_TRACE(d, "GYRO %f %f %f", angular_velocity.x, angular_velocity.y, angular_velocity.z);
	/*
	 */

	if (d->config.variant == CONTROLLER_VIVE_WAND) {
		struct xrt_vec3 fixed_acceleration = {.x = -acceleration.x, .y = -acceleration.z, .z = -acceleration.y};
		acceleration = fixed_acceleration;

		struct xrt_vec3 fixed_angular_velocity = {
		    .x = -angular_velocity.x, .y = -angular_velocity.z, .z = -angular_velocity.y};
		angular_velocity = fixed_angular_velocity;
	} else if (d->config.variant == CONTROLLER_INDEX_RIGHT) {
		struct xrt_vec3 fixed_acceleration = {.x = acceleration.z, .y = -acceleration.y, .z = acceleration.x};
		acceleration = fixed_acceleration;

		struct xrt_vec3 fixed_angular_velocity = {
		    .x = angular_velocity.z, .y = -angular_velocity.y, .z = angular_velocity.x};
		angular_velocity = fixed_angular_velocity;
	} else if (d->config.variant == CONTROLLER_INDEX_LEFT) {
		struct xrt_vec3 fixed_acceleration = {.x = -acceleration.z, .y = acceleration.x, .z = -acceleration.y};
		acceleration = fixed_acceleration;

		struct xrt_vec3 fixed_angular_velocity = {
		    .x = -angular_velocity.z, .y = angular_velocity.x, .z = -angular_velocity.y};
		angular_velocity = fixed_angular_velocity;
	}

	d->last.acc = acceleration;
	d->last.gyro = angular_velocity;

	struct xrt_space_relation rel = {0};
	rel.relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;

	os_mutex_lock(&d->fusion.mutex);
	m_imu_3dof_update(&d->fusion.i3dof, d->imu.last_sample_ts_ns, &acceleration, &angular_velocity);
	rel.pose.orientation = d->fusion.i3dof.rot;
	os_mutex_unlock(&d->fusion.mutex);

	m_relation_history_push(d->fusion.relation_hist, &rel, now_ns);

	// Update the pose we show in the GUI.
	d->pose = rel.pose;
}

static void
controller_handle_touch_force(struct vive_controller_device *d, struct watchman_touch_force *sample)
{
	d->state.touch = sample->touch;

	d->state.middle_finger_handle = sample->middle_finger_handle;
	d->state.ring_finger_handle = sample->ring_finger_handle;
	d->state.pinky_finger_handle = sample->pinky_finger_handle;
	d->state.index_finger_trigger = sample->index_finger_trigger;

	d->state.squeeze_force = sample->squeeze_force;
	d->state.trackpad_force = sample->trackpad_force;
}

static void
vive_controller_handle_lighthousev1(struct vive_controller_device *d, uint8_t *buf, uint8_t len)
{
	VIVE_TRACE(d, "Got lighthouse message with len %d.\n", len);
}

/*
 * Handles battery, imu, trigger, buttons, trackpad.
 * Then hands off to vive_controller_handle_lighthousev1().
 */
static void
vive_controller_decode_watchmanv1(struct vive_controller_device *d, struct vive_controller_message *message)
{
	uint8_t *buf = message->payload;
	uint8_t *end = message->payload + message->len - 1;

	/*
	for (int i = 0; i < message->len; i++) {
	        //printf("%02x ", buf[i]);
	        int j = 8;
	        while(j--) {
	                putchar('0' + ((buf[i] >> j) & 1));
	        }
	        putchar(' ');
	}
	printf("\n");
	*/

	/* payload starts with "event flags" byte.
	 * If it does not start with 111, it contains only lighthouse data.
	 * If it starts with 111, events follow in this order, each of them
	 * optional:
	 *   - battery:  1 byte (1110???1)
	 *   - trigger:  1 byte (1111?1??)
	 *   - trackpad: 4 byte (1111??1?)
	 *   - buttons:  1 byte (1111???1)
	 *   - imu:     13 byte (111?1???)
	 * There may be another input event after a battery event.
	 * Lighthouse data may follow in the rest of the payload.
	 */

	// input events have first three bits set
	if ((*buf & 0xe0) == 0xe0 && buf < end) {

		// clang-format off

		// battery follows when 1110???1
		bool has_battery  = (*buf & 0x10) != 0x10 && (*buf & 0x1) == 0x1;

		// input follows when 1111?<trigger><trackpad><buttons>
		bool has_trigger  = (*buf & 0x10) == 0x10 && (*buf & 0x4) == 0x4;
		bool has_trackpad = (*buf & 0x10) == 0x10 && (*buf & 0x2) == 0x2;
		bool has_buttons  = (*buf & 0x10) == 0x10 && (*buf & 0x1) == 0x1;

		// imu event follows when 111?1???
		// there are imu-only messages, and imu-after-battery
		bool has_imu      = (*buf & 0x08) == 0x8;

		// clang-format on

		VIVE_TRACE(d,
		           "battery %d trigger %d trackpad %d "
		           "buttons %d imu %d",
		           has_battery, has_trigger, has_trackpad, has_buttons, has_imu);

		buf++;

		if (has_battery) {
			controller_handle_battery(d, (struct vive_controller_battery_sample *)buf);
			buf += sizeof(struct vive_controller_battery_sample);
		}

		if (has_buttons) {
			controller_handle_buttons(d, (struct vive_controller_button_sample *)buf);
			buf += sizeof(struct vive_controller_button_sample);
		}
		if (has_trigger) {
			controller_handle_analog_trigger(d, (struct vive_controller_trigger_sample *)buf);
			buf += sizeof(struct vive_controller_trigger_sample);
		}
		if (has_trackpad) {
			controller_handle_touch_position(d, (struct vive_controller_touch_sample *)buf);
			buf += 4;
		}
		if (has_imu) {
			vive_controller_handle_imu_sample(d, (struct watchman_imu_sample *)buf);
			buf += sizeof(struct watchman_imu_sample);
		}
	}

	if (buf > end)
		VIVE_ERROR(d, "overshoot: %ld\n", buf - end);

	if (buf < end)
		vive_controller_handle_lighthousev1(d, buf, end - buf);
}

/*
 * Handles battery, imu, trigger, buttons, trackpad.
 * Then hands off to vive_controller_handle_lighthousev1().
 */
static void
vive_controller_decode_watchmanv2(struct vive_controller_device *d, struct vive_controller_message *message)
{
	uint8_t *buf = message->payload;
	uint8_t *end = message->payload + message->len - 1;

#ifdef WATCHMAN2_PRINT_HID
	for (int i = 0; i < message->len; i++) {
		int j = 8;
		while (j--) {
			putchar('0' + ((buf[i] >> j) & 1));
		}
		putchar(' ');
	}
	printf("\n");
	for (int i = 0; i < message->len; i++) {
		printf("%8.02x ", buf[i]);
	}
	printf("\n");
#endif


	/* payload starts with "event flags" byte. */

	/*
	 * If flags == 0xe1 == 11100001, battery follows.
	 * Battery is always at the beginning of the payload.
	 * after battery there may be another payload.
	 * careful: 0xe1 often comes alone without actual data.
	 */
	if (*buf == 0xe1 && buf < end) {
		buf++;
		controller_handle_battery(d, (struct vive_controller_battery_sample *)buf);
		buf += sizeof(struct vive_controller_battery_sample);

#ifdef WATCHMAN2_PRINT_HID
		printf(
		    "         "
		    "  battery");
#endif
	}


	/*
	 * If flags == 0xf0 == 11110000,  8 bytes touch+force follow.
	 * This package is always at the beginning of the payload.
	 */
	if (*buf == 0xf0 && buf < end) {
		buf++;
		controller_handle_touch_force(d, (struct watchman_touch_force *)buf);
		size_t s = sizeof(struct watchman_touch_force);
		buf += s;

#ifdef WATCHMAN2_PRINT_HID
		printf("        ");
		for (size_t i = 0; i < s; i++)
			printf("  t&force");
#endif
	}

	/*
	 * If flags == 0xe8 == 11101000, imu data follows.
	 * This package can be at the beginning of the payload or after battery.
	 */
	// TODO: it's possible we misparse non-im udata as imu data
	if (*buf == 0xe8 && buf < end) {
		buf++;
		vive_controller_handle_imu_sample(d, (struct watchman_imu_sample *)buf);
		size_t s = sizeof(struct watchman_imu_sample);
		buf += s;

#ifdef WATCHMAN2_PRINT_HID
		printf("        ");
		for (size_t i = 0; i < s; i++)
			printf("      imu");
#endif
	}

	/*
	 * If flags starts with 1111, events follow in this order,
	 * each of them optional:
	 *   - trigger:      1 byte  (1111?1??)
	 *   - trackpad:     4 byte  (1111??1?)
	 *   - buttons:      1 byte  (1111???1)
	 *   - touch&force+imu or imu: 8+13 or 13 byte (11111???)
	 * There may be another input event after a battery event.
	 */
	if ((*buf & 0xf0) == 0xf0 && buf < end - 1) {

		// clang-format off

		// input flags 1111<touch_force><trigger><trackpad><buttons>
		bool has_touch_force = (*buf & 0x8) == 0x8;
		bool has_trigger     = (*buf & 0x4) == 0x4;
		bool has_trackpad    = (*buf & 0x2) == 0x2;
		bool has_buttons     = (*buf & 0x1) == 0x1;

		// clang-format on

		buf++;

#ifdef WATCHMAN2_PRINT_HID
		printf("        ");
#endif

		if (has_buttons) {
			controller_handle_buttons(d, (struct vive_controller_button_sample *)buf);
			buf += sizeof(struct vive_controller_button_sample);
#ifdef WATCHMAN2_PRINT_HID
			printf("  buttons");
#endif
		}
		if (has_trigger) {
			controller_handle_analog_trigger(d, (struct vive_controller_trigger_sample *)buf);
			buf += sizeof(struct vive_controller_trigger_sample);
#ifdef WATCHMAN2_PRINT_HID
			printf("  trigger");
#endif
		}
		if (has_trackpad) {
			controller_handle_touch_position(d, (struct vive_controller_touch_sample *)buf);
			buf += sizeof(struct vive_controller_touch_sample);
#ifdef WATCHMAN2_PRINT_HID
			for (unsigned long i = 0; i < sizeof(struct vive_controller_touch_sample); i++)
				printf(" trackpad");
#endif
		}
		if (has_touch_force) {
			uint8_t type_flag = *buf;
			if (type_flag == TYPE_FLAG_TOUCH_FORCE) {
				controller_handle_touch_force(d, (struct watchman_touch_force *)buf);
				size_t s = sizeof(struct watchman_touch_force);
				buf += s;
#ifdef WATCHMAN2_PRINT_HID
				for (unsigned long i = 0; i < sizeof(struct watchman_touch_force); i++)
					printf("  t&force");
#endif
			}
		}
		// if something still follows, usually imu
		// sometimes it's 5 unknown bytes'
		if (buf < end && end - buf >= (long)sizeof(struct watchman_imu_sample)) {
			vive_controller_handle_imu_sample(d, (struct watchman_imu_sample *)buf);
			size_t s = sizeof(struct watchman_imu_sample);
			buf += s;
#ifdef WATCHMAN2_PRINT_HID
			for (unsigned long i = 0; i < sizeof(struct watchman_imu_sample); i++)
				printf("      imu");
#endif
		}
	}


#ifdef WATCHMAN2_PRINT_HID
	printf("\n");
#endif

	if (buf < end) {
		VIVE_TRACE(d, "%ld bytes unparsed data in message\n", message->len - (buf - message->payload) - 1);
	}
	if (buf > end)
		VIVE_ERROR(d, "overshoot: %ld\n", buf - end);

	//! @todo: Parse lighthouse v2 data
}
/*
 * Decodes multiplexed Wireless Receiver messages.
 */
static void
vive_controller_decode_message(struct vive_controller_device *d, struct vive_controller_message *message)
{
	d->last_ticks = (message->timestamp_hi << 24) | (message->timestamp_lo << 16);

	//! @todo: Check if Vive controller on watchman2 is correctly handled
	//! with watchman2 codepath
	switch (d->watchman_gen) {
	case WATCHMAN_GEN1: vive_controller_decode_watchmanv1(d, message); break;
	case WATCHMAN_GEN2: vive_controller_decode_watchmanv2(d, message); break;
	default: VIVE_ERROR(d, "Can't decode unknown watchman gen");
	}
}

#define FEATURE_BUFFER_SIZE 256

static int
vive_controller_device_update(struct vive_controller_device *d)
{
	uint8_t buf[FEATURE_BUFFER_SIZE];

	int ret = os_hid_read(d->controller_hid, buf, sizeof(buf), 1000);
	if (ret == 0) {
		// controller off
		return true;
	}

	if (ret < 0) {
		VIVE_ERROR(d, "Failed to read device '%i'!", ret);
		return false;
	}

	switch (buf[0]) {
	case VIVE_CONTROLLER_REPORT1_ID:
		os_mutex_lock(&d->lock);
		vive_controller_decode_message(d, &((struct vive_controller_report1 *)buf)->message);
		os_mutex_unlock(&d->lock);
		break;

	case VIVE_CONTROLLER_REPORT2_ID:
		os_mutex_lock(&d->lock);
		vive_controller_decode_message(d, &((struct vive_controller_report2 *)buf)->message[0]);
		vive_controller_decode_message(d, &((struct vive_controller_report2 *)buf)->message[1]);
		os_mutex_unlock(&d->lock);
		break;
	case VIVE_CONTROLLER_DISCONNECT_REPORT_ID: VIVE_DEBUG(d, "Controller disconnected."); break;
	default: VIVE_ERROR(d, "Unknown controller message type: %u", buf[0]);
	}

	return true;
}

static void *
vive_controller_run_thread(void *ptr)
{
	struct vive_controller_device *d = (struct vive_controller_device *)ptr;

	uint8_t buf[FEATURE_BUFFER_SIZE];
	while (os_hid_read(d->controller_hid, buf, sizeof(buf), 0) > 0) {
		// Empty queue first
	}

	os_thread_helper_lock(&d->controller_thread);
	while (os_thread_helper_is_running_locked(&d->controller_thread)) {
		os_thread_helper_unlock(&d->controller_thread);

		if (!vive_controller_device_update(d)) {
			return NULL;
		}

		// Just keep swimming.
		os_thread_helper_lock(&d->controller_thread);
	}

	return NULL;
}

void
vive_controller_reset_pose_cb(void *ptr)
{
	struct vive_controller_device *d = (struct vive_controller_device *)ptr;
	os_mutex_lock(&d->fusion.mutex);
	m_imu_3dof_reset(&d->fusion.i3dof);
	d->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	os_mutex_unlock(&d->fusion.mutex);
}

static void
vive_controller_setup_ui(struct vive_controller_device *d)
{
	char tmp[256] = {0};
	snprintf(tmp, sizeof(tmp), "Vive Controller %zu", d->index);

	u_var_add_root(d, tmp, false);
	u_var_add_log_level(d, &d->log_level, "Log level");

	u_var_add_gui_header(d, NULL, "Tracking");
	u_var_add_pose(d, &d->pose, "Tracked Pose");
	u_var_add_pose(d, &d->offset, "Pose Offset");

	d->gui.reset_pose_btn.cb = vive_controller_reset_pose_cb;
	d->gui.reset_pose_btn.ptr = d;
	u_var_add_button(d, &d->gui.reset_pose_btn, "Reset pose");

	u_var_add_gui_header(d, NULL, "3DoF Tracking");
	m_imu_3dof_add_vars(&d->fusion.i3dof, d, "");
	u_var_add_gui_header(d, NULL, "Calibration");
	u_var_add_vec3_f32(d, &d->config.imu.acc_scale, "acc_scale");
	u_var_add_vec3_f32(d, &d->config.imu.acc_bias, "acc_bias");
	u_var_add_vec3_f32(d, &d->config.imu.gyro_scale, "gyro_scale");
	u_var_add_vec3_f32(d, &d->config.imu.gyro_bias, "gyro_bias");
}

/*
 *
 * 'Exported' function(s).
 *
 */

#define SET_WAND_INPUT(NAME, NAME2)                                                                                    \
	do {                                                                                                           \
		(d->base.inputs[VIVE_CONTROLLER_INDEX_##NAME].name = XRT_INPUT_VIVE_##NAME2);                          \
	} while (0)

#define SET_INDEX_INPUT(NAME, NAME2)                                                                                   \
	do {                                                                                                           \
		(d->base.inputs[VIVE_CONTROLLER_INDEX_##NAME].name = XRT_INPUT_INDEX_##NAME2);                         \
	} while (0)

struct vive_controller_device *
vive_controller_create(struct os_hid_device *controller_hid, enum watchman_gen watchman_gen, int controller_num)
{

	enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	struct vive_controller_device *d =
	    U_DEVICE_ALLOCATE(struct vive_controller_device, flags, VIVE_CONTROLLER_MAX_INDEX, 1);

	d->log_level = debug_get_log_option_vive_log();
	d->watchman_gen = WATCHMAN_GEN_UNKNOWN;
	d->config.variant = CONTROLLER_UNKNOWN;
	d->index = controller_num;
	d->pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	d->offset = (struct xrt_pose)XRT_POSE_IDENTITY;

	d->watchman_gen = watchman_gen;

	m_imu_3dof_init(&d->fusion.i3dof, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	m_relation_history_create(&d->fusion.relation_hist);
	int ret = os_mutex_init(&d->fusion.mutex);
	if (ret != 0) {
		VIVE_ERROR(d, "Failed to init 3dof mutex");
		return false;
	}

	/* default values, will be queried from device */
	d->config.imu.gyro_range = 8.726646f;
	d->config.imu.acc_range = 39.226600f;

	d->config.imu.acc_scale.x = 1.0f;
	d->config.imu.acc_scale.y = 1.0f;
	d->config.imu.acc_scale.z = 1.0f;
	d->config.imu.gyro_scale.x = 1.0f;
	d->config.imu.gyro_scale.y = 1.0f;
	d->config.imu.gyro_scale.z = 1.0f;

	d->config.imu.acc_bias.x = 0.0f;
	d->config.imu.acc_bias.y = 0.0f;
	d->config.imu.acc_bias.z = 0.0f;
	d->config.imu.gyro_bias.x = 0.0f;
	d->config.imu.gyro_bias.y = 0.0f;
	d->config.imu.gyro_bias.z = 0.0f;

	d->controller_hid = controller_hid;

	d->base.destroy = vive_controller_device_destroy;
	d->base.get_tracked_pose = vive_controller_device_get_tracked_pose;
	d->base.set_output = vive_controller_device_set_output;

	// Have to init before destroy is called.
	os_mutex_init(&d->lock);
	os_thread_helper_init(&d->controller_thread);

	if (vive_get_imu_range_report(d->controller_hid, &d->config.imu.gyro_range, &d->config.imu.acc_range) != 0) {
		// reading range report fails for powered off controller
		vive_controller_device_destroy(&d->base);
		return NULL;
	}

	VIVE_DEBUG(d, "Vive controller gyroscope range     %f", d->config.imu.gyro_range);
	VIVE_DEBUG(d, "Vive controller accelerometer range %f", d->config.imu.acc_range);

	// successful config parsing determines d->config.variant
	char *config = vive_read_config(d->controller_hid);

	if (config != NULL) {
		vive_config_parse_controller(&d->config, config, d->log_level);
		free(config);
	} else {
		VIVE_ERROR(d, "Could not get Vive controller config\n");
		vive_controller_device_destroy(&d->base);
		return NULL;
	}

	snprintf(d->base.serial, XRT_DEVICE_NAME_LEN, "%s", d->config.firmware.device_serial_number);

	if (d->config.variant == CONTROLLER_VIVE_WAND) {
		d->base.name = XRT_DEVICE_VIVE_WAND;
		snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Vive Wand Controller (vive)");

		SET_WAND_INPUT(SYSTEM_CLICK, SYSTEM_CLICK);
		SET_WAND_INPUT(SQUEEZE_CLICK, SQUEEZE_CLICK);
		SET_WAND_INPUT(MENU_CLICK, MENU_CLICK);
		SET_WAND_INPUT(TRIGGER_CLICK, TRIGGER_CLICK);
		SET_WAND_INPUT(TRIGGER_VALUE, TRIGGER_VALUE);
		SET_WAND_INPUT(TRACKPAD, TRACKPAD);
		SET_WAND_INPUT(TRACKPAD_CLICK, TRACKPAD_CLICK);
		SET_WAND_INPUT(TRACKPAD_TOUCH, TRACKPAD_TOUCH);

		SET_WAND_INPUT(AIM_POSE, AIM_POSE);
		SET_WAND_INPUT(GRIP_POSE, GRIP_POSE);

		d->base.outputs[0].name = XRT_OUTPUT_NAME_VIVE_HAPTIC;

		d->base.update_inputs = vive_controller_device_wand_update_inputs;

		d->base.binding_profiles = vive_binding_profiles_wand;
		d->base.binding_profile_count = vive_binding_profiles_wand_count;

		d->base.device_type = XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER;
	} else if (d->config.variant == CONTROLLER_INDEX_LEFT || d->config.variant == CONTROLLER_INDEX_RIGHT) {
		d->base.name = XRT_DEVICE_INDEX_CONTROLLER;

		SET_INDEX_INPUT(SYSTEM_CLICK, SYSTEM_CLICK);
		SET_INDEX_INPUT(A_CLICK, A_CLICK);
		SET_INDEX_INPUT(B_CLICK, B_CLICK);
		SET_INDEX_INPUT(TRIGGER_CLICK, TRIGGER_CLICK);
		SET_INDEX_INPUT(TRIGGER_VALUE, TRIGGER_VALUE);
		SET_INDEX_INPUT(TRACKPAD, TRACKPAD);
		SET_INDEX_INPUT(TRACKPAD_TOUCH, TRACKPAD_TOUCH);
		SET_INDEX_INPUT(THUMBSTICK, THUMBSTICK);
		SET_INDEX_INPUT(THUMBSTICK_CLICK, THUMBSTICK_CLICK);

		SET_INDEX_INPUT(THUMBSTICK_TOUCH, THUMBSTICK_TOUCH);
		SET_INDEX_INPUT(SYSTEM_TOUCH, SYSTEM_TOUCH);
		SET_INDEX_INPUT(A_TOUCH, A_TOUCH);
		SET_INDEX_INPUT(B_TOUCH, B_TOUCH);
		SET_INDEX_INPUT(SQUEEZE_VALUE, SQUEEZE_VALUE);
		SET_INDEX_INPUT(SQUEEZE_FORCE, SQUEEZE_FORCE);
		SET_INDEX_INPUT(TRIGGER_TOUCH, TRIGGER_TOUCH);
		SET_INDEX_INPUT(TRACKPAD_FORCE, TRACKPAD_FORCE);

		SET_INDEX_INPUT(AIM_POSE, AIM_POSE);
		SET_INDEX_INPUT(GRIP_POSE, GRIP_POSE);

		d->base.outputs[0].name = XRT_OUTPUT_NAME_INDEX_HAPTIC;

		d->base.update_inputs = vive_controller_device_index_update_inputs;

		d->base.get_hand_tracking = vive_controller_get_hand_tracking;

		d->base.binding_profiles = vive_binding_profiles_index;
		d->base.binding_profile_count = vive_binding_profiles_index_count;

		if (d->config.variant == CONTROLLER_INDEX_LEFT) {
			d->base.device_type = XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
			d->base.inputs[VIVE_CONTROLLER_HAND_TRACKING].name = XRT_INPUT_GENERIC_HAND_TRACKING_LEFT;
			snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Valve Index Left Controller (vive)");
		} else if (d->config.variant == CONTROLLER_INDEX_RIGHT) {
			d->base.device_type = XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
			d->base.inputs[VIVE_CONTROLLER_HAND_TRACKING].name = XRT_INPUT_GENERIC_HAND_TRACKING_RIGHT;
			snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Valve Index Right Controller (vive)");
		}
	} else if (d->config.variant == CONTROLLER_TRACKER_GEN1) {
		d->base.name = XRT_DEVICE_VIVE_TRACKER_GEN1;
		d->base.update_inputs = u_device_noop_update_inputs;
		d->base.device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Vive Tracker Gen1 (vive)");
	} else if (d->config.variant == CONTROLLER_TRACKER_GEN2) {
		d->base.name = XRT_DEVICE_VIVE_TRACKER_GEN2;
		d->base.update_inputs = u_device_noop_update_inputs;
		d->base.device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Vive Tracker Gen2 (vive)");
	} else if (d->config.variant == CONTROLLER_TRACKER_GEN3) {
		d->base.name = XRT_DEVICE_VIVE_TRACKER_GEN3;
		d->base.update_inputs = u_device_noop_update_inputs;
		d->base.device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Vive Tracker Gen3 (vive)");
	} else if (d->config.variant == CONTROLLER_TRACKER_TUNDRA) {
		d->base.name = XRT_DEVICE_VIVE_TRACKER_TUNDRA;
		d->base.update_inputs = u_device_noop_update_inputs;
		d->base.device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		snprintf(d->base.str, XRT_DEVICE_NAME_LEN, "Tundra Tracker Gen3 (vive)");
	} else {
		d->base.name = XRT_DEVICE_GENERIC_HMD;
		d->base.device_type = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		VIVE_ERROR(d, "Failed to assign update input function");
	}

	if (d->controller_hid) {
		int ret = os_thread_helper_start(&d->controller_thread, vive_controller_run_thread, d);
		if (ret != 0) {
			VIVE_ERROR(d, "Failed to start mainboard thread!");
			vive_controller_device_destroy(&d->base);
			return NULL;
		}
	}

	VIVE_DEBUG(d, "Opened vive controller!\n");
	d->base.orientation_tracking_supported = true;
	d->base.position_tracking_supported = false;
	d->base.hand_tracking_supported =
	    d->config.variant == CONTROLLER_INDEX_LEFT || d->config.variant == CONTROLLER_INDEX_RIGHT;

	vive_controller_setup_ui(d);

	return d;
}
