#include "LPC214x.h"
#include <stdint.h>
#include <stdio.h>
#include "t962.h"
#include "lcd.h"
#include "nvstorage.h"
#include "eeprom.h"
#include "reflow.h"

#include "reflow_profiles.h"
#include "flashprofiles.h"

#define RAMPTEST
#define PIDTEST

extern uint8_t graphbmp[];

// SAC305 profile
static const profile sac305profile = {
	"SAC305 Leadfree", {
		// Preheat
		50,  65,  80,  90,  92,  94,  96,  97,  98,  99, 100, 100,
		105, 115, 125, 135, 140,
		// Flux activation
		145, 150, 155, 160, 165, 170, 175, 180, 
		// reflow peak
		195, 210, 235, 245, 225, 205, 185, 165,
		// cool down
		155, 145, 140, 135, 130, 125, 120, 115, 110, 105, 100,
		0,   0 // 360-470 s
	}
};

// Amtech 4300 63Sn/37Pb leaded profile
static const profile am4300profile = {
	"4300 63SN/37PB", {
		 50, 50, 50, 60, 73, 86,100,113,126,140,143,147,150,154,157,161, // 0-150s
		164,168,171,175,179,183,195,207,215,207,195,183,168,154,140,125, // Adjust peak from 205 to 220C
		111, 97, 82, 68, 54,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0  // 320-470s
	}
};

// NC-31 low-temp lead-free profile
static const profile nc31profile = {
	"NC-31 LOW-TEMP LF", {
		 50, 50, 50, 50, 55, 70, 85, 90, 95,100,102,105,107,110,112,115, // 0-150s
		117,120,122,127,132,138,148,158,160,158,148,138,130,122,114,106, // Adjust peak from 158 to 165C
		 98, 90, 82, 74, 66, 58,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0  // 320-470s
	}
};

// SynTECH-LF normal temp lead-free profile
static const profile syntechlfprofile = {
	"AMTECH SYNTECH-LF", {
		 50, 50, 50, 50, 60, 70, 80, 90,100,110,120,130,140,149,158,166, // 0-150s
		175,184,193,201,210,219,230,240,245,240,230,219,212,205,198,191, // Adjust peak from 230 to 249C
		184,177,157,137,117, 97, 77, 57,  0,  0,  0,  0,  0,  0,  0,  0  // 320-470s
	}
};

#ifdef RAMPTEST
// Ramp speed test temp profile
static const profile rampspeed_testprofile = {
	"RAMP SPEED TEST", {
		 50, 50, 50, 50,245,245,245,245,245,245,245,245,245,245,245,245, // 0-150s
		245,245,245,245,245,245,245,245,245, 50, 50, 50, 50, 50, 50, 50, // 160-310s
		 50, 50, 50, 50, 50, 50, 50, 50,  0,  0,  0,  0,  0,  0,  0,  0  // 320-470s
	}
};
#endif

#ifdef PIDTEST
// PID gain adjustment test profile (5% setpoint change)
static const profile pidcontrol_testprofile = {
	"PID CONTROL TEST",	{
		171,171,171,171,171,171,171,171,171,171,171,171,171,171,171,171, // 0-150s
		180,180,180,180,180,180,180,180,171,171,171,171,171,171,171,171, // 160-310s
		  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0  // 320-470s
	}
};
#endif

// EEPROM profile 1
static char ee1_name[20] = "CUSTOM #1";
static ramprofile ee1 = { "CUSTOM #1" };

// EEPROM profile 2
static char ee2_name[20] = "CUSTOM #2";
static ramprofile ee2 = { "CUSTOM #2" };

static const profile* profiles[] = {
	&sac305profile,
	&syntechlfprofile,
	&nc31profile,
	&am4300profile,
#ifdef RAMPTEST
	&rampspeed_testprofile,
#endif
#ifdef PIDTEST
	&pidcontrol_testprofile,
#endif
	(profile*) &ee1,
	(profile*) &ee2
};

#define NUMPROFILES (sizeof(profiles) / sizeof(profiles[0]))

// current profile index
static uint8_t profileidx = 0;

// Flash profile shadow — holds the currently-loaded flash profile in RAM
static char flash_shadow_name[FLASH_PROFILE_NAME_LEN];
static ramprofile flash_shadow = { flash_shadow_name };
static int flash_shadow_slot = -1;  // which flash slot is loaded, -1 = none

// Get the Nth valid flash slot (skipping empty slots)
// Returns the flash slot number, or -1 if n is out of range
static int flash_nth_valid(int n) {
	int count = 0;
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		if (FlashProfiles_IsValid(i)) {
			if (count == n) return i;
			count++;
		}
	}
	return -1;
}

