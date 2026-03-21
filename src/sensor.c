
#include "LPC214x.h"
#include <stdint.h>
#include <stdio.h>
#include "adc.h"
#include "t962.h"
#include "onewire.h"
#include "max31855.h"
#include "nvstorage.h"

#include "sensor.h"

/*
* Normally the control input is the average of the first two TCs.
* By defining this any TC that has a readout 5C (or more) higher
* than the TC0 and TC1 average will be used as control input instead.
* Use if you have very sensitive components. Note that this will also
* kick in if the two sides of the oven has different readouts, as the
* code treats all four TCs the same way.
*/
//#define MAXTEMPOVERRIDE

// Operational Mode
static OperationMode_t opMode = AMBIENT;
static uint8_t opModeTempThresh = 5;
// Gain adjust, this may have to be calibrated per device if factory trimmer adjustments are off
static float adcgainadj[2];
static float adcoffsetadj[2]; // Ambient (low-temp) offset
static float adcoffsetadj_hi[2]; // High-temp offset at 200°C

// Two-point interpolation: linearly blend between ambient offset and hi-temp offset
// ambient ref = 25°C, hi ref = 200°C
static float Sensor_InterpolateOffset(int ch, float rawtemp) {
	if (adcoffsetadj_hi[ch] == adcoffsetadj[ch]) {
		return adcoffsetadj[ch]; // Same offset, no interpolation needed
	}
	if (rawtemp <= 25.0f) return adcoffsetadj[ch];
	if (rawtemp >= 200.0f) return adcoffsetadj_hi[ch];
	float frac = (rawtemp - 25.0f) / 175.0f;
	return adcoffsetadj[ch] + (adcoffsetadj_hi[ch] - adcoffsetadj[ch]) * frac;
}

