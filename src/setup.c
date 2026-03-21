/*
 * setup.c - T-962 reflow controller
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

#include <stdint.h>
#include <stdio.h>
#include "nvstorage.h"
#include "reflow_profiles.h"
#include "setup.h"

static setupMenuStruct setupmenu[] = {
	{"Min fan speed    %4.0f",	REFLOW_MIN_FAN_SPEED,	 0, 254, 0, 1.0f, 		"Min fan speed     OFF","Min fan speed     MAX"},
	{"Cycle done beep %4.1fs",	REFLOW_BEEP_DONE_LEN,	 0, 254, 0, 0.1f, 		"Cycle done beep   OFF","Cycle done beep   MAX"},
	{"Left TC gain     %1.2f",	TC_LEFT_GAIN, 			10, 190, 0, 0.01f,		"Left TC gain     0.10","Left TC gain     1.90"},
	{"Left TC offs.  %+1.2f",	TC_LEFT_OFFSET, 		 0, 254, -127, 0.10f,	"Left TC offs.  -12.70","Left TC offs.   12.70"},
	{"Right TC gain    %1.2f",	TC_RIGHT_GAIN, 			10, 190, 0, 0.01f,		"Right TC gain    0.10","Right TC gain    1.90"},
	{"Right TC offs. %+1.2f",	TC_RIGHT_OFFSET, 	 	 0, 254, -127, 0.10f,	"Right TC offs. -12.70","Right TC offs.  12.70"},
	{"Preheat temp   %4.0fC",	REFLOW_PREHEAT_TEMP,	 0, 50, 30, 1.0f,		"Preheat temp       30","Preheat temp       80"},
	{"Bang-bang heat  %4.0f",	REFLOW_BANGBANG_MODE,	 0, 1, 0, 1.0f,		"Bang-bang heat    OFF","Bang-bang heat     ON"},
	{"BB heat offset %3.0fC",	REFLOW_BB_HEAT_OFFSET,	 0, 25, 0, 1.0f,	"BB heat offset     0C","BB heat offset    25C"},
	{"BB cool offset %3.0fC",	REFLOW_BB_COOL_OFFSET,	 0, 25, 0, 1.0f,	"BB cool offset     0C","BB cool offset    25C"},
	{"PID Kp        %5.1f",		PID_TUNE_KP,			 0, 254, 0, 0.5f,	"PID Kp      DEFAULT","PID Kp        127.0"},
	{"PID Ki       %5.3f",		PID_TUNE_KI,			 0, 254, 0, 0.002f,	"PID Ki      DEFAULT","PID Ki        0.508"},
	{"PID Kd        %5.1f",		PID_TUNE_KD,			 0, 254, 0, 0.5f,	"PID Kd      DEFAULT","PID Kd        127.0"},
	{"Screensaver mins %4.0f",	SCREENSAVER_ACTIVE, 	 0, 60, 0, 1.0f,		"Screensaver       OFF","Screensaver    1 HOUR"},
	{"Runaway thresh %3.0fC",	SAFETY_RUNAWAY_THRESH,	 0, 50, 0, 1.0f,		"Runaway prot.     OFF","Runaway thresh    50C"},
	{"Buzzer alerts  %4.0f",	REFLOW_BUZZER_ALERTS,	 0, 1, 0, 1.0f,		"Buzzer alerts     OFF","Buzzer alerts      ON"},
	{"Max cool rate %3.0f/s",	REFLOW_MAX_COOL_RATE,	 0, 50, 0, 0.1f,		"Max cool rate UNLIMIT","Max cool rate  5.0C/s"},
	{"L TC hi-off %+1.2f",		TC_LEFT_OFFSET_HI, 	 0, 254, -127, 0.10f,	"L TC hi-off   -12.70","L TC hi-off    12.70"},
	{"R TC hi-off %+1.2f",		TC_RIGHT_OFFSET_HI, 	 0, 254, -127, 0.10f,	"R TC hi-off   -12.70","R TC hi-off    12.70"},
	{"Temp unit    %4.0f",		TEMP_UNIT_FAHRENHEIT,	 0, 1, 0, 1.0f,		"Temp unit       DEG C","Temp unit       DEG F"},
	{"Fan kickstart %4.0f",		REFLOW_FAN_KICKSTART,	 0, 1, 0, 1.0f,		"Fan kickstart     OFF","Fan kickstart      ON"},
};

#define NUM_SETUP_ITEMS (sizeof(setupmenu) / sizeof(setupmenu[0]))

int Setup_getNumItems(void) {
	return NUM_SETUP_ITEMS;
}

int _getRawValue(int item) {
	return NV_GetConfig(setupmenu[item].nvval);
}

float Setup_getValue(int item) {
	int intval = _getRawValue(item);
	intval += setupmenu[item].offset;
	return ((float)intval) * setupmenu[item].multiplier;
}

void Setup_setValue(int item, int value) {
	NV_SetConfig(setupmenu[item].nvval, value);
	Reflow_ValidateNV();
}

void Setup_setRealValue(int item, float value) {
	int intval = (int)(value / setupmenu[item].multiplier);
	intval -= setupmenu[item].offset;
	Setup_setValue(item, intval);
}

void Setup_increaseValue(int item, int amount) {
	int curval = _getRawValue(item) + amount;
	int maxval = setupmenu[item].maxval;
	if(curval > maxval)
		curval = maxval;
	Setup_setValue(item, curval);
}

void Setup_decreaseValue(int item, int amount) {
	int curval = _getRawValue(item) - amount;
	int minval = setupmenu[item].minval;
	if(curval < minval)
		curval = minval;
	Setup_setValue(item, curval);
}

void Setup_printFormattedValue(int item) {
	printf(setupmenu[item].formatstr, Setup_getValue(item));
}

int Setup_snprintFormattedValue(char* buf, int n, int item) {
	int curval = _getRawValue(item);
	int minval = setupmenu[item].minval;
	int maxval = setupmenu[item].maxval;
	if(curval==minval){
		return snprintf(buf, n, "%s", setupmenu[item].minStr);
	}
	if(curval==maxval){
		return snprintf(buf, n, "%s", setupmenu[item].maxStr);
	}

	return snprintf(buf, n, setupmenu[item].formatstr, Setup_getValue(item));
}