// Load a flash profile into the shadow buffer
static void load_flash_shadow(int slot) {
	if (slot == flash_shadow_slot) return; // already loaded
	if (FlashProfiles_Read(slot, flash_shadow.temperatures, flash_shadow_name) == 0) {
		flash_shadow.name = flash_shadow_name;
		flash_shadow_slot = slot;
	}
}

static void ByteswapTempProfile(uint16_t* buf) {
	for (int i = 0; i < NUMPROFILETEMPS; i++) {
		uint16_t word = buf[i];
		buf[i] = word >> 8 | word << 8;
	}
}

void Reflow_LoadCustomProfiles(void) {
	EEPROM_Read((uint8_t*)ee1.temperatures, 2, 96);
	ByteswapTempProfile(ee1.temperatures);

	EEPROM_Read((uint8_t*)ee2.temperatures, 98 + 2, 96); // Moved from 128+2 to 98+2
	ByteswapTempProfile(ee2.temperatures);
}

void Reflow_ValidateNV(void) {
	if (NV_GetConfig(REFLOW_BEEP_DONE_LEN) == 255) {
		// Default 1 second beep length
		NV_SetConfig(REFLOW_BEEP_DONE_LEN, 10);
	}

	if (NV_GetConfig(REFLOW_MIN_FAN_SPEED) == 255) {
		// Default fan speed is now 8
		NV_SetConfig(REFLOW_MIN_FAN_SPEED, 8);
	}

	if (NV_GetConfig(REFLOW_BAKE_SETPOINT_H) == 255 || NV_GetConfig(REFLOW_BAKE_SETPOINT_L) == 255) {
		NV_SetConfig(REFLOW_BAKE_SETPOINT_H, SETPOINT_DEFAULT >> 8);
		NV_SetConfig(REFLOW_BAKE_SETPOINT_L, (uint8_t)SETPOINT_DEFAULT);
		printf("Resetting bake setpoint to default.");
	}

	if (NV_GetConfig(REFLOW_PREHEAT_TEMP) == 255) {
		NV_SetConfig(REFLOW_PREHEAT_TEMP, 20); // Default 50C (20 + 30 offset)
	}

	if (NV_GetConfig(REFLOW_BANGBANG_MODE) == 255) {
		NV_SetConfig(REFLOW_BANGBANG_MODE, 0); // Default OFF (PID control)
	}

	if (NV_GetConfig(REFLOW_BB_HEAT_OFFSET) == 255) {
		NV_SetConfig(REFLOW_BB_HEAT_OFFSET, 0); // Default: no anticipatory offset
	}

	if (NV_GetConfig(REFLOW_BB_COOL_OFFSET) == 255) {
		NV_SetConfig(REFLOW_BB_COOL_OFFSET, 0); // Default: no anticipatory offset
	}

	if (NV_GetConfig(SAFETY_RUNAWAY_THRESH) == 255) {
		NV_SetConfig(SAFETY_RUNAWAY_THRESH, 30); // Default: 30°C above setpoint
	}

	if (NV_GetConfig(REFLOW_BUZZER_ALERTS) == 255) {
		NV_SetConfig(REFLOW_BUZZER_ALERTS, 1); // Default: ON
	}

	if (NV_GetConfig(REFLOW_MAX_COOL_RATE) == 255) {
		NV_SetConfig(REFLOW_MAX_COOL_RATE, 0); // Default: unlimited (no rate limiting)
	}

	if (NV_GetConfig(REFLOW_FAN_KICKSTART) == 255) {
		NV_SetConfig(REFLOW_FAN_KICKSTART, 0); // Default: OFF
	}

	Reflow_SelectProfileIdx(NV_GetConfig(REFLOW_PROFILE));
}

int Reflow_GetProfileIdx(void) {
	return profileidx;
}

int Reflow_GetTotalProfileCount(void) {
	return NUMPROFILES + FlashProfiles_GetCount();
}

int Reflow_IsFlashProfile(void) {
	return profileidx >= NUMPROFILES;
}

int Reflow_SelectProfileIdx(int idx) {
	int total = Reflow_GetTotalProfileCount();
	if (total == 0) total = NUMPROFILES; // safety
	if (idx < 0) {
		idx = total - 1;
	} else if (idx >= total) {
		idx = 0;
	}
	profileidx = idx;

	// If in flash range, load the profile into shadow
	if (profileidx >= NUMPROFILES) {
		int flash_n = profileidx - NUMPROFILES;
		int slot = flash_nth_valid(flash_n);
		if (slot >= 0) {
			load_flash_shadow(slot);
		} else {
			// Invalid — wrap back to first profile
			profileidx = 0;
		}
	}

	NV_SetConfig(REFLOW_PROFILE, profileidx);
	return profileidx;
}

int Reflow_SelectEEProfileIdx(int idx) {
	if (idx == 1) {
		profileidx = (NUMPROFILES - 2);
	} else if (idx == 2) {
		profileidx = (NUMPROFILES - 1);
	}
	return profileidx;
}

