/*
 * flashprofiles.c - IAP flash profile storage for T-962
 *
 * Stores additional reflow profiles in flash sector 10 (0x18000-0x1FFFF).
 * Profiles persist across power cycles but are ERASED on firmware update.
 * Use the 'backup' serial command to dump all profiles before updating.
 *
 * LPC2134 IAP requires interrupts disabled during flash operations.
 * Each profile uses one 256-byte block (IAP minimum write size).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "flashprofiles.h"
#include "reflow_profiles.h"
#include "vic.h"

// IAP function pointer (same entry point as io.c)
typedef void (*IAP_t)(unsigned int [], unsigned int[]);
static IAP_t iap_entry = (void*)0x7ffffff1;

// IAP command codes
#define IAP_PREPARE_SECTORS   50
#define IAP_COPY_RAM_TO_FLASH 51
#define IAP_ERASE_SECTORS     52

// System clock in kHz for IAP (55.296 MHz)
#define IAP_CCLK_KHZ          55296

// RAM buffer for flash writes — must be word-aligned, 256 bytes
static uint8_t flash_write_buf[FLASH_PROFILE_BLOCK_SIZE] __attribute__((aligned(4)));

// RAM cache of profile validity (scanned at init)
static uint8_t profile_valid[FLASH_PROFILE_MAX_SLOTS];
static int profile_count = 0;

// Profile block structure packed into 256 bytes
typedef struct __attribute__((packed)) {
	uint32_t magic;                          // FLASH_PROFILE_MAGIC
	char     name[FLASH_PROFILE_NAME_LEN];   // Null-terminated
	uint16_t temps[48];                      // Temperature data
	uint8_t  reserved[256 - 4 - 20 - 96];   // Pad to 256
} FlashProfileBlock_t;

// Get pointer to a profile block in flash (read-only)
static const FlashProfileBlock_t* flash_profile_ptr(int slot) {
	uint32_t addr = FLASH_PROFILE_BASE + (uint32_t)slot * FLASH_PROFILE_BLOCK_SIZE;
	return (const FlashProfileBlock_t*)addr;
}

// IAP helper: prepare sector 10 for write/erase
static int iap_prepare(void) {
	unsigned int command[5], result[4];
	command[0] = IAP_PREPARE_SECTORS;
	command[1] = FLASH_PROFILE_SECTOR;
	command[2] = FLASH_PROFILE_SECTOR;
	uint32_t save = VIC_DisableIRQ();
	iap_entry(command, result);
	VIC_RestoreIRQ(save);
	return (result[0] == 0) ? 0 : -1;
}

// IAP helper: erase sector 10
static int iap_erase_sector(void) {
	if (iap_prepare() != 0) return -1;
	unsigned int command[5], result[4];
	command[0] = IAP_ERASE_SECTORS;
	command[1] = FLASH_PROFILE_SECTOR;
	command[2] = FLASH_PROFILE_SECTOR;
	command[3] = IAP_CCLK_KHZ;
	uint32_t save = VIC_DisableIRQ();
	iap_entry(command, result);
	VIC_RestoreIRQ(save);
	return (result[0] == 0) ? 0 : -1;
}

// IAP helper: write 256 bytes from RAM buffer to flash address
static int iap_write_block(uint32_t flash_addr, const uint8_t* data) {
	if (iap_prepare() != 0) return -1;
	unsigned int command[5], result[4];
	command[0] = IAP_COPY_RAM_TO_FLASH;
	command[1] = flash_addr;
	command[2] = (uintptr_t)data;
	command[3] = FLASH_PROFILE_BLOCK_SIZE;
	command[4] = IAP_CCLK_KHZ;
	uint32_t save = VIC_DisableIRQ();
	iap_entry(command, result);
	VIC_RestoreIRQ(save);
	return (result[0] == 0) ? 0 : -1;
}

// ---- Cache for sector rewrite ----
// We must read all profiles BEFORE erasing, since erase wipes the sector.
typedef struct {
	uint32_t magic;
	char     name[FLASH_PROFILE_NAME_LEN];
	uint16_t temps[48];
} ProfileCache_t;

// Cache, erase, rewrite the entire sector with one slot changed
static int rewrite_sector(int target_slot, const uint16_t* new_temps, const char* new_name, int deleting) {
	static ProfileCache_t cache[FLASH_PROFILE_MAX_SLOTS];
	int cache_valid[FLASH_PROFILE_MAX_SLOTS];
	memset(cache_valid, 0, sizeof(cache_valid));

	// Step 1: Cache all existing profiles from flash
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		if (i == target_slot) {
			if (!deleting && new_temps) {
				cache[i].magic = FLASH_PROFILE_MAGIC;
				if (new_name) {
					strncpy(cache[i].name, new_name, FLASH_PROFILE_NAME_LEN - 1);
					cache[i].name[FLASH_PROFILE_NAME_LEN - 1] = '\0';
				} else {
					snprintf(cache[i].name, FLASH_PROFILE_NAME_LEN, "Flash #%d", i);
				}
				memcpy(cache[i].temps, new_temps, sizeof(uint16_t) * 48);
				cache_valid[i] = 1;
			}
		} else if (profile_valid[i]) {
			const FlashProfileBlock_t* p = flash_profile_ptr(i);
			cache[i].magic = p->magic;
			memcpy(cache[i].name, p->name, FLASH_PROFILE_NAME_LEN);
			memcpy(cache[i].temps, p->temps, sizeof(uint16_t) * 48);
			cache_valid[i] = 1;
		}
	}

	// Step 2: Erase the sector
	if (iap_erase_sector() != 0) {
		printf("\n[ERROR] Flash erase failed\n");
		return -1;
	}

	// Step 3: Write all cached profiles back
	int new_count = 0;
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		if (!cache_valid[i]) {
			profile_valid[i] = 0;
			continue;
		}
		memset(flash_write_buf, 0xFF, FLASH_PROFILE_BLOCK_SIZE);
		FlashProfileBlock_t* blk = (FlashProfileBlock_t*)flash_write_buf;
		blk->magic = FLASH_PROFILE_MAGIC;
		memcpy(blk->name, cache[i].name, FLASH_PROFILE_NAME_LEN);
		memcpy(blk->temps, cache[i].temps, sizeof(uint16_t) * 48);

		uint32_t addr = FLASH_PROFILE_BASE + (uint32_t)i * FLASH_PROFILE_BLOCK_SIZE;
		if (iap_write_block(addr, flash_write_buf) != 0) {
			printf("\n[ERROR] Flash write failed at slot %d\n", i);
			profile_valid[i] = 0;
			continue;
		}
		profile_valid[i] = 1;
		new_count++;
	}
	profile_count = new_count;
	return 0;
}

// ---- Public API ----

void FlashProfiles_Init(void) {
	profile_count = 0;
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		const FlashProfileBlock_t* p = flash_profile_ptr(i);
		if (p->magic == FLASH_PROFILE_MAGIC) {
			profile_valid[i] = 1;
			profile_count++;
		} else {
			profile_valid[i] = 0;
		}
	}
	printf("\nFlash profiles: %d/%d slots used\n", profile_count, FLASH_PROFILE_MAX_SLOTS);
}

int FlashProfiles_GetCount(void) { return profile_count; }
int FlashProfiles_GetCapacity(void) { return FLASH_PROFILE_MAX_SLOTS; }

int FlashProfiles_IsValid(int slot) {
	if (slot < 0 || slot >= FLASH_PROFILE_MAX_SLOTS) return 0;
	return profile_valid[slot];
}

const char* FlashProfiles_GetName(int slot) {
	if (!FlashProfiles_IsValid(slot)) return NULL;
	return flash_profile_ptr(slot)->name;
}

int FlashProfiles_Read(int slot, uint16_t temps[48], char name[FLASH_PROFILE_NAME_LEN]) {
	if (!FlashProfiles_IsValid(slot)) return -1;
	const FlashProfileBlock_t* p = flash_profile_ptr(slot);
	memcpy(temps, p->temps, sizeof(uint16_t) * 48);
	if (name) {
		memcpy(name, p->name, FLASH_PROFILE_NAME_LEN);
		name[FLASH_PROFILE_NAME_LEN - 1] = '\0';
	}
	return 0;
}

int FlashProfiles_WriteProfile(int slot, const uint16_t temps[48], const char* name) {
	if (slot < 0 || slot >= FLASH_PROFILE_MAX_SLOTS) return -1;
	return rewrite_sector(slot, temps, name, 0);
}

int FlashProfiles_Delete(int slot) {
	if (slot < 0 || slot >= FLASH_PROFILE_MAX_SLOTS) return -1;
	if (!profile_valid[slot]) return 0;
	return rewrite_sector(slot, NULL, NULL, 1);
}

void FlashProfiles_BackupAll(void) {
	printf("\n# T-962 Profile Backup\n");
	printf("# Paste this into serial after reflashing to restore.\n");

	// EEPROM profiles (survive reflashing, backup anyway for completeness)
	printf("# --- EEPROM profiles ---\n");
	Reflow_ExportProfile(5);  // CUSTOM#1
	Reflow_ExportProfile(6);  // CUSTOM#2

	// Flash profiles (erased on firmware update)
	printf("# --- Flash profiles (lost on update) ---\n");
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		if (!profile_valid[i]) continue;
		const FlashProfileBlock_t* p = flash_profile_ptr(i);
		printf("save flash %d ", i);
		for (int t = 0; t < 48; t++) {
			if (t > 0) printf(",");
			printf("%d", p->temps[t]);
		}
		printf(",%s\n", p->name);
	}
	printf("# --- End of backup ---\n");
}