static float temperature[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static uint8_t tempvalid = 0;
static uint8_t cjsensorpresent = 0;

// The feedback temperature
static float avgtemp;
static float coldjunction;

OperationMode_t Sensor_getOpMode() {
	return opMode;
}

void Sensor_printOpMode() {
	const char* modes[] = { "AMBIENT", "MAXTEMPOVERRIDE", "SPLIT" };
	//printf("\nCurrent Operational Mode: %s\n", modes[(uint8_t)Sensor_getOpMode()]);
	printf("%s", modes[(uint8_t)Sensor_getOpMode()]);
}

void Sensor_setOpMode(OperationMode_t newmode) {
	opMode = newmode;
	NV_SetConfig(OP_MODE, (uint8_t)newmode);
}

uint8_t Sensor_getOpModeThreshold()
{
	return opModeTempThresh;
}

void Sensor_setOpModeThreshold(uint8_t threshold)
{
	opModeTempThresh = threshold;
	NV_SetConfig(MODE_THRESH, threshold);
}

void Sensor_ValidateNV(void) {
	int temp;

	temp = NV_GetConfig(TC_LEFT_GAIN);
	if (temp == 255) {
		temp = 100;
		NV_SetConfig(TC_LEFT_GAIN, temp); // Default unity gain
	}
	adcgainadj[0] = ((float)temp) * 0.01f;

	temp = NV_GetConfig(TC_RIGHT_GAIN);
	if (temp == 255) {
		temp = 100;
		NV_SetConfig(TC_RIGHT_GAIN, temp); // Default unity gain
	}
	adcgainadj[1] = ((float)temp) * 0.01f;

	temp = NV_GetConfig(TC_LEFT_OFFSET);
	if (temp == 255) {
		temp = 100;
		NV_SetConfig(TC_LEFT_OFFSET, temp); // Default +/-0 offset
	}
	adcoffsetadj[0] = ((float)(temp - 127)) * 0.5f;

	temp = NV_GetConfig(TC_RIGHT_OFFSET);
	if (temp == 255) {
		temp = 100;
		NV_SetConfig(TC_RIGHT_OFFSET, temp); // Default +/-0 offset
	}
	adcoffsetadj[1] = ((float)(temp - 127)) * 0.5f;

	// High-temp offsets (default same as ambient = no two-point correction)
	temp = NV_GetConfig(TC_LEFT_OFFSET_HI);
	if (temp == 255) {
		temp = NV_GetConfig(TC_LEFT_OFFSET); // Default: same as ambient
	}
	adcoffsetadj_hi[0] = ((float)(temp - 127)) * 0.5f;

	temp = NV_GetConfig(TC_RIGHT_OFFSET_HI);
	if (temp == 255) {
		temp = NV_GetConfig(TC_RIGHT_OFFSET); // Default: same as ambient
	}
	adcoffsetadj_hi[1] = ((float)(temp - 127)) * 0.5f;

	temp = NV_GetConfig(OP_MODE);
	opMode = (OperationMode_t)temp;

	temp = NV_GetConfig(MODE_THRESH);
	opModeTempThresh = temp;
}


void Sensor_DoConversion(void) {
	uint16_t temp[2];
	/*
	* These are the temperature readings we get from the thermocouple interfaces
	* Right now it is assumed that if they are indeed present the first two
	* channels will be used as feedback
	*/
	float tctemp[4], tccj[4];
	uint8_t tcpresent[4];
	tempvalid = 0; // Assume no valid readings;
	for (int i = 0; i < 4; i++) { // Get 4 TC channels
		tcpresent[i] = OneWire_IsTCPresent(i);
		if (tcpresent[i]) {
			tctemp[i] = OneWire_GetTCReading(i);
			tccj[i] = OneWire_GetTCColdReading(i);
			if (i > 1) {
				temperature[i] = tctemp[i];
				tempvalid |= (1 << i);
			}
		}
		else {
			tcpresent[i] = SPI_IsTCPresent(i);
			if (tcpresent[i]) {
				tctemp[i] = SPI_GetTCReading(i);
				tccj[i] = SPI_GetTCColdReading(i);
				if (i > 1) {
					temperature[i] = tctemp[i];
					tempvalid |= (1 << i);
				}
			}
		}
	}

	// Assume no CJ sensor
	cjsensorpresent = 0;
	if (tcpresent[0] && tcpresent[1]) {
		// Adjust values with calibration settings (two-point interpolation)
		float off0 = Sensor_InterpolateOffset(0, tctemp[0]);
		float off1 = Sensor_InterpolateOffset(1, tctemp[1]);
		float t0 = tctemp[0] * adcgainadj[0]  + off0;
		float t1 = tctemp[1] * adcgainadj[1]  + off1;
		avgtemp = (t0 + t1) / 2.0f;
		temperature[0] = t0;
		temperature[1] = t1;
		tempvalid |= 0x03;
		coldjunction = (tccj[0] + tccj[1]) / 2.0f;
		cjsensorpresent = 1;
	} else if (tcpresent[2] && tcpresent[3]) {
		// Adjust values with calibration settings (two-point interpolation)
		float off0 = Sensor_InterpolateOffset(0, tctemp[2]);
		float off1 = Sensor_InterpolateOffset(1, tctemp[3]);
		float t0 = tctemp[2] * adcgainadj[0]  + off0;
		float t1 = tctemp[3] * adcgainadj[1]  + off1;
		avgtemp = (t0 + t1) / 2.0f;
		temperature[0] = t0;
		temperature[1] = t1;
		tempvalid |= 0x03;
		tempvalid &= ~0x0C;
		coldjunction = (tccj[2] + tccj[3]) / 2.0f;
		cjsensorpresent = 1;
	} else {
		// If the external TC interface is not present we fall back to the
		// built-in ADC, with or without compensation
		coldjunction = OneWire_GetTempSensorReading();
		if (coldjunction < 127.0f) {
			cjsensorpresent = 1;
		} else {
			coldjunction = 25.0f; // Assume 25C ambient if not found
		}
		temp[0] = ADC_Read(1);
		temp[1] = ADC_Read(2);

		// ADC oversamples to supply 4 additional bits of resolution
		temperature[0] = ((float)temp[0]) / 16.0f;
		temperature[1] = ((float)temp[1]) / 16.0f;

		// Gain adjust
		temperature[0] *= adcgainadj[0];
		temperature[1] *= adcgainadj[1];

		// Offset adjust (two-point interpolation)
		temperature[0] += coldjunction + Sensor_InterpolateOffset(0, temperature[0]);
		temperature[1] += coldjunction + Sensor_InterpolateOffset(1, temperature[1]);

		tempvalid |= 0x03;

		avgtemp = (temperature[0] + temperature[1]) / 2.0f;
	}

	// if the mode is not AMBIENT, we override avgtemp based on OpMode
	switch (opMode) {
		case MAXTEMPOVERRIDE: {
			// If one of the temperature sensors reports higher than opModeTempThresh above
			// the average, use that as control input
			float newtemp = avgtemp;
			for (int i = 0; i < 4; i++) {
				if (tcpresent[i] && temperature[i] > (avgtemp + (float)opModeTempThresh) && temperature[i] > newtemp) {
					newtemp = temperature[i];
				}
			}
			if (avgtemp != newtemp) {
				avgtemp = newtemp;
			}
			break;
		}
		case SPLIT: {
			//Override avgtemp to board temp if > opModeTempThresh
			if (avgtemp > opModeTempThresh) {
				if (tcpresent[2] && tcpresent[3]) {
					avgtemp = (tctemp[2] + tctemp[3]) / 2.0f;
				}
				else if (tcpresent[2]) {
					avgtemp = tctemp[2];
				}
				else if (tcpresent[3]) {
					avgtemp = tctemp[3];
				}
			}
			break;
		}
		case AMBIENT: {
			//default
		}
	}
}

uint8_t Sensor_ColdjunctionPresent(void) {
	return cjsensorpresent;
}


float Sensor_GetTemp(TempSensor_t sensor) {
	if (sensor == TC_COLD_JUNCTION) {
		return coldjunction;
	} else if(sensor == TC_AVERAGE) {
		return avgtemp;
	} else if(sensor < TC_NUM_ITEMS) {
		return temperature[sensor - TC_LEFT];
	} else {
		return 0.0f;
	}
}

uint8_t Sensor_IsValid(TempSensor_t sensor) {
	if (sensor == TC_COLD_JUNCTION) {
		return cjsensorpresent;
	} else if(sensor == TC_AVERAGE) {
		return 1;
	} else if(sensor >= TC_NUM_ITEMS) {
		return 0;
	}
	return (tempvalid & (1 << (sensor - TC_LEFT))) ? 1 : 0;
}

void Sensor_ListAll(void) {
	int count = 5;
	char* names[] = {"Left", "Right", "Extra 1", "Extra 2", "Cold junction"};
	TempSensor_t sensors[] = {TC_LEFT, TC_RIGHT, TC_EXTRA1, TC_EXTRA2, TC_COLD_JUNCTION};
	char* format = "\n%13s: %4.1fdegC";

	for (int i = 0; i < count; i++) {
		if (Sensor_IsValid(sensors[i])) {
			printf(format, names[i], Sensor_GetTemp(sensors[i]));
		}
	}
	if (!Sensor_IsValid(TC_COLD_JUNCTION)) {
		printf("\nNo cold-junction sensor on PCB");
	}
}

// ============================================================================
// TC Offset Auto-Calibration
// At cold ambient, the cold junction sensor (DS18B20) is used as the reference.
// Both TCs should read the same temperature. Any difference is offset error.
// ============================================================================

static float cal_error[2] = { 0.0f, 0.0f };

float Sensor_GetCalError(int channel) {
	if (channel >= 0 && channel < 2) return cal_error[channel];
	return 0.0f;
}

int Sensor_AutoCalibrate(void) {
	// Require cold junction sensor
	if (!cjsensorpresent) {
		printf("\nTC Cal: No cold junction sensor present");
		return 2;
	}

	// Read current temperatures
	Sensor_DoConversion();
	float ref = Sensor_GetTemp(TC_COLD_JUNCTION);
	float left = Sensor_GetTemp(TC_LEFT);
	float right = Sensor_GetTemp(TC_RIGHT);

	// Check oven is cold enough
	if (ref > TCCAL_MAX_TEMP || left > TCCAL_MAX_TEMP || right > TCCAL_MAX_TEMP) {
		printf("\nTC Cal: Oven too hot (%.1f/%.1f/%.1fC, max %.0fC)",
		       left, right, ref, TCCAL_MAX_TEMP);
		return 1;
	}

	// Calculate errors (positive = TC reads too high)
	cal_error[0] = left - ref;
	cal_error[1] = right - ref;

	// Current NV offset values
	int nv_left = NV_GetConfig(TC_LEFT_OFFSET);
	int nv_right = NV_GetConfig(TC_RIGHT_OFFSET);

	// Offset encoding: actual = (nv - 127) * 0.5
	// To reduce reading by `error`: new_nv = old_nv - (error / 0.5)
	int new_left = nv_left - (int)(cal_error[0] * 2.0f + 0.5f);
	int new_right = nv_right - (int)(cal_error[1] * 2.0f + 0.5f);

	// Clamp to valid NV range (0-254, 255 is reserved for uninitialised)
	if (new_left < 0) new_left = 0;
	if (new_left > 254) new_left = 254;
	if (new_right < 0) new_right = 0;
	if (new_right > 254) new_right = 254;

	NV_SetConfig(TC_LEFT_OFFSET, (uint8_t)new_left);
	NV_SetConfig(TC_RIGHT_OFFSET, (uint8_t)new_right);

	// Reload calibration values
	adcoffsetadj[0] = ((float)(new_left - 127)) * 0.5f;
	adcoffsetadj[1] = ((float)(new_right - 127)) * 0.5f;

	printf("\nTC Cal: ref=%.1fC L=%.1f(err=%+.1f) R=%.1f(err=%+.1f)",
	       ref, left, cal_error[0], right, cal_error[1]);
	printf("\nTC Cal: NV offset L=%d->%d R=%d->%d",
	       nv_left, new_left, nv_right, new_right);

	return 0;
}