int Reflow_GetEEProfileIdx(void) {
	if (profileidx == (NUMPROFILES - 2)) {
		return 1;
	} else 	if (profileidx == (NUMPROFILES - 1)) {
		return 2;
	} else {
		return 0;
	}
}

int Reflow_SaveEEProfile(void) {
	int retval = 0;
	uint8_t offset;
	uint16_t* tempptr;
	if (profileidx == (NUMPROFILES - 2)) {
		offset = 0;
		tempptr = ee1.temperatures;
	} else if (profileidx == (NUMPROFILES - 1)) {
		offset = 98; // Moved from 128 to 98
		tempptr = ee2.temperatures;
	} else {
		return -1;
	}
	offset += 2; // Skip "magic"
	ByteswapTempProfile(tempptr);

	// Store profile
	retval = EEPROM_Write(offset, (uint8_t*)tempptr, 96);
	ByteswapTempProfile(tempptr);
	return retval;
}

void Reflow_ListProfiles(void) {
	for (int i = 0; i < NUMPROFILES; i++) {
		printf("%d: %s\n", i, profiles[i]->name);
	}
	int fc = FlashProfiles_GetCount();
	if (fc > 0) {
		printf("--- Flash profiles ---\n");
		for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
			if (FlashProfiles_IsValid(i)) {
				printf("F%d: %s\n", i, FlashProfiles_GetName(i));
			}
		}
	}
}

const char* Reflow_GetProfileName(void) {
	if (profileidx >= NUMPROFILES) {
		return flash_shadow.name;
	}
	return profiles[profileidx]->name;
}

uint16_t Reflow_GetSetpointAtIdx(uint8_t idx) {
	if (idx > (NUMPROFILETEMPS - 1)) {
		return 0;
	}
	if (profileidx >= NUMPROFILES) {
		return flash_shadow.temperatures[idx];
	}
	return profiles[profileidx]->temperatures[idx];
}

void Reflow_SetSetpointAtIdx(uint8_t idx, uint16_t value) {
	if (idx > (NUMPROFILETEMPS - 1)) { return; }
	if (value > SETPOINT_MAX) { return; }
	if (profileidx >= NUMPROFILES) { return; } // Flash profiles are read-only

	uint16_t* temp = (uint16_t*) &profiles[profileidx]->temperatures[idx];
	if (temp >= (uint16_t*)0x40000000) {
		*temp = value; // If RAM-based
	}
}

void Reflow_PlotProfile(int highlight) {
	LCD_BMPDisplay(graphbmp, 0, 0);

	for(int x = 0; x < NUMPROFILETEMPS; x++) {
		int realx = (x << 1) + XAXIS;
		int y = Reflow_GetSetpointAtIdx(x) / 5;
		y = YAXIS - y;
		LCD_SetPixel(realx, y);

		if (highlight == x) {
			for(uint8_t xPos=XAXIS;xPos<109;xPos++){
				LCD_SetPixel(xPos, y);
			}
			for(uint8_t yPos=7;yPos<YAXIS;yPos++){
				LCD_SetPixel(realx, yPos);
			}
		}
	}
}

void Reflow_DumpProfile(int profile) {
	if (profile > NUMPROFILES) {
		printf("\nNo profile with id: %d\n", profile);
		return;
	}

	int current = profileidx;
	profileidx = profile;

	for (int i = 0; i < NUMPROFILETEMPS; i++) {
		printf("%4d,", Reflow_GetSetpointAtIdx(i));
		if (i == 15 || i == 31) {
			printf("\n ");
		}
	}
	printf("\n");
	profileidx = current;
}

void Reflow_ExportProfile(int profile) {
	if (profile >= NUMPROFILES) {
		printf("\nNo profile with id: %d\n", profile);
		return;
	}

	int current = profileidx;
	profileidx = profile;

	// Output in import-compatible format
	printf("\nimport profile %d ", Reflow_GetEEProfileIdx() > 0 ? Reflow_GetEEProfileIdx() : 1);
	for (int i = 0; i < NUMPROFILETEMPS; i++) {
		if (i > 0) printf(",");
		printf("%d", Reflow_GetSetpointAtIdx(i));
	}
	printf("\n");
	profileidx = current;
}

void Reflow_SetProfileName(int eeIdx, const char* name) {
	char* dest;
	ramprofile* rp;
	if (eeIdx == 1) {
		dest = ee1_name;
		rp = &ee1;
	} else if (eeIdx == 2) {
		dest = ee2_name;
		rp = &ee2;
	} else {
		return;
	}
	// Copy name, max 18 chars + null
	int i;
	for (i = 0; i < 18 && name[i] != '\0'; i++) {
		dest[i] = name[i];
	}
	dest[i] = '\0';
	rp->name = dest;
}

