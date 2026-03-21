#ifndef NVSTORAGE_H_
#define NVSTORAGE_H_

// NV storage is backed by I2C EEPROM. The current structure supports
// up to 256 bytes of total state, providing ample room for configuration.
// Only append to the end of this list to avoid backwards incompatibilities
typedef enum eNVItem {
	REFLOW_BEEP_DONE_LEN=0,
	REFLOW_PROFILE=1,
	TC_LEFT_GAIN,
	TC_LEFT_OFFSET,
	TC_RIGHT_GAIN,
	TC_RIGHT_OFFSET,
	REFLOW_MIN_FAN_SPEED,
	REFLOW_BAKE_SETPOINT_H,
	REFLOW_BAKE_SETPOINT_L,
	REFLOW_PREHEAT_TEMP,
	REFLOW_BANGBANG_MODE,
	REFLOW_BB_HEAT_OFFSET,
	REFLOW_BB_COOL_OFFSET,

	PID_TUNE_KP,	// Stored as value * 2, so 20.0 = 40
	PID_TUNE_KI,	// Stored as value * 500, so 0.016 = 8
	PID_TUNE_KD,	// Stored as value * 2, so 62.5 = 125

	SCREENSAVER_ACTIVE,

	OP_MODE,
	MODE_THRESH,

	SAFETY_RUNAWAY_THRESH,	// Max °C above setpoint before abort (0=disabled, 1-50)
	REFLOW_BUZZER_ALERTS,	// Audible alerts at stage transitions (0=off, 1=on)
	REFLOW_MAX_COOL_RATE,	// Max cooling rate °C/10s (0=unlimited, 1-50)

	TC_LEFT_OFFSET_HI,		// High-temp offset at 200°C (same encoding as TC_LEFT_OFFSET)
	TC_RIGHT_OFFSET_HI,	// High-temp offset at 200°C (same encoding as TC_RIGHT_OFFSET)

	TEMP_UNIT_FAHRENHEIT,	// 0=Celsius, 1=Fahrenheit display
	REFLOW_FAN_KICKSTART,	// 0=OFF, 1=ON

	NVITEM_NUM_ITEMS // Last value
} NVItem_t;

void NV_Init(void);
uint8_t NV_GetConfig(NVItem_t item);
void NV_SetConfig(NVItem_t item, uint8_t value);
int32_t NV_Work(void);

#endif /* NVSTORAGE_H_ */
