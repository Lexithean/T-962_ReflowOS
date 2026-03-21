/*
 * nvstorage.c - Persistent settings storage for T-962 reflow controller
 *
 * Copyright (C) 2011,2014 Werner Johansson, wj@unifiedengineering.se
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

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "nvstorage.h"
#include "eeprom.h"
#include "sched.h"

#define NVMAGIC (('W' << 8) | 'J')
#define NV_STORAGE_OFFSET 0xC4  // 196 (60 bytes available before end of 256B EEPROM)
#define NV_STORAGE_OFFSET_OLD 0x62 // 98 (Overlapped with Profile 2)

typedef struct __attribute__ ((__packed__)) {
	uint16_t magic;
	uint8_t numitems;
	uint8_t config[NVITEM_NUM_ITEMS];
} NV_t;

static NV_t myNV; // RAM copy of the NV data

static uint8_t nvupdatepending=0;

static void SetNVUpdatePending(void) {
	nvupdatepending = 1;
	Sched_SetState(NV_WORK, 1, 0);
}

void NV_Init(void) {
	// 1. Try to read from the NEW location
	EEPROM_Read((uint8_t*)&myNV, NV_STORAGE_OFFSET, sizeof(myNV));

	if (myNV.magic != NVMAGIC) {
		// 2. NEW location is empty. Check if there's data in the OLD location (Migration)
		EEPROM_Read((uint8_t*)&myNV, NV_STORAGE_OFFSET_OLD, sizeof(myNV));

		if (myNV.magic == NVMAGIC) {
			printf("\nNV: Migrating data to safe EEPROM location...");

			// A. Move Profile 2 first (from 130 to 98)
			uint8_t profBuf[98];
			EEPROM_Read(profBuf, 130, 98); // Legacy P2 start
			EEPROM_Write(98, profBuf, 98); // New P2 start (immediately after P1)

			// B. Move Settings to new location (196) and update numitems if needed
			if (myNV.numitems < NVITEM_NUM_ITEMS) {
				uint8_t bytestoclear = NVITEM_NUM_ITEMS - myNV.numitems;
				memset(myNV.config + myNV.numitems, 0xff, bytestoclear);
				myNV.numitems = NVITEM_NUM_ITEMS;
			}
			EEPROM_Write(NV_STORAGE_OFFSET, (uint8_t*)&myNV, sizeof(myNV));

			// C. Clear old magic to finalize migration
			uint16_t clearMagic = 0x0000;
			EEPROM_Write(NV_STORAGE_OFFSET_OLD, (uint8_t*)&clearMagic, 2);

			printf(" Done.");
		} else {
			// 3. Brand new initialization
			myNV.magic = NVMAGIC;
			myNV.numitems = NVITEM_NUM_ITEMS;
			memset(myNV.config, 0xff, NVITEM_NUM_ITEMS);
			printf("\nNV initialization cleared %d items", NVITEM_NUM_ITEMS);
			SetNVUpdatePending();
		}
	} else if(myNV.numitems < NVITEM_NUM_ITEMS) {
		// Existing settings at new location, just adding new items
		uint8_t bytestoclear = NVITEM_NUM_ITEMS - myNV.numitems;
		memset(myNV.config + myNV.numitems, 0xff, bytestoclear);
		myNV.numitems = NVITEM_NUM_ITEMS;
		printf("\nNV upgrade cleared %d new items", bytestoclear);
		SetNVUpdatePending();
	}
#ifndef MINIMALISTIC
	Sched_SetWorkfunc(NV_WORK,NV_Work);
	Sched_SetState(NV_WORK, 1, 0);
#endif
}

uint8_t NV_GetConfig(NVItem_t item) {
	if (item < NVITEM_NUM_ITEMS) {
		return myNV.config[item];
	} else {
		return 0;
	}
}

void NV_SetConfig(NVItem_t item, uint8_t value) {
	if (item < NVITEM_NUM_ITEMS) {
		if (value != myNV.config[item]) {
			myNV.config[item] = value;
			SetNVUpdatePending();
		}
	}
}

// Periodic updater of NV
int32_t NV_Work(void) {
	static uint8_t count = 0;
	if (nvupdatepending) count ++;
	if (count == 4) {
		nvupdatepending = count = 0;
		printf("\nFlushing NV copy to EE...");
		EEPROM_Write(NV_STORAGE_OFFSET, (uint8_t*)&myNV, sizeof(myNV));
	}
	return nvupdatepending ? (TICKS_SECS(2)) : -1;
}
