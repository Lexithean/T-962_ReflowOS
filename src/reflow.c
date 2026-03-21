/*
 * reflow.c - Actual reflow profile logic for T-962 reflow controller
 *
 * Copyright (C) 2014 Werner Johansson, wj@unifiedengineering.se
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "LPC214x.h"
#include <stdint.h>
#include <stdio.h>
#include "t962.h"
#include "reflow_profiles.h"
#include "io.h"
#include "lcd.h"
#include "rtc.h"
#include "PID_v1.h"
#include "sched.h"
#include "nvstorage.h"
#include "sensor.h"
#include "reflow.h"

// Standby temperature in degrees Celsius
#define STANDBYTEMP (50)

// 250ms between each run
#define PID_TIMEBASE (250)

#define TICKS_PER_SECOND (1000 / PID_TIMEBASE)

static PidType PID;

static uint16_t intsetpoint;
static int bake_timer = 0;

static float avgtemp;

static uint8_t reflowdone = 0;
static ReflowMode_t mymode = REFLOW_STANDBY;
static uint16_t numticks = 0;

static int standby_logging = 0;

uint8_t plotDot[TOTAL_DOTS];
static int reflowPaused=0;

static int32_t Reflow_Work(void) {
	static ReflowMode_t oldmode = REFLOW_INITIAL;
	static uint32_t lasttick = 0;
	uint8_t fan, heat;
	uint32_t ticks = RTC_Read();

	Sensor_DoConversion();
	avgtemp = Sensor_GetTemp(TC_AVERAGE);

	const char* modestr = "UNKNOWN";

	// Depending on mode we should run this with different parameters
	if (mymode == REFLOW_STANDBY || mymode == REFLOW_STANDBYFAN) {
		intsetpoint = STANDBYTEMP;
		// Cool to standby temp but don't heat to get there
		Reflow_Run(0, avgtemp, &heat, &fan, intsetpoint);
		heat = 0;

		// Suppress slow-running fan in standby
		if (mymode == REFLOW_STANDBY && avgtemp < (float)STANDBYTEMP) {
			 fan = 0;
		}
		modestr = "STANDBY";

	} else if(mymode == REFLOW_BAKE) {
		reflowdone = Reflow_Run(0, avgtemp, &heat, &fan, intsetpoint) ? 1 : 0;
		modestr = "BAKE";

	} else if(mymode == REFLOW_REFLOW) {
		int preheat_temp = NV_GetConfig(REFLOW_PREHEAT_TEMP) + 30;
		if (preheat_temp > 30 && avgtemp < (float)preheat_temp) {
			// Preheat phase: heat to user-configured temp before starting the profile
			Reflow_Run(0, avgtemp, &heat, &fan, preheat_temp);
			modestr = "PREHEAT";
			// Don't increment ticks - profile timer hasn't started
		} else {
			reflowdone = Reflow_Run(ticks, avgtemp, &heat, &fan, 0) ? 1 : 0;
			modestr = "REFLOW";
		}

	} else {
		heat = fan = 0;
	}
	Set_Heater(heat);
	Set_Fan(fan);

	if (mymode != oldmode) {
		printf("\n# Time,  Temp0, Temp1, Temp2, Temp3,  Set,Actual, Heat, Fan,  ColdJ, Mode");
		oldmode = mymode;
		numticks = 0;
	} else if (mymode == REFLOW_BAKE) {
		if (bake_timer > 0 && numticks >= bake_timer) {
			printf("\n DONE baking, set bake timer to 0.");
			bake_timer = 0;
			Reflow_SetMode(REFLOW_STANDBY);
		}

		// start increasing ticks after setpoint is reached...
		if (avgtemp < intsetpoint && bake_timer > 0) {
			modestr = "BAKE-PREHEAT";
		} else {
			numticks++;
		}
	} else if (mymode == REFLOW_REFLOW) {
		numticks++;
	}

	if (!(mymode == REFLOW_STANDBY && standby_logging == 0)) {
		printf("\n%6.1f,  %5.1f, %5.1f, %5.1f, %5.1f,  %3u, %5.1f,  %3u, %3u,  %5.1f, %s",
		       ((float)numticks / TICKS_PER_SECOND),
		       Sensor_GetTemp(TC_LEFT),
		       Sensor_GetTemp(TC_RIGHT),
		       Sensor_GetTemp(TC_EXTRA1),
		       Sensor_GetTemp(TC_EXTRA2),
		       intsetpoint, avgtemp,
		       heat, fan,
		       Sensor_GetTemp(TC_COLD_JUNCTION),
		       modestr);
	}

	if (numticks & 1) {
		// Force UI refresh every other cycle
		Sched_SetState(MAIN_WORK, 2, 0);
	}

	uint32_t thistick = Sched_GetTick();
	if (lasttick == 0) {
		lasttick = thistick - TICKS_MS(PID_TIMEBASE);
	}

	int32_t nexttick = (2 * TICKS_MS(PID_TIMEBASE)) - (thistick - lasttick);
	if ((thistick - lasttick) > (2 * TICKS_MS(PID_TIMEBASE))) {
		printf("\nReflow can't keep up with desired PID_TIMEBASE!");
		nexttick = 0;
	}
	lasttick += TICKS_MS(PID_TIMEBASE);
	return nexttick;
}

void Reflow_Init(void) {
	Sched_SetWorkfunc(REFLOW_WORK, Reflow_Work);
	//PID_init(&PID, 10, 0.04, 5, PID_Direction_Direct); // This does not reach the setpoint fast enough
	//PID_init(&PID, 30, 0.2, 5, PID_Direction_Direct); // This reaches the setpoint but oscillates a bit especially during cooling
	//PID_init(&PID, 30, 0.2, 15, PID_Direction_Direct); // This overshoots the setpoint
	//PID_init(&PID, 25, 0.15, 15, PID_Direction_Direct); // This overshoots the setpoint slightly
	//PID_init(&PID, 20, 0.07, 25, PID_Direction_Direct);
	//PID_init(&PID, 20, 0.04, 25, PID_Direction_Direct); // Improvement as far as I can tell, still work in progress
	PID_init(&PID, 0, 0, 0, PID_Direction_Direct); // Can't supply tuning to PID_Init when not using the default timebase
	PID_SetSampleTime(&PID, PID_TIMEBASE);
	PID_SetTunings(&PID, 20, 0.016, 62.5); // Adjusted values to compensate for the incorrect timebase earlier
	//PID_SetTunings(&PID, 80, 0, 0); // This results in oscillations with 14.5s cycle time
	//PID_SetTunings(&PID, 30, 0, 0); // This results in oscillations with 14.5s cycle time
	//PID_SetTunings(&PID, 15, 0, 0);
	//PID_SetTunings(&PID, 10, 0, 0); // no oscillations, but offset
	//PID_SetTunings(&PID, 10, 0.020, 0); // getting there
	//PID_SetTunings(&PID, 10, 0.013, 0);
	//PID_SetTunings(&PID, 10, 0.0066, 0);
	//PID_SetTunings(&PID, 10, 0.2, 0);
	//PID_SetTunings(&PID, 10, 0.020, 1.0); // Experimental

	Reflow_LoadCustomProfiles();

	Reflow_ValidateNV();
	Sensor_ValidateNV();

	Reflow_LoadSetpoint();

	PID.mySetpoint = (float)SETPOINT_DEFAULT;
	PID_SetOutputLimits(&PID, 0, 255 + 248);
	PID_SetMode(&PID, PID_Mode_Manual);
	PID.myOutput = 248; // Between fan and heat
	PID_SetMode(&PID, PID_Mode_Automatic);
	RTC_Zero();	// reset RTC

	// clear plotted dots
	for(uint8_t n=0;n<TOTAL_DOTS;n++){
		plotDot[n]=0;
	}
	reflowPaused=0;

	// Start work
	Sched_SetState(REFLOW_WORK, 2, 0);
}

void Reflow_TogglePause(void){
	reflowPaused=(reflowPaused==1?0:1);
	if(reflowPaused){
		RTC_Hold();
	}else{
		RTC_Resume();
	}
}

int Reflow_IsPaused(void){
	return reflowPaused;
}

void Reflow_SetMode(ReflowMode_t themode) {
	mymode = themode;
	// reset reflowdone if mode is set to standby.
	if (themode == REFLOW_STANDBY)  {
		reflowdone = 0;
	}
}

void Reflow_SetSetpoint(uint16_t thesetpoint) {
	intsetpoint = thesetpoint;

	NV_SetConfig(REFLOW_BAKE_SETPOINT_H, (uint8_t)(thesetpoint >> 8));
	NV_SetConfig(REFLOW_BAKE_SETPOINT_L, (uint8_t)thesetpoint);
}

void Reflow_LoadSetpoint(void) {
	intsetpoint = NV_GetConfig(REFLOW_BAKE_SETPOINT_H) << 8;
	intsetpoint |= NV_GetConfig(REFLOW_BAKE_SETPOINT_L);

	printf("\n bake setpoint values: %x, %x, %d\n",
		NV_GetConfig(REFLOW_BAKE_SETPOINT_H),
		NV_GetConfig(REFLOW_BAKE_SETPOINT_L), intsetpoint);
}

int16_t Reflow_GetActualTemp(void) {
	return (int)Sensor_GetTemp(TC_AVERAGE);
}

uint8_t Reflow_IsDone(void) {
	return reflowdone;
}

uint16_t Reflow_GetSetpoint(void) {
	return intsetpoint;
}

void Reflow_SetBakeTimer(int seconds) {
	// reset ticks to 0 when adjusting timer.
	numticks = 0;
	bake_timer = seconds * TICKS_PER_SECOND;
}

int Reflow_IsPreheating(void) {
	return bake_timer > 0 && avgtemp < intsetpoint;
}

int Reflow_GetTimeLeft(void) {
	if (bake_timer == 0) {
		return -1;
	}
	return (bake_timer - numticks) / TICKS_PER_SECOND;
}

// returns -1 if the reflow process is done.
int32_t Reflow_Run(uint32_t thetime, float meastemp, uint8_t* pheat, uint8_t* pfan, int32_t manualsetpoint) {
	int32_t retval = 0;

	if (manualsetpoint) {
		PID.mySetpoint = (float)manualsetpoint;

		if (bake_timer > 0 && (Reflow_GetTimeLeft() == 0 || Reflow_GetTimeLeft() == -1)) {
			retval = -1;
		}
	} else {
		// Figure out what setpoint to use from the profile, brute-force way. Fix this.
		uint8_t idx = thetime / 10;
		uint16_t start = idx * 10;
		uint16_t offset = thetime - start;
		if (idx < (NUMPROFILETEMPS - 2)) {
			uint16_t value = Reflow_GetSetpointAtIdx(idx);
			uint16_t value2 = Reflow_GetSetpointAtIdx(idx + 1);

			// If current set temp and next are zero, then profile is over
			if (value > 0 && value2 > 0) {
				uint16_t avg = (value * (10 - offset) + value2 * offset) / 10;

				// Keep the setpoint for the UI...
				intsetpoint = avg;
				if (value2 > avg) {
					// Temperature is rising,
					// using the future value for PID regulation produces better result when heating
					PID.mySetpoint = (float)value2;
				} else {
					// Use the interpolated value when cooling
					PID.mySetpoint = (float)avg;
				}
			} else {
				retval = -1;
			}
		} else {
			retval = -1;
		}
	}

	if (!manualsetpoint) {
		// Plot actual temperature on top of desired profile
		int realx = (thetime / 5) + XAXIS;
		int y = (uint16_t)(meastemp * 0.2f);
		y = YAXIS - y;
		plotDot[realx]=y;
	}

	if (NV_GetConfig(REFLOW_BANGBANG_MODE)) {
		// Bang-bang control for T-962C: IR heaters perform poorly when pulsed
		// via PWM. Drive at 100% or 0% and let thermal mass smooth the response.
		// Anticipatory offsets reduce overshoot/undershoot — use BB Tune to calibrate.
		float heat_off = (float)NV_GetConfig(REFLOW_BB_HEAT_OFFSET);
		float cool_off = (float)NV_GetConfig(REFLOW_BB_COOL_OFFSET);
		if (heat_off >= 255) heat_off = 0; // Uninitialised NV
		if (cool_off >= 255) cool_off = 0;

		if (meastemp < (PID.mySetpoint - heat_off)) {
			// Below target minus offset: full heat
			*pheat = 255;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
		} else if (meastemp > (PID.mySetpoint + cool_off)) {
			// Above target plus offset: full fan cooling
			*pheat = 0;
			*pfan = 255;
		} else {
			// Dead zone: coast (no heat, minimum fan)
			*pheat = 0;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
		}
	} else {
		// Original PID control
		PID.myInput = meastemp;
		PID_Compute(&PID);
		uint32_t out = PID.myOutput;
		if (out < 248) { // Fan in reverse
			*pfan = 255 - out;
			*pheat = 0;
		} else {
			*pheat = out - 248;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
		}
	}
	return retval;
}

void Reflow_ToggleStandbyLogging(void) {
	standby_logging = !standby_logging;
}

// plot current reflow data on graph
void Reflow_PlotDots(void){
	LCD_FB_Clear();

	Reflow_PlotProfile(-1);	// draws graph and plots profile
	for(uint8_t n=XAXIS;n<TOTAL_DOTS;n++){
		if(plotDot[n]>0){
			LCD_SetPixel(n,plotDot[n]);
		}
	}

}

// ============================================================================
// Bang-Bang Auto-Tune
// Measures thermal overshoot (heating) and undershoot (cooling) over multiple
// cycles, averaging the results for a reliable anticipatory offset.
// ============================================================================

static BBTunePhase_t bbtune_phase = BBTUNE_PROMPT;
static int bbtune_cycle = 0;
static float bbtune_peak = 0;
static float bbtune_trough = 999;
static float bbtune_prev_temp = 0;
static int bbtune_heat_sum = 0;
static int bbtune_cool_sum = 0;
static int bbtune_heat_result = 0;
static int bbtune_cool_result = 0;

void Reflow_BBTune_Start(void) {
	bbtune_phase = BBTUNE_HEATING;
	bbtune_cycle = 0;
	bbtune_peak = 0;
	bbtune_trough = 999;
	bbtune_prev_temp = 0;
	bbtune_heat_sum = 0;
	bbtune_cool_sum = 0;
	bbtune_heat_result = 0;
	bbtune_cool_result = 0;
	Reflow_SetMode(REFLOW_BBTUNE);
}

void Reflow_BBTune_Stop(void) {
	bbtune_phase = BBTUNE_PROMPT;
	Reflow_SetMode(REFLOW_STANDBY);
}

BBTunePhase_t Reflow_BBTune_GetPhase(void) {
	return bbtune_phase;
}

int Reflow_BBTune_GetCycle(void) {
	return bbtune_cycle;
}

int Reflow_BBTune_GetHeatOffset(void) {
	return bbtune_heat_result;
}

int Reflow_BBTune_GetCoolOffset(void) {
	return bbtune_cool_result;
}

int32_t Reflow_BBTune_Work(uint8_t* pheat, uint8_t* pfan) {
	Sensor_DoConversion();
	float temp = Sensor_GetTemp(TC_AVERAGE);

	switch (bbtune_phase) {
		case BBTUNE_HEATING:
			// Full heat until we reach the high target
			*pheat = 255;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
			if (temp >= (float)BBTUNE_TARGET_HIGH) {
				// Reached target — switch to coast and track peak
				bbtune_phase = BBTUNE_HEAT_COAST;
				bbtune_peak = temp;
				bbtune_prev_temp = temp;
			}
			break;

		case BBTUNE_HEAT_COAST:
			// Heaters off, min fan — wait for temp to peak and start falling
			*pheat = 0;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
			if (temp > bbtune_peak) {
				bbtune_peak = temp;
			}
			// Detect falling: temp dropped 2°C below peak (filters noise)
			if (temp < (bbtune_peak - 2.0f)) {
				int overshoot = (int)(bbtune_peak - (float)BBTUNE_TARGET_HIGH + 0.5f);
				if (overshoot < 0) overshoot = 0;
				bbtune_heat_sum += overshoot;
				printf("\nBB Tune cycle %d: heat overshoot = %d C (peak %.1f)",
				       bbtune_cycle + 1, overshoot, bbtune_peak);
				// Move to cooling phase
				bbtune_phase = BBTUNE_COOLING;
			}
			break;

		case BBTUNE_COOLING:
			// Full fan until we reach the low target
			*pheat = 0;
			*pfan = 255;
			if (temp <= (float)BBTUNE_TARGET_LOW) {
				// Reached target — switch to coast and track trough
				bbtune_phase = BBTUNE_COOL_COAST;
				bbtune_trough = temp;
				bbtune_prev_temp = temp;
			}
			break;

		case BBTUNE_COOL_COAST:
			// Fan off — wait for temp to trough and start rising
			*pheat = 0;
			*pfan = 0;
			if (temp < bbtune_trough) {
				bbtune_trough = temp;
			}
			// Detect rising: temp rose 2°C above trough (filters noise)
			if (temp > (bbtune_trough + 2.0f)) {
				int undershoot = (int)((float)BBTUNE_TARGET_LOW - bbtune_trough + 0.5f);
				if (undershoot < 0) undershoot = 0;
				bbtune_cool_sum += undershoot;
				printf("\nBB Tune cycle %d: cool undershoot = %d C (trough %.1f)",
				       bbtune_cycle + 1, undershoot, bbtune_trough);

				bbtune_cycle++;
				if (bbtune_cycle >= BBTUNE_NUM_CYCLES) {
					// All cycles done — average and store
					bbtune_heat_result = (bbtune_heat_sum + BBTUNE_NUM_CYCLES / 2) / BBTUNE_NUM_CYCLES;
					bbtune_cool_result = (bbtune_cool_sum + BBTUNE_NUM_CYCLES / 2) / BBTUNE_NUM_CYCLES;
					if (bbtune_heat_result > 25) bbtune_heat_result = 25;
					if (bbtune_cool_result > 25) bbtune_cool_result = 25;
					NV_SetConfig(REFLOW_BB_HEAT_OFFSET, (uint8_t)bbtune_heat_result);
					NV_SetConfig(REFLOW_BB_COOL_OFFSET, (uint8_t)bbtune_cool_result);
					printf("\nBB Tune DONE: heat offset = %d C, cool offset = %d C",
					       bbtune_heat_result, bbtune_cool_result);
					bbtune_phase = BBTUNE_DONE;
				} else {
					// Next cycle — reset peak/trough and start heating again
					bbtune_peak = 0;
					bbtune_trough = 999;
					bbtune_phase = BBTUNE_HEATING;
				}
			}
			break;

		case BBTUNE_DONE:
			// Stay here until UI handles exit
			*pheat = 0;
			*pfan = NV_GetConfig(REFLOW_MIN_FAN_SPEED);
			break;

		default:
			*pheat = 0;
			*pfan = 0;
			break;
	}

	// Return time until next call (same as PID timebase)
	return TICKS_MS(PID_TIMEBASE);
}
