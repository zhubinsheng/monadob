// Copyright 2023, Tobias Frisch
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Nreal Air packet parsing implementation.
 * @author Tobias Frisch <thejackimonster@gmail.com>
 * @ingroup drv_na
 */

#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"

#include "os/os_hid.h"

#include "util/u_logging.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NA_HANDLE_IFACE 3
#define NA_CONTROL_IFACE 4

#define NA_MSG_R_BRIGHTNESS 0x03
#define NA_MSG_W_BRIGHTNESS 0x04
#define NA_MSG_R_DISP_MODE 0x07
#define NA_MSG_W_DISP_MODE 0x08

#define NA_MSG_P_START_HEARTBEAT 0x6c02
#define NA_MSG_P_BUTTON_PRESSED 0x6C05
#define NA_MSG_P_END_HEARTBEAT 0x6c12
#define NA_MSG_P_ASYNC_TEXT_LOG 0x6c09

#define NA_BUTTON_PHYS_DISPLAY_TOGGLE 0x1
#define NA_BUTTON_PHYS_BRIGHTNESS_UP 0x2
#define NA_BUTTON_PHYS_BRIGHTNESS_DOWN 0x3

#define NA_BUTTON_VIRT_DISPLAY_TOGGLE 0x1
#define NA_BUTTON_VIRT_MENU_TOGGLE 0x3
#define NA_BUTTON_VIRT_BRIGHTNESS_UP 0x6
#define NA_BUTTON_VIRT_BRIGHTNESS_DOWN 0x7
#define NA_BUTTON_VIRT_MODE_UP 0x8
#define NA_BUTTON_VIRT_MODE_DOWN 0x9

#define NA_BRIGHTNESS_MIN 0
#define NA_BRIGHTNESS_MAX 7

#define NA_DISPLAY_MODE_2D 0x1
#define NA_DISPLAY_MODE_3D 0x3

#define NA_TICKS_PER_SECOND (1000.0) // 1 KHz ticks
#define NA_NS_PER_TICK (1000000)     // Each tick is a millisecond

#define NA_MSG_GET_CAL_DATA_LENGTH 0x14
#define NA_MSG_CAL_DATA_GET_NEXT_SEGMENT 0x15
#define NA_MSG_ALLOCATE_CAL_DATA_BUFFER 0x16
#define NA_MSG_WRITE_CAL_DATA_SEGMENT 0x17
#define NA_MSG_FREE_CAL_BUFFER 0x18
#define NA_MSG_START_IMU_DATA 0x19
#define NA_MSG_GET_STATIC_ID 0x1A
#define NA_MSG_UNKNOWN 0x1D

struct na_parsed_calibration
{
	struct xrt_vec3 accel_bias;
	struct xrt_quat accel_q_gyro;
	struct xrt_vec3 gyro_bias;
	struct xrt_quat gyro_q_mag;
	struct xrt_vec3 mag_bias;

	struct xrt_vec3 scale_accel;
	struct xrt_vec3 scale_gyro;
	struct xrt_vec3 scale_mag;

	float imu_noises[4];
};

/*!
 * A parsed single gyroscope, accelerometer and
 * magnetometer sample with their corresponding
 * factors for conversion from raw data.
 *
 * @ingroup drv_na
 */
struct na_parsed_sample
{
	struct xrt_vec3_i32 accel;
	struct xrt_vec3_i32 gyro;
	struct xrt_vec3_i32 mag;

	int16_t accel_multiplier;
	int16_t gyro_multiplier;
	int16_t mag_multiplier;

	int32_t accel_divisor;
	int32_t gyro_divisor;
	int32_t mag_divisor;
};

/*!
 * Over the wire sensor packet from the glasses.
 *
 * @ingroup drv_na
 */
struct na_parsed_sensor
{
	int16_t temperature;
	uint64_t timestamp;

	struct na_parsed_sample sample;
};

/*!
 * Over the wire sensor control data packet from the glasses.
 *
 * @ingroup drv_na
 */
struct na_parsed_sensor_control_data
{
	uint16_t length;
	uint8_t msgid;

	uint8_t data[56];
};

/*!
 * A control packet from the glasses in wire format.
 *
 * @ingroup drv_na
 */
struct na_parsed_control
{
	uint16_t length;
	uint64_t timestamp;
	uint16_t action;

	uint8_t data[42];
};

/*!
 * Create Nreal Air glasses.
 *
 * @ingroup drv_na
 */
struct xrt_device *
na_hmd_create_device(struct os_hid_device *sensor_device,
                     struct os_hid_device *control_device,
                     enum u_logging_level log_level);

bool
na_parse_calibration_buffer(struct na_parsed_calibration *calibration, const char *buffer, size_t size);

bool
na_parse_sensor_packet(struct na_parsed_sensor *sensor, const uint8_t *buffer, int size);

bool
na_parse_sensor_control_data_packet(struct na_parsed_sensor_control_data *data, const uint8_t *buffer, int size);

bool
na_parse_control_packet(struct na_parsed_control *control, const uint8_t *buffer, int size);

#ifdef __cplusplus
}
#endif
