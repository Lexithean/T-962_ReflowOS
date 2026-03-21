/*
 * main.c - T-962 reflow controller
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
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "serial.h"
#include "advancedcmd.h"
#include "lcd.h"
#include "io.h"
#include "sched.h"
#include "onewire.h"
#include "adc.h"
#include "i2c.h"
#include "rtc.h"
#include "eeprom.h"
#include "keypad.h"
#include "reflow.h"
#include "reflow_profiles.h"
#include "sensor.h"
#include "buzzer.h"
#include "nvstorage.h"
#include "version.h"
#include "vic.h"
#include "max31855.h"
#include "systemfan.h"
#include "setup.h"
#include "ui_extras.h"
#include "flashprofiles.h"

extern uint8_t logobmp[];
extern uint8_t stopbmp[];
extern uint8_t selectbmp[];
extern uint8_t editbmp[];
extern uint8_t f3editbmp[];
extern uint8_t graph2bmp[];
extern uint8_t exitbmp[];

// No version.c file generated for LPCXpresso builds, fall back to this
__attribute__((weak)) const char* Version_GetGitVersion(void) {
	return "no version info";
}

static char* format_about = \
"\nT-962-controller open source firmware (%s)" \
"\n" \
"\nSee https://github.com/UnifiedEngineering/T-962-improvements for more details." \
"\n" \
"\nInitializing improved reflow oven...";

static char* help_text = \
"\nT-962-controller serial interface.\n\n" \
" about                   Show about + debug information\n" \
" bake <setpoint>         Enter Bake mode with setpoint\n" \
" bake <setpoint> <time>  Enter Bake mode with setpoint for <time> seconds\n" \
" backup                  Dump all profiles as restorable text\n" \
" delete flash <N>        Delete flash profile slot N\n" \
" dump profile <id>       Dump profile temperature data\n" \
" export profile <id>     Export profile in import-compatible format\n" \
" help                    Display help text\n" \
" import profile N t,t,.. Import text profile into CUSTOM#N (1 or 2)\n" \
" json                    Toggle JSON serial output mode\n" \
" list flash              List flash-stored profiles\n" \
" list profiles           List available reflow profiles\n" \
" list settings           List machine settings\n" \
" name profile N <name>   Rename CUSTOM#N profile (max 18 chars)\n" \
" quiet                   No logging in standby mode\n" \
" reflow                  Start reflow with selected profile\n" \
" save flash N t,t,..,Nm  Save profile to flash slot N\n" \
" select profile <id>     Select reflow profile by id\n" \
" set OpMode <mode>       Set Operational Mode (0-2)\n" \
" set OpThresh <thresh>   Set mode threshold in C (0-255)\n" \
" setting <id> <value>    Set setting id to value\n" \
" stop                    Exit reflow or bake mode\n" \
" bbtune                  Run bang-bang auto-tune\n" \
" pidtune                 Run PID auto-tune\n" \
" tccal                   Run TC offset auto-calibration\n" \
" values                  Dump currently measured values\n" \
"\n";


static float display_temp(float celsius) {
	if (NV_GetConfig(TEMP_UNIT_FAHRENHEIT) == 1) {
		return celsius * 9.0f / 5.0f + 32.0f;
	}
	return celsius;
}

static const char* temp_unit(void) {
	if (NV_GetConfig(TEMP_UNIT_FAHRENHEIT) == 1) {
		return "F";
	}
	return "C";
}

static int32_t Main_Work(void);

int main(void) {
	char buf[22];
	int len=0;

	IO_JumpBootloader();

	PLLCFG = (1 << 5) | (4 << 0); //PLL MSEL=0x4 (+1), PSEL=0x1 (/2) so 11.0592*5 = 55.296MHz, Fcco = (2x55.296)*2 = 221MHz which is within 156 to 320MHz
	PLLCON = 0x01;
	PLLFEED = 0xaa;
	PLLFEED = 0x55; // Feed complete
	while (!(PLLSTAT & (1 << 10))); // Wait for PLL to lock
	PLLCON = 0x03;
	PLLFEED = 0xaa;
	PLLFEED = 0x55; // Feed complete
	VPBDIV = 0x01; // APB runs at the same frequency as the CPU (55.296MHz)
	MAMTIM = 0x03; // 3 cycles flash access recommended >40MHz
	MAMCR = 0x02; // Fully enable memory accelerator

	VIC_Init();
	Sched_Init();
	IO_Init();
	Set_Heater(0);
	Set_Fan(0);
	Serial_Init();
	printf(format_about, Version_GetGitVersion());

	I2C_Init();
	EEPROM_Init();
	NV_Init();

	LCD_Init();
	LCD_BMPDisplay(logobmp, 0, 0);

	IO_InitWatchdog();
	IO_PrintResetReason();

	len = IO_Partinfo(buf, sizeof(buf), "%s rev %c");
	//LCD_disp_str((uint8_t*)buf, len, 0, 64 - 6, FONT6X6);
	printf("\nRunning on an %s", buf);

	len = snprintf(buf, sizeof(buf), "%s", Version_GetGitVersion());
	//LCD_disp_str((uint8_t*)buf, len, 128 - (len * 6), 0, FONT6X6);

	LCD_FB_Update();
	Keypad_Init();
	Buzzer_Init();
	ADC_Init();
	RTC_Init();
	OneWire_Init();
	SPI_TC_Init();
	Reflow_Init();
	FlashProfiles_Init();
	SystemFan_Init();
	printf("\nCurrent Operational Mode: "); Sensor_printOpMode(); printf("\n");
	printf("Current Operational Mode Threshold: %u C\n", Sensor_getOpModeThreshold());

	Sched_SetWorkfunc(MAIN_WORK, Main_Work);
	Sched_SetState(MAIN_WORK, 1, TICKS_SECS(3)); // Enable in 3 seconds

	Buzzer_Beep(BUZZ_1KHZ, 255, TICKS_MS(50));

	while (1) {
#ifdef ENABLE_SLEEP
//		int32_t sleeptime;
//		sleeptime = Sched_Do(0); // No fast-forward support
		//printf("\n%d ticks 'til next activity"),sleeptime);
#else
		Sched_Do(0); // No fast-forward support
#endif
	}
	return 0;
}

typedef enum eMainMode {
	MAIN_HOME = 0,
	MAIN_ABOUT,
	MAIN_SETUP,
	MAIN_BAKE,
	MAIN_SELECT_PROFILE,
	MAIN_EDIT_PROFILE,
	MAIN_REFLOW,
	MAIN_BBTUNE,
	MAIN_PIDTUNE,
	MAIN_TCCAL,
	MAIN_SCREENSAVER,
	MAIN_INIT
} MainMode_t;

static char buf[25];

// ============================================================================
// Rolling temperature graph for tune screens
// Graph area: X=9..118 (110 pixels), Y=10..52 (42 pixels)
// ============================================================================
#define TUNE_GRAPH_X0    (9)
#define TUNE_GRAPH_W     (110)
#define TUNE_GRAPH_Y0    (10)   // Top of graph (high temp)
#define TUNE_GRAPH_Y1    (52)   // Bottom of graph (low temp)
#define TUNE_GRAPH_H     (TUNE_GRAPH_Y1 - TUNE_GRAPH_Y0)
#define TUNE_GRAPH_TMIN  (20.0f)   // Min temp on Y axis
#define TUNE_GRAPH_TMAX  (260.0f)  // Max temp on Y axis

static uint8_t tuneGraph[TUNE_GRAPH_W]; // Y pixel for each sample
static int tuneGraphIdx = 0;
static int tuneGraphCount = 0;

static void TuneGraph_Reset(void) {
	tuneGraphIdx = 0;
	tuneGraphCount = 0;
	for (int i = 0; i < TUNE_GRAPH_W; i++) tuneGraph[i] = 0;
}

static void TuneGraph_AddSample(float temp) {
	// Map temperature to Y pixel (inverted: high temp = low Y)
	float frac = (temp - TUNE_GRAPH_TMIN) / (TUNE_GRAPH_TMAX - TUNE_GRAPH_TMIN);
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	uint8_t y = TUNE_GRAPH_Y1 - (uint8_t)(frac * (float)TUNE_GRAPH_H);
	tuneGraph[tuneGraphIdx] = y;
	tuneGraphIdx = (tuneGraphIdx + 1) % TUNE_GRAPH_W;
	if (tuneGraphCount < TUNE_GRAPH_W) tuneGraphCount++;
}

static void TuneGraph_Draw(float targetHigh, float targetLow) {
	// Draw graph border (left and bottom lines)
	for (int y = TUNE_GRAPH_Y0; y <= TUNE_GRAPH_Y1; y++) {
		LCD_SetPixel(TUNE_GRAPH_X0 - 1, y);
	}
	for (int x = TUNE_GRAPH_X0 - 1; x <= TUNE_GRAPH_X0 + TUNE_GRAPH_W; x++) {
		LCD_SetPixel(x, TUNE_GRAPH_Y1 + 1);
	}

	// Draw target line(s) as dashed
	if (targetHigh > 0) {
		float frac = (targetHigh - TUNE_GRAPH_TMIN) / (TUNE_GRAPH_TMAX - TUNE_GRAPH_TMIN);
		if (frac >= 0.0f && frac <= 1.0f) {
			uint8_t ty = TUNE_GRAPH_Y1 - (uint8_t)(frac * (float)TUNE_GRAPH_H);
			for (int x = TUNE_GRAPH_X0; x < TUNE_GRAPH_X0 + TUNE_GRAPH_W; x += 3) {
				LCD_SetPixel(x, ty);
			}
		}
	}
	if (targetLow > 0 && targetLow != targetHigh) {
		float frac = (targetLow - TUNE_GRAPH_TMIN) / (TUNE_GRAPH_TMAX - TUNE_GRAPH_TMIN);
		if (frac >= 0.0f && frac <= 1.0f) {
			uint8_t ty = TUNE_GRAPH_Y1 - (uint8_t)(frac * (float)TUNE_GRAPH_H);
			for (int x = TUNE_GRAPH_X0; x < TUNE_GRAPH_X0 + TUNE_GRAPH_W; x += 3) {
				LCD_SetPixel(x, ty);
			}
		}
	}

	// Draw temperature trace (rolling, newest sample on the right)
	for (int i = 0; i < tuneGraphCount; i++) {
		int bufIdx;
		int screenX;
		if (tuneGraphCount < TUNE_GRAPH_W) {
			// Buffer not full yet: draw from left
			bufIdx = i;
			screenX = TUNE_GRAPH_X0 + i;
		} else {
			// Buffer full: oldest is at tuneGraphIdx, draw rolling
			bufIdx = (tuneGraphIdx + i) % TUNE_GRAPH_W;
			screenX = TUNE_GRAPH_X0 + i;
		}
		if (tuneGraph[bufIdx] > 0) {
			LCD_SetPixel(screenX, tuneGraph[bufIdx]);
		}
	}
}
static int len;
static uint16_t animCnt=0;
static int16_t animIX=0,animIY=0,animIZ=0;
static uint8_t blinkCnt=0,blinkOn=0;


static int32_t Main_Work(void) {
	static MainMode_t mode = MAIN_HOME;
	static MainMode_t prevMode = MAIN_INIT;
	static uint16_t setpoint = 0;
	static uint8_t modeChange=0;

	if (setpoint == 0) {
		Reflow_LoadSetpoint();
		setpoint = Reflow_GetSetpoint();
	}
	static int timer = 0;

	// profile editing
	static uint8_t profile_time_idx = 0;
	static uint8_t current_edit_profile;

	int32_t retval = TICKS_MS(500);

	uint32_t keyspressed = Keypad_Get();

	char serial_cmd[255] = "";
	char* cmd_select_profile = "select profile %d";
	char* cmd_bake = "bake %d %d";
	char* cmd_dump_profile = "dump profile %d";
	char* cmd_setting = "setting %d %f";
	char* cmd_setOpMode = "set OpMode %d";
	char* cmd_setOpModeThresh = "set OpThresh %d";
	
	advancedSerialCMD advCmd = { 0 };
	
	if (uart_chkAdvCmd(&advCmd)) {
		switch (advCmd.cmd) {
			case SetEEProfileCmd:
			{
				EEProfileCMD EEcmd;
				memcpy(&EEcmd, &advCmd.data, sizeof(advCmd.data));
			
				if (EEcmd.profileNum == 1 || EEcmd.profileNum == 2) {
					printf("\nSetting EE profile %d:\n ", advCmd.data[0]);
					Reflow_SelectEEProfileIdx(EEcmd.profileNum);
					for (unsigned char i = 0; i < NUMPROFILETEMPS; ++i) {
						Reflow_SetSetpointAtIdx(i, EEcmd.tempData[i]);
					}
					Reflow_SaveEEProfile();
					if (EEcmd.profileNum == 1) Reflow_DumpProfile(5); //CUSTOM#1
					else Reflow_DumpProfile(6); //CUSTOM#2
				}
				else {
					printf("\nOnly EEPROM profile 1 and 2 are supported for this command.\n");
				}
				//reset advCmd
				//memset(&advCmd, 0, sizeof(advCmd));
				break;
			}
			//Add more advanced commands here
			//case ...
			default: {
				printf("\nUnknown Advanced Command entered.\n");
				//memset(&advCmd, 0, sizeof(advCmd));
				uart_rxflush();
			}
		}
	} else if (uart_available() > 3) {
		int len = uart_readline(serial_cmd, 255);

		if (len > 0) {
			int param, param1;
			float paramF;

			if (strcmp(serial_cmd, "about") == 0) {
				printf(format_about, Version_GetGitVersion());
				len = IO_Partinfo(buf, sizeof(buf), "\nPart number: %s rev %c\n");
				printf(buf);
				EEPROM_Dump();

				printf("\nSensor values:\n");
				Sensor_ListAll();

			} else if (strcmp(serial_cmd, "help") == 0 || strcmp(serial_cmd, "?") == 0) {
				printf(help_text);

			} else if (strcmp(serial_cmd, "list profiles") == 0) {
				printf("\nReflow profiles available:\n");

				Reflow_ListProfiles();
				printf("\n");

			} else if (strcmp(serial_cmd, "reflow") == 0) {
				printf("\nStarting reflow with profile: %s\n", Reflow_GetProfileName());
				mode = MAIN_HOME;
				// this is a bit dirty, but with the least code duplication.
				keyspressed = KEY_S;

			} else if (strcmp(serial_cmd, "list settings") == 0) {
				printf("\nCurrent settings:\n\n");
				for (int i = 0; i < Setup_getNumItems() ; i++) {
					printf("%d: ", i);
					Setup_printFormattedValue(i);
					printf("\n");
				}

			} else if (strcmp(serial_cmd, "bbtune") == 0) {
				if (NV_GetConfig(REFLOW_BANGBANG_MODE)) {
					printf("\nStarting bang-bang auto-tune (3 cycles)...\n");
					Reflow_BBTune_Start();
					mode = MAIN_BBTUNE;
					retval = 0;
				} else {
					printf("\nBang-bang mode must be enabled first\n");
				}

			} else if (strcmp(serial_cmd, "pidtune") == 0) {
				if (!NV_GetConfig(REFLOW_BANGBANG_MODE)) {
					printf("\nStarting PID auto-tune (Ziegler-Nichols, 3 cycles)...\n");
					Reflow_PIDTune_Start();
					mode = MAIN_PIDTUNE;
					retval = 0;
				} else {
					printf("\nPID tune requires PID mode (bang-bang must be OFF)\n");
				}

			} else if (strcmp(serial_cmd, "tccal") == 0) {
				printf("\nRunning TC offset auto-calibration...\n");
				int result = Sensor_AutoCalibrate();
				if (result == 0) {
					printf("\nCalibration successful\n");
				} else if (result == 1) {
					printf("\nOven too hot, let it cool below %.0fC\n", TCCAL_MAX_TEMP);
				} else {
					printf("\nNo cold junction sensor found\n");
				}

			} else if (strcmp(serial_cmd, "stop") == 0) {
				printf("\nStopping bake/reflow");
				mode = MAIN_HOME;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0;

			} else if (strcmp(serial_cmd, "quiet") == 0) {
				Reflow_ToggleStandbyLogging();
				printf("\nToggled standby logging\n");

			} else if (strcmp(serial_cmd, "values") == 0) {
				printf("\nActual measured values:\n");
				Sensor_ListAll();
				printf("\n");

			} else if (strcmp(serial_cmd, "get OpMode") == 0) {
				printf("\nCurrent Operational Mode: "); Sensor_printOpMode(); printf("\n");

			} else if (strcmp(serial_cmd, "get OpThresh") == 0) {
				printf("\nCurrent Operational Mode Threshold: %u C\n", Sensor_getOpModeThreshold());

			} else if (sscanf(serial_cmd, cmd_setOpMode, &param) > 0) {
				// set operational mode
				if (param > 2 || param < 0) {
					printf("\nOnly options are 0-2. See Help.\n");

				} else {
					Sensor_setOpMode((OperationMode_t)param);
					printf("\nOperational Mode Set: "); Sensor_printOpMode(); printf("\n");
				}

			} else if (sscanf(serial_cmd, cmd_setOpModeThresh, &param) > 0) {
				// set operational mode threshold
				if (param > 255 || param < 0) {
					printf("\nOnly options are 0-255\n");
				} else {
					Sensor_setOpModeThreshold((uint8_t)param);
					printf("\nOperational Mode Threshold Set: %d C\n", param);
				}

			} else if (sscanf(serial_cmd, cmd_select_profile, &param) > 0) {
				// select profile
				Reflow_SelectProfileIdx(param);
				printf("\nSelected profile %d: %s\n", param, Reflow_GetProfileName());

			} else if (sscanf(serial_cmd, cmd_bake, &param, &param1) > 0) {
				if (param < SETPOINT_MIN) {
					printf("\nSetpoint must be >= %ddegC\n", SETPOINT_MIN);
					param = SETPOINT_MIN;
				}
				if (param > SETPOINT_MAX) {
					printf("\nSetpont must be <= %ddegC\n", SETPOINT_MAX);
					param = SETPOINT_MAX;
				}
				if (param1 < 1) {
					printf("\nTimer must be greater than 0\n");
					param1 = 1;
				}

				if (param1 < BAKE_TIMER_MAX) {
					printf("\nStarting bake with setpoint %ddegC for %ds after reaching setpoint\n", param, param1);
					timer = param1;
					Reflow_SetBakeTimer(timer);
				} else {
					printf("\nStarting bake with setpoint %ddegC\n", param);
				}

				setpoint = param;
				Reflow_SetSetpoint(setpoint);
				mode = MAIN_BAKE;
				Reflow_SetMode(REFLOW_BAKE);

			} else if (sscanf(serial_cmd, cmd_dump_profile, &param) > 0) {
				printf("\nDumping profile %d: %s\n ", param, Reflow_GetProfileName());
				Reflow_DumpProfile(param);

			} else if (sscanf(serial_cmd, cmd_setting, &param, &paramF) > 0) {
				Setup_setRealValue(param, paramF);
				printf("\nAdjusted setting: ");
				Setup_printFormattedValue(param);

			} else if (strcmp(serial_cmd, "json") == 0) {
				Reflow_SetJsonOutput(!Reflow_GetJsonOutput());
				printf("\nJSON output: %s\n", Reflow_GetJsonOutput() ? "ON" : "OFF");

			} else if (strncmp(serial_cmd, "import profile ", 15) == 0) {
				// Text-based profile import: "import profile N t1,t2,t3,..."
				// N = 1 or 2 (CUSTOM EE profile slots)
				int profNum = serial_cmd[15] - '0';
				if (profNum == 1 || profNum == 2) {
					char* tempStr = &serial_cmd[17]; // Skip "import profile N "
					Reflow_SelectEEProfileIdx(profNum);
					int idx = 0;
					char* tok = tempStr;
					while (idx < NUMPROFILETEMPS && *tok != '\0') {
						int val = 0;
						while (*tok >= '0' && *tok <= '9') {
							val = val * 10 + (*tok - '0');
							tok++;
						}
						Reflow_SetSetpointAtIdx(idx, (uint16_t)val);
						idx++;
						if (*tok == ',') tok++;
						while (*tok == ' ') tok++;
					}
					// Zero remaining entries
					for (int i = idx; i < NUMPROFILETEMPS; i++) {
						Reflow_SetSetpointAtIdx(i, 0);
					}
					Reflow_SaveEEProfile();
					printf("\nImported %d temperature points to CUSTOM#%d\n", idx, profNum);
					Reflow_DumpProfile(profNum == 1 ? 5 : 6);
				} else {
					printf("\nOnly CUSTOM profile 1 or 2 supported (import profile 1 or 2)\n");
				}

			} else if (sscanf(serial_cmd, "export profile %d", &param) > 0) {
				Reflow_ExportProfile(param);

			} else if (strncmp(serial_cmd, "name profile ", 13) == 0) {
				int profNum = serial_cmd[13] - '0';
				if ((profNum == 1 || profNum == 2) && serial_cmd[14] == ' ') {
					Reflow_SetProfileName(profNum, &serial_cmd[15]);
					printf("\nRenamed CUSTOM#%d to: %s\n", profNum, Reflow_GetProfileName());
				} else {
					printf("\nUsage: name profile 1 MyProfile (or 2)\n");
				}

			} else if (strncmp(serial_cmd, "save flash ", 11) == 0) {
				// save flash N t1,t2,...,Name
				int fslot = serial_cmd[11] - '0';
				if (serial_cmd[12] >= '0' && serial_cmd[12] <= '9') {
					fslot = fslot * 10 + (serial_cmd[12] - '0');
				}
				if (fslot >= 0 && fslot < FLASH_PROFILE_MAX_SLOTS) {
					// Find the temperature data start
					char* tempStart = strchr(&serial_cmd[11], ' ');
					if (tempStart) {
						tempStart++;
						uint16_t ftemps[48] = {0};
						char fname[FLASH_PROFILE_NAME_LEN] = {0};
						int tidx = 0;
						char* tok = tempStart;
						while (tidx < 48 && *tok != '\0') {
							if (*tok >= '0' && *tok <= '9') {
								int val = 0;
								while (*tok >= '0' && *tok <= '9') {
									val = val * 10 + (*tok - '0');
									tok++;
								}
								ftemps[tidx++] = (uint16_t)val;
								if (*tok == ',') tok++;
							} else {
								// Rest is the name
								strncpy(fname, tok, FLASH_PROFILE_NAME_LEN - 1);
								break;
							}
						}
						// If last CSV field is text (after all temps), use as name
						if (fname[0] == '\0') {
							snprintf(fname, FLASH_PROFILE_NAME_LEN, "Flash #%d", fslot);
						}
						if (FlashProfiles_WriteProfile(fslot, ftemps, fname) == 0) {
							printf("\nSaved %d points to flash slot %d (%s)\n", tidx, fslot, fname);
						} else {
							printf("\n[ERROR] Flash write failed\n");
						}
					} else {
						printf("\nUsage: save flash N t1,t2,...,Name\n");
					}
				} else {
					printf("\nSlot must be 0-%d\n", FLASH_PROFILE_MAX_SLOTS - 1);
				}

			} else if (sscanf(serial_cmd, "delete flash %d", &param) > 0) {
				if (FlashProfiles_Delete(param) == 0) {
					printf("\nDeleted flash profile %d\n", param);
				} else {
					printf("\n[ERROR] Delete failed\n");
				}

			} else if (strcmp(serial_cmd, "list flash") == 0) {
				printf("\nFlash profiles: %d/%d slots\n", FlashProfiles_GetCount(), FlashProfiles_GetCapacity());
				for (int fi = 0; fi < FLASH_PROFILE_MAX_SLOTS; fi++) {
					if (FlashProfiles_IsValid(fi)) {
						printf("  [%2d] %s\n", fi, FlashProfiles_GetName(fi));
					}
				}

			} else if (strcmp(serial_cmd, "backup") == 0) {
				FlashProfiles_BackupAll();

			} else {
				printf("\nCannot understand command, ? for help\n");
			}
		}
	}

	// Set flag if mode was changed by user action (to minimise redraws)
	if(mode!=prevMode){
		prevMode=mode;
		modeChange=1;
		animCnt=0;
		animIX=0;
		animIY=0;
		animIZ=0;
	}else{
		modeChange=0;
		++animCnt;
	}

	if(++blinkCnt>=5){
		blinkOn=(blinkOn==0?1:0);
		blinkCnt=0;
	}




	// main menu state machine
	// setup/calibration
	if (mode == MAIN_SETUP) {
		static int8_t selected = 0;
		static int8_t scrollPos = 0;
		int y = 0;

		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1) keyrepeataccel = 1;
		if (keyrepeataccel > 30) keyrepeataccel = 30;

		if (keyspressed & KEY_F1) {
			if (selected > 0) { // Prev row
				selected--;
			} else { // wrap
				selected = Setup_getNumItems() - 1;
			}
		}
		if (keyspressed & KEY_F2) {
			if (selected < (Setup_getNumItems() - 1)) { // Next row
				selected++;
			} else { // wrap
				selected = 0;
			}
		}

		if (keyspressed & KEY_F3) {
			Setup_decreaseValue(selected, keyrepeataccel);
		}
		if (keyspressed & KEY_F4) {
			Setup_increaseValue(selected, keyrepeataccel);
		}

		LCD_FB_Clear();
		retval = TICKS_MS(100);

		showHeader("Setup/calibration");
		for(uint8_t n=0;n<128;n++){
			LCD_SetPixel(n,7);
			LCD_SetPixel(n,64-9);
		}

		y += 11;

		int8_t maxItems=6;
		int8_t numItems=Setup_getNumItems();
		int8_t endItem=scrollPos+maxItems;

		if(scrollPos>selected){
			scrollPos=selected;
			endItem=scrollPos+maxItems;
		}else if(selected>=endItem){
			endItem=selected+1;
			scrollPos=endItem-maxItems;
		}

		if(scrollPos<0){
			scrollPos=0;
			endItem=maxItems;
		}

		if(endItem>numItems){
			endItem=numItems;
		}

		if(endItem-scrollPos>maxItems){
			endItem=scrollPos+maxItems;
		}

		for (int i = scrollPos; i < endItem ; i++) {
			len = Setup_snprintFormattedValue(buf, sizeof(buf), i);
			LCD_disp_str((uint8_t*)buf, len, 0, y, FONT6X6 | (selected == i) ? INVERT : 0);
			y += 7;
		}

		// buttons
		y = 64 - 7;
		LCD_disp_str((uint8_t*)" < ", 3, 0, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" > ", 3, 20, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" - ", 3, 45, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" + ", 3, 65, y, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)" DONE ", 6, 91, y, FONT6X6 | INVERT);

		// Leave setup
		if (keyspressed & KEY_S) {
			// If on bang-bang rows and bang-bang is ON, go to BB Tune
			if (NV_GetConfig(REFLOW_BANGBANG_MODE) && selected >= 7 && selected <= 9) {
				mode = MAIN_BBTUNE;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0;
			// If on PID rows and bang-bang is OFF, go to PID Tune
			} else if (!NV_GetConfig(REFLOW_BANGBANG_MODE) && selected >= 10 && selected <= 12) {
				mode = MAIN_PIDTUNE;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0;
			// If on TC offset rows (3 or 5 = left/right offset), go to TC Cal
			} else if (selected == 3 || selected == 5) {
				mode = MAIN_TCCAL;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0;
			} else {
				mode = MAIN_HOME;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0; // Force immediate refresh
			}
		}



	// About
	} else if (mode == MAIN_ABOUT) {
		if(modeChange){
			LCD_FB_Clear();
			LCD_BMPDisplay(logobmp, 0, 0);
			uint8_t n,i;

			for(n=0;n<8;n++){
				for(i=0;i<5;i++){
					if( (i>1) || (n>0 && i>0) || (n>1 && i==0) )
						LCD_disp_str((uint8_t*)" ", 1, (128-8*6)+(n*6), 26+(i*7), FONT6X6|INVERT);
				}
			}

			for(n=0;n<22;n++){
				LCD_disp_str((uint8_t*)" ", 1, n*6, 2, FONT6X6);
				LCD_disp_str((uint8_t*)" ", 1, n*6, 64-10, FONT6X6);
				LCD_disp_str((uint8_t*)" ", 1, n*6, 64-7, FONT6X6);
			}

			for(n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}


			len = snprintf(buf, sizeof(buf), "T-962 OVEN");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_RIGHT(len), 20, FONT6X6);
			len = snprintf(buf, sizeof(buf), "SMASHCAT UI");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_RIGHT(len), 28, FONT6X6);
			len = snprintf(buf, sizeof(buf), "%s", Version_GetGitVersion());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_RIGHT(len), 36, FONT6X6);
			len = snprintf(buf, sizeof(buf), "UNIFIED ENGINEERING");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_RIGHT(len), 44, FONT6X6);

			LCD_disp_str((uint8_t*)"  ", 2, 128-12, 64-7, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"S", 1, 128-9, 64-7, FONT6X6 | INVERT);

		}
		retval = TICKS_MS(100);
		showHeader("ABOUT");

		// Leave about with any key.
		if (keyspressed & KEY_ANY) {
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}


	// Reflow active!
	} else if (mode == MAIN_REFLOW) {
		static uint16_t prev_setpoint = 0;
		static uint8_t alerted_rising = 0;
		static uint8_t alerted_peak = 0;
		static uint8_t alerted_cooling = 0;

		// Check for thermal runaway
		if (Reflow_ThermalRunaway()) {
			LCD_FB_Clear();
			showHeader("!! RUNAWAY !!");
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}
			len = snprintf(buf, sizeof(buf), "THERMAL RUNAWAY");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 14, FONT6X6 | INVERT);
			len = snprintf(buf, sizeof(buf), "TEMP EXCEEDED LIMIT");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 24, FONT6X6);
			len = snprintf(buf, sizeof(buf), "HEATER OFF - FAN ON");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 34, FONT6X6);
			len = snprintf(buf, sizeof(buf), "TEMP: %.0f`", Sensor_GetTemp(TC_AVERAGE));
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 44, FONT6X6);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" DISMISS ", 9, 91 - 18, y, FONT6X6 | INVERT);

			if (animIX == 0) {
				Buzzer_Beep(BUZZ_2KHZ, 255, TICKS_MS(2000));
				animIX = 2; // Use 2 to distinguish from normal done
			}

			retval = TICKS_MS(250);

			if (keyspressed & KEY_ANY) {
				Reflow_ClearRunaway();
				mode = MAIN_HOME;
				retval = 0;
			}

		} else {
			if(Reflow_IsDone()){
				if(animIX==0){
					Buzzer_Beep(BUZZ_1KHZ, 255, TICKS_MS(100) * NV_GetConfig(REFLOW_BEEP_DONE_LEN));
					printf("\nReflow %s\n", "completed");
					animIX=1;
					Reflow_SetMode(REFLOW_STANDBY);
				}
			}

			// Stage transition buzzer alerts
			if (NV_GetConfig(REFLOW_BUZZER_ALERTS) && !Reflow_IsDone()) {
				uint16_t sp = Reflow_GetSetpoint();
				// Rising: temp passing through profile setpoint upward
				if (sp > prev_setpoint + 5 && !alerted_rising && sp > 100) {
					// Temperature is ramping up past 100°C - soak/ramp alert
					Buzzer_Beep(BUZZ_1KHZ, 200, TICKS_MS(100));
					alerted_rising = 1;
				}
				// Peak: setpoint starts dropping (reflow peak reached)
				if (prev_setpoint > 0 && sp < prev_setpoint - 3 && !alerted_peak && prev_setpoint > 150) {
					Buzzer_Beep(BUZZ_2KHZ, 255, TICKS_MS(200));
					alerted_peak = 1;
				}
				// Cooling: temp dropping below 100°C
				if (alerted_peak && sp < 100 && !alerted_cooling) {
					Buzzer_Beep(BUZZ_1KHZ, 200, TICKS_MS(150));
					alerted_cooling = 1;
				}
				prev_setpoint = sp;
			}

			displayReflowScreen(keyspressed,modeChange,animIX);
			retval = TICKS_MS(100);

			// Abort reflow
			if (keyspressed & KEY_S) {
				if(animIX==0){
					printf("\nReflow %s\n", "interrupted by keypress");
				}
				Reflow_ClearRunaway();
				prev_setpoint = 0;
				alerted_rising = alerted_peak = alerted_cooling = 0;
				mode = MAIN_HOME;
				Reflow_SetMode(REFLOW_STANDBY);
				retval = 0; // Force immediate refresh
			}
		}



	// Select reflow profile
	} else if (mode == MAIN_SELECT_PROFILE) {
		int curprofile = Reflow_GetProfileIdx();
		LCD_FB_Clear();

		// Prev profile
		if (keyspressed & KEY_F1) {
			curprofile--;
		}
		// Next profile
		if (keyspressed & KEY_F2) {
			curprofile++;
		}

		Reflow_SelectProfileIdx(curprofile);

		Reflow_PlotProfile(-1);
		LCD_BMPDisplay(selectbmp, 127 - 17, 0);
		int eeidx = Reflow_GetEEProfileIdx();
		if (eeidx) { // Display edit button
			LCD_BMPDisplay(f3editbmp, 127 - 17, 29);
		}
		len = snprintf(buf, sizeof(buf), "%s", Reflow_GetProfileName());
		LCD_disp_str((uint8_t*)buf, len, 13, 0, FONT6X6);

		if (eeidx && keyspressed & KEY_F3) { // Edit ee profile
			mode = MAIN_EDIT_PROFILE;
			current_edit_profile = eeidx;
			retval = 0; // Force immediate refresh
		}

		// Select current profile
		if (keyspressed & KEY_S) {
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}




	// Manual bake mode
	} else if (mode == MAIN_BAKE) {
		LCD_FB_Clear();
		retval = TICKS_MS(100);
		showHeader("MANUAL/BAKE MODE");
		for(uint8_t n=0;n<128;n++){
			LCD_SetPixel(n,7);
			LCD_SetPixel(n,64-9);
		}

		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1)
			keyrepeataccel = 1;
		if (keyrepeataccel > 30)
			keyrepeataccel = 30;

		// Setpoint-
		if (keyspressed & KEY_F1) {
			setpoint -= keyrepeataccel;
			if (setpoint < SETPOINT_MIN) setpoint = SETPOINT_MIN;
		}

		// Setpoint+
		if (keyspressed & KEY_F2) {
			setpoint += keyrepeataccel;
			if (setpoint > SETPOINT_MAX) setpoint = SETPOINT_MAX;
		}

		// timer --
		if (keyspressed & KEY_F3) {
			if (timer - keyrepeataccel < 0) {
				// infinite bake
				timer = -1;
			} else {
				timer -= keyrepeataccel;
			}
		}
		// timer ++
		if (keyspressed & KEY_F4) {
			timer += keyrepeataccel;
		}

		int y = 10;
		// display F1 button only if setpoint can be decreased
		char f1function = ' ';
		if (setpoint > SETPOINT_MIN) {
			LCD_disp_str((uint8_t*)"F1", 2, 0, y, FONT6X6 | INVERT);
			f1function = '-';
		}
		// display F2 button only if setpoint can be increased
		char f2function = ' ';
		if (setpoint < SETPOINT_MAX) {
			LCD_disp_str((uint8_t*)"F2", 2, LCD_ALIGN_RIGHT(2), y, FONT6X6 | INVERT);
			f2function = '+';
		}
		len = snprintf(buf, sizeof(buf), "%c SETPOINT %d`%s %c", f1function, (int)setpoint, temp_unit(), f2function);
		LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, FONT6X6);


		y = 18;
		if (timer == 0) {
			len = snprintf(buf, sizeof(buf), "inf TIMER stop +");
		} else if (timer < 0) {
			len = snprintf(buf, sizeof(buf), "no timer    stop");
		} else {
			len = snprintf(buf, sizeof(buf), "- TIMER %3d:%02d +", timer / 60, timer % 60);
		}

		LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, FONT6X6);

		if (timer >= 0) {
			LCD_disp_str((uint8_t*)"F3", 2, 0, y, FONT6X6 | INVERT);
		}
		LCD_disp_str((uint8_t*)"F4", 2, LCD_ALIGN_RIGHT(2), y, FONT6X6 | INVERT);

		y = 27;
		if (timer > 0) {
			int time_left = Reflow_GetTimeLeft();
			if (Reflow_IsPreheating()) {
				len = snprintf(buf, sizeof(buf), "PREHEAT");
				LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, (blinkOn==1?FONT6X6|INVERT:FONT6X6));
			} else if (Reflow_IsDone() || time_left < 0) {
				len = snprintf(buf, sizeof(buf), "DONE");
				LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, (blinkOn==1?FONT6X6|INVERT:FONT6X6));
			} else {
				len = snprintf(buf, sizeof(buf), "%d:%02d", time_left / 60, time_left % 60);
				LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, FONT6X6);
			}
		}

		y = 36;
		len = snprintf(buf, sizeof(buf), "OVEN TEMP %3.1f`%s", display_temp(Sensor_GetTemp(TC_AVERAGE)), temp_unit());
		LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), y, FONT6X6);

		y = 44;
		len = snprintf(buf, sizeof(buf), "  L %3.1f`", display_temp(Sensor_GetTemp(TC_LEFT)));
		LCD_disp_str((uint8_t*)buf, len, 0, y, FONT6X6);
		len = snprintf(buf, sizeof(buf), "  R %3.1f`", display_temp(Sensor_GetTemp(TC_RIGHT)));
		LCD_disp_str((uint8_t*)buf, len, LCD_CENTER, y, FONT6X6);




		if (Sensor_IsValid(TC_EXTRA1) || Sensor_IsValid(TC_EXTRA2)) {
			y = 42;
			if (Sensor_IsValid(TC_EXTRA1)) {
				len = snprintf(buf, sizeof(buf), " X1 %3.1f`", display_temp(Sensor_GetTemp(TC_EXTRA1)));
				LCD_disp_str((uint8_t*)buf, len, 0, y, FONT6X6);
			}
			if (Sensor_IsValid(TC_EXTRA2)) {
				len = snprintf(buf, sizeof(buf), " X2 %3.1f`", display_temp(Sensor_GetTemp(TC_EXTRA2)));
				LCD_disp_str((uint8_t*)buf, len, LCD_CENTER, y, FONT6X6);
			}
		}

		y = 50;
		len = snprintf(buf, sizeof(buf), "COLD JUNCTION");
		LCD_disp_str((uint8_t*)buf, len, 0, 64-7, FONT6X6);

		y += 8;
		if (Sensor_IsValid(TC_COLD_JUNCTION)) {
			len = snprintf(buf, sizeof(buf), "%3.1f`", display_temp(Sensor_GetTemp(TC_COLD_JUNCTION)));
		} else {
			len = snprintf(buf, sizeof(buf), "ERR");
		}
		LCD_disp_str((uint8_t*)buf, len, 123-(7 * 6), 64-7, FONT6X6);

		//LCD_BMPDisplay(stopbmp, 127 - 17, 0);
		LCD_disp_str((uint8_t*)"  ", 2, 128-12, 64-7, FONT6X6 | INVERT);
		LCD_disp_str((uint8_t*)"S", 1, 128-9, 64-7, FONT6X6 | INVERT);

		Reflow_SetSetpoint(setpoint);

		if (timer > 0 && Reflow_IsDone()) {
			Buzzer_Beep(BUZZ_1KHZ, 255, TICKS_MS(100) * NV_GetConfig(REFLOW_BEEP_DONE_LEN));
			Reflow_SetBakeTimer(0);
			Reflow_SetMode(REFLOW_STANDBY);
		}

		if (keyspressed & KEY_F3 || keyspressed & KEY_F4) {
			if (timer == 0) {
				Reflow_SetMode(REFLOW_STANDBY);
			} else {
				if (timer == -1) {
					Reflow_SetBakeTimer(0);
				} else if (timer > 0) {
					Reflow_SetBakeTimer(timer);
					printf("\nSetting bake timer to %d\n", timer);
				}
				Reflow_SetMode(REFLOW_BAKE);
			}
		}

		// Abort bake
		if (keyspressed & KEY_S) {
			printf("\nEnd bake mode by keypress\n");

			mode = MAIN_HOME;
			Reflow_SetBakeTimer(0);
			Reflow_SetMode(REFLOW_STANDBY);
			retval = 0; // Force immediate refresh
		}




	// Edit profile
	} else if (mode == MAIN_EDIT_PROFILE) { // Edit ee1 or 2
		LCD_FB_Clear();
		int keyrepeataccel = keyspressed >> 17; // Divide the value by 2
		if (keyrepeataccel < 1)
			keyrepeataccel = 1;
		if (keyrepeataccel > 30)
			keyrepeataccel = 30;

		int16_t cursetpoint;
		Reflow_SelectEEProfileIdx(current_edit_profile);
		if (keyspressed & KEY_F1 && profile_time_idx > 0) { // Prev time
			profile_time_idx--;
		}
		if (keyspressed & KEY_F2 && profile_time_idx < 47) { // Next time
			profile_time_idx++;
		}
		cursetpoint = Reflow_GetSetpointAtIdx(profile_time_idx);

		if (keyspressed & KEY_F3) { // Decrease setpoint
			cursetpoint -= keyrepeataccel;
		}
		if (keyspressed & KEY_F4) { // Increase setpoint
			cursetpoint += keyrepeataccel;
		}
		if (cursetpoint < 0) cursetpoint = 0;
		if (cursetpoint > SETPOINT_MAX) cursetpoint = SETPOINT_MAX;
		Reflow_SetSetpointAtIdx(profile_time_idx, cursetpoint);

		Reflow_PlotProfile(profile_time_idx);
		LCD_BMPDisplay(editbmp, 127 - 17, 0);

		len = snprintf(buf, sizeof(buf), "%01u:%02u %03u`", (profile_time_idx*10)/60,(profile_time_idx*10)%60 , cursetpoint);
		LCD_disp_str((uint8_t*)buf, len, 33, 0, FONT6X6);

		// Done editing
		if (keyspressed & KEY_S) {
			Reflow_SaveEEProfile();
			mode = MAIN_HOME;
			retval = 0; // Force immediate refresh
		}



	// Bang-bang auto-tune
	} else if (mode == MAIN_BBTUNE) {
		LCD_FB_Clear();
		retval = TICKS_MS(250);

		BBTunePhase_t phase = Reflow_BBTune_GetPhase();

		if (phase == BBTUNE_PROMPT) {
			// Pre-start prompt
			showHeader("BB AUTO-TUNE");
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}

			len = snprintf(buf, sizeof(buf), "INSERT PCB FOR");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 16, FONT6X6);
			len = snprintf(buf, sizeof(buf), "BEST RESULTS");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 24, FONT6X6);
			len = snprintf(buf, sizeof(buf), "3 HEAT/COOL CYCLES");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 36, FONT6X6);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" START ", 7, 0, y, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)" BACK ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_F1) {
				TuneGraph_Reset();
				Reflow_BBTune_Start();
				retval = 0;
			}
			if (keyspressed & KEY_S) {
				mode = MAIN_SETUP;
				Reflow_SetMode(REFLOW_STANDBYFAN);
				retval = 0;
			}

		} else if (phase == BBTUNE_DONE) {
			// Show results
			showHeader("BB TUNE DONE");
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}

			len = snprintf(buf, sizeof(buf), "HEAT OFFSET: %dC", Reflow_BBTune_GetHeatOffset());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 18, FONT6X6);
			len = snprintf(buf, sizeof(buf), "COOL OFFSET: %dC", Reflow_BBTune_GetCoolOffset());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 28, FONT6X6);
			len = snprintf(buf, sizeof(buf), "SAVED TO MEMORY");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 42, FONT6X6 | INVERT);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" DONE ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_ANY) {
				Reflow_BBTune_Stop();
				mode = MAIN_HOME;
				retval = 0;
			}

		} else {
			// Tuning in progress — show live graph
			uint8_t heat, fan;
			Reflow_BBTune_Work(&heat, &fan);
			Set_Heater(heat);
			Set_Fan(fan);

			// Add temperature sample to rolling graph
			TuneGraph_AddSample(Sensor_GetTemp(TC_AVERAGE));

			// Header with cycle and phase
			const char* phasestr = "";
			switch (phase) {
				case BBTUNE_HEATING:    phasestr = "HEAT"; break;
				case BBTUNE_HEAT_COAST: phasestr = "PEAK"; break;
				case BBTUNE_COOLING:    phasestr = "COOL"; break;
				case BBTUNE_COOL_COAST: phasestr = "LOW"; break;
				default: break;
			}
			int cycle = Reflow_BBTune_GetCycle();
			len = snprintf(buf, sizeof(buf), "%d/%d %s %.0f`",
			               cycle + 1, BBTUNE_NUM_CYCLES, phasestr,
			               Sensor_GetTemp(TC_AVERAGE));
			showHeader(buf);

			// Draw graph with both target lines
			TuneGraph_Draw((float)BBTUNE_TARGET_HIGH, (float)BBTUNE_TARGET_LOW);

			// Bottom bar
			for(uint8_t n=0;n<128;n++) LCD_SetPixel(n, 64-9);
			int y = 64 - 7;
			len = snprintf(buf, sizeof(buf), "%d-%dC", BBTUNE_TARGET_LOW, BBTUNE_TARGET_HIGH);
			LCD_disp_str((uint8_t*)buf, len, 2, y, FONT6X6);
			LCD_disp_str((uint8_t*)" ABORT ", 7, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_S) {
				printf("\nBB Tune aborted by user\n");
				Reflow_BBTune_Stop();
				mode = MAIN_HOME;
				retval = 0;
			}
		}


	// TC offset auto-calibration
	} else if (mode == MAIN_TCCAL) {
		static int tccal_result = -1; // -1=not run yet
		LCD_FB_Clear();
		retval = TICKS_MS(250);

		showHeader("TC CALIBRATION");
		for(uint8_t n=0;n<128;n++){
			LCD_SetPixel(n,7);
			LCD_SetPixel(n,64-9);
		}

		if (tccal_result == -1) {
			// Not run yet — show prompt
			len = snprintf(buf, sizeof(buf), "ENSURE OVEN IS");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 14, FONT6X6);
			len = snprintf(buf, sizeof(buf), "COLD (BELOW %dC)", (int)TCCAL_MAX_TEMP);
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 22, FONT6X6);
			len = snprintf(buf, sizeof(buf), "CJ REF: %.1fC", Sensor_GetTemp(TC_COLD_JUNCTION));
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 34, FONT6X6);
			len = snprintf(buf, sizeof(buf), "L:%.1f R:%.1f", Sensor_GetTemp(TC_LEFT), Sensor_GetTemp(TC_RIGHT));
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 42, FONT6X6);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)"  CAL  ", 7, 0, y, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)" BACK ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_F1) {
				tccal_result = Sensor_AutoCalibrate();
				retval = 0;
			}
			if (keyspressed & KEY_S) {
				tccal_result = -1;
				mode = MAIN_SETUP;
				Reflow_SetMode(REFLOW_STANDBYFAN);
				retval = 0;
			}
		} else if (tccal_result == 0) {
			// Success
			len = snprintf(buf, sizeof(buf), "CALIBRATED OK");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 14, FONT6X6 | INVERT);
			len = snprintf(buf, sizeof(buf), "L ERR: %+.1fC", Sensor_GetCalError(0));
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 26, FONT6X6);
			len = snprintf(buf, sizeof(buf), "R ERR: %+.1fC", Sensor_GetCalError(1));
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 34, FONT6X6);
			len = snprintf(buf, sizeof(buf), "SAVED TO MEMORY");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 46, FONT6X6);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" DONE ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_ANY) {
				tccal_result = -1;
				mode = MAIN_HOME;
				retval = 0;
			}
		} else {
			// Error
			const char* errmsg = (tccal_result == 1) ? "OVEN TOO HOT" : "NO CJ SENSOR";
			len = snprintf(buf, sizeof(buf), "ERROR:");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 20, FONT6X6);
			len = snprintf(buf, sizeof(buf), "%s", errmsg);
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 30, FONT6X6 | INVERT);
			if (tccal_result == 1) {
				len = snprintf(buf, sizeof(buf), "LET COOL BELOW %dC", (int)TCCAL_MAX_TEMP);
				LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 42, FONT6X6);
			}

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" BACK ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_ANY) {
				tccal_result = -1;
				mode = MAIN_HOME;
				retval = 0;
			}
		}


	// PID auto-tune
	} else if (mode == MAIN_PIDTUNE) {
		LCD_FB_Clear();
		retval = TICKS_MS(250);

		PIDTunePhase_t phase = Reflow_PIDTune_GetPhase();

		if (phase == PIDTUNE_PROMPT) {
			showHeader("PID AUTO-TUNE");
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}

			len = snprintf(buf, sizeof(buf), "INSERT PCB FOR");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 16, FONT6X6);
			len = snprintf(buf, sizeof(buf), "BEST RESULTS");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 24, FONT6X6);
			len = snprintf(buf, sizeof(buf), "ZIEGLER-NICHOLS");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 36, FONT6X6);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" START ", 7, 0, y, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)" BACK ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_F1) {
				TuneGraph_Reset();
				Reflow_PIDTune_Start();
				retval = 0;
			}
			if (keyspressed & KEY_S) {
				mode = MAIN_SETUP;
				Reflow_SetMode(REFLOW_STANDBYFAN);
				retval = 0;
			}

		} else if (phase == PIDTUNE_DONE) {
			showHeader("PID TUNE DONE");
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,64-9);
			}

			len = snprintf(buf, sizeof(buf), "Kp: %.2f", Reflow_PIDTune_GetKp());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 14, FONT6X6);
			len = snprintf(buf, sizeof(buf), "Ki: %.4f", Reflow_PIDTune_GetKi());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 24, FONT6X6);
			len = snprintf(buf, sizeof(buf), "Kd: %.1f", Reflow_PIDTune_GetKd());
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 34, FONT6X6);
			len = snprintf(buf, sizeof(buf), "SAVED TO MEMORY");
			LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 46, FONT6X6 | INVERT);

			int y = 64 - 7;
			LCD_disp_str((uint8_t*)" DONE ", 6, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_ANY) {
				Reflow_PIDTune_Stop();
				Reflow_LoadPIDTuning(); // Apply new tuning immediately
				mode = MAIN_HOME;
				retval = 0;
			}

		} else {
			// Tuning in progress — show live graph
			uint8_t heat, fan;
			Reflow_PIDTune_Work(&heat, &fan);
			Set_Heater(heat);
			Set_Fan(fan);

			// Add temperature sample to rolling graph
			TuneGraph_AddSample(Sensor_GetTemp(TC_AVERAGE));

			// Header with cycle and phase
			const char* phasestr = "";
			switch (phase) {
				case PIDTUNE_SETTLING:    phasestr = "SETTLE"; break;
				case PIDTUNE_OSCILLATING: phasestr = "OSCIL"; break;
				default: break;
			}
			int cycle = Reflow_PIDTune_GetCycle();
			len = snprintf(buf, sizeof(buf), "%d/%d %s %.0f`",
			               cycle + 1, PIDTUNE_NUM_CYCLES, phasestr,
			               Sensor_GetTemp(TC_AVERAGE));
			showHeader(buf);

			// Draw graph with single target line
			TuneGraph_Draw((float)PIDTUNE_TARGET, 0);

			// Bottom bar
			for(uint8_t n=0;n<128;n++) LCD_SetPixel(n, 64-9);
			int y = 64 - 7;
			len = snprintf(buf, sizeof(buf), "TGT %dC", PIDTUNE_TARGET);
			LCD_disp_str((uint8_t*)buf, len, 2, y, FONT6X6);
			LCD_disp_str((uint8_t*)" ABORT ", 7, 91, y, FONT6X6 | INVERT);

			if (keyspressed & KEY_S) {
				printf("\nPID Tune aborted by user\n");
				Reflow_PIDTune_Stop();
				mode = MAIN_HOME;
				retval = 0;
			}
		}


	// Show screensaver
	} else if (mode == MAIN_SCREENSAVER) {

		if(animIY<58){
			if(modeChange){
				initSprites();
			}
			if(((++animIX)%4)==0 && (animIZ<5)){
				++animIZ;
			}
			LCD_ScrollDisplay(animIZ);
			animIY+=animIZ;
		}else{
			drawSprites();

			if (keyspressed & KEY_S) {
				retval=0;
				mode=MAIN_HOME;
			}
		}
		retval = TICKS_MS(50);	// approx 20fps - LCD panel on oven can't really update any quicker without looking terrible...

	// Main menu
	} else {
		if(modeChange){
			LCD_FB_Clear();
			initScreensaverTimeout();
			LCD_disp_str((uint8_t*)"F1", 2, 0,     (8 * 1)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"ABOUT", 5, 14, (8 * 1)+1, FONT6X6);
			LCD_disp_str((uint8_t*)"F2", 2, 0,     (8 * 2)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"SETUP", 5, 14, (8 * 2)+1, FONT6X6);
			LCD_disp_str((uint8_t*)"F3", 2, 0,     (8 * 3)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"BAKE/MANUAL MODE", 16, 14, (8 * 3)+1, FONT6X6);
			LCD_disp_str((uint8_t*)"F4", 2, 0, (8 * 4)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"SELECT PROFILE", 14, 14, (8 * 4)+1, FONT6X6);
			LCD_disp_str((uint8_t*)"  ", 2, 0, (8 * 5)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"S", 1, 3, (8 * 5)+1, FONT6X6 | INVERT);
			LCD_disp_str((uint8_t*)"RUN REFLOW PROFILE", 18, 14, (8 * 5)+1, FONT6X6);
			for(uint8_t n=0;n<128;n++){
				LCD_SetPixel(n,7);
				LCD_SetPixel(n,63-6);
			}
		}

		showHeader("MAIN MENU");

		len = snprintf(buf, sizeof(buf), "%s", Reflow_GetProfileName());
		LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), (8 * 6)+1, FONT6X6 | INVERT);

		len = snprintf(buf,sizeof(buf), " OVEN TEMPERATURE %d` ", Reflow_GetActualTemp());
		LCD_disp_str((uint8_t*)buf, len, LCD_ALIGN_CENTER(len), 64 - 6, FONT6X6);

		// Make sure reflow complete beep is silenced when pressing any key
		if (keyspressed) {
			Buzzer_Beep(BUZZ_NONE, 0, 0);
		}

		retval = TICKS_MS(100);

		// About
		if (keyspressed & KEY_F1) {
			mode = MAIN_ABOUT;
			retval = 0; // Force immediate refresh
		}
		if (keyspressed & KEY_F2) { // Setup/cal
			mode = MAIN_SETUP;
			Reflow_SetMode(REFLOW_STANDBYFAN);
			retval = 0; // Force immediate refresh
		}

		// Bake mode
		if (keyspressed & KEY_F3) {
			mode = MAIN_BAKE;
			Reflow_Init();
			retval = 0; // Force immediate refresh
		}

		// Select profile
		if (keyspressed & KEY_F4) {
			mode = MAIN_SELECT_PROFILE;
			retval = 0; // Force immediate refresh
		}

		// Start reflow
		if (keyspressed & KEY_S) {
			mode = MAIN_REFLOW;
			LCD_FB_Clear();
			printf("\nStarting reflow with profile: %s", Reflow_GetProfileName());
			Reflow_Init();
			Reflow_SetMode(REFLOW_REFLOW);
			retval = 0; // Force immediate refresh
		}

		if(timeForScreensaver()){
			mode=MAIN_SCREENSAVER;
		}
	}

	LCD_FB_Update();

	return retval;
}
