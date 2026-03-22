/*
 * flashprofiles.c - IAP flash profile storage for T-962 reflow controller
 *
 * Stores reflow profiles in LPC2134 flash sector 7 (8KB at 0xE000).
 * Each profile occupies a 256-byte block. Supports up to 32 profiles.
 *
 * Uses NXP IAP (In-Application Programming) ROM routines for erase/write.
 * Cache-erase-rewrite pattern: all valid profiles are cached in RAM,
 * sector is erased, then profiles are written back.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "flashprofiles.h"
#include "reflow_profiles.h"
#include "vic.h"

// IAP ROM entry point
typedef void (*IAP)(unsigned int[], unsigned int[]);
static IAP iap_entry = (void*)0x7ffffff1;

// IAP command codes
#define IAP_PREPARE_SECTORS  50
#define IAP_COPY_RAM_FLASH   51
#define IAP_ERASE_SECTORS    52

// On-flash profile block layout (256 bytes)
typedef struct {
	uint32_t magic;
	char     name[FLASH_PROFILE_NAME_LEN];
	uint16_t temps[48];                     // 96 bytes
	uint8_t  _reserved[256 - 4 - FLASH_PROFILE_NAME_LEN - 96];
} FlashProfileBlock_t;

// Validity cache (scanned at init)
static uint8_t profile_valid[FLASH_PROFILE_MAX_SLOTS];
static int profile_count = 0;

// 256-byte write buffer — must be word-aligned for IAP
static uint8_t __attribute__((aligned(4))) flash_write_buf[FLASH_PROFILE_BLOCK_SIZE];

// ---- Internal helpers ----

static const FlashProfileBlock_t* flash_profile_ptr(int slot) {
	return (const FlashProfileBlock_t*)((uintptr_t)FLASH_PROFILE_BASE +
		(uint32_t)slot * FLASH_PROFILE_BLOCK_SIZE);
}

static int iap_erase_sector(void) {
	unsigned int command[5], result[3];
	uint32_t save = VIC_DisableIRQ();

	// Prepare sector (must stay valid until erase)
	command[0] = IAP_PREPARE_SECTORS;
	command[1] = FLASH_PROFILE_SECTOR;
	command[2] = FLASH_PROFILE_SECTOR;
	iap_entry(command, result);
	if (result[0] != 0) {
		VIC_RestoreIRQ(save);
		printf("\n[IAP] Prepare for erase failed: code %u\n", result[0]);
		return -1;
	}

	// Erase sector
	command[0] = IAP_ERASE_SECTORS;
	command[1] = FLASH_PROFILE_SECTOR;
	command[2] = FLASH_PROFILE_SECTOR;
	command[3] = IAP_CCLK_KHZ;
	iap_entry(command, result);

	VIC_RestoreIRQ(save);
	if (result[0] != 0) {
		printf("\n[IAP] Erase failed: code %u\n", result[0]);
	}
	return (result[0] == 0) ? 0 : -1;
}

static int iap_write_block(uint32_t flash_addr, uint8_t* data) {
	unsigned int command[5], result[3];
	uint32_t save = VIC_DisableIRQ();

	// Prepare sector (must stay valid until copy)
	command[0] = IAP_PREPARE_SECTORS;
	command[1] = FLASH_PROFILE_SECTOR;
	command[2] = FLASH_PROFILE_SECTOR;
	iap_entry(command, result);
	if (result[0] != 0) {
		VIC_RestoreIRQ(save);
		printf("\n[IAP] Prepare for write failed: code %u\n", result[0]);
		return -1;
	}

	// Copy RAM to Flash
	command[0] = IAP_COPY_RAM_FLASH;
	command[1] = flash_addr;
	command[2] = (uintptr_t)data;
	command[3] = FLASH_PROFILE_BLOCK_SIZE;
	command[4] = IAP_CCLK_KHZ;
	iap_entry(command, result);

	VIC_RestoreIRQ(save);
	if (result[0] != 0) {
		printf("\n[IAP] Write to 0x%lX failed: code %u\n", (unsigned long)flash_addr, result[0]);
	}
	return (result[0] == 0) ? 0 : -1;
}

// ---- Cache for sector rewrite ----
// Compact cache — only stores the essential data (not the full 256-byte block)
typedef struct {
	char     name[FLASH_PROFILE_NAME_LEN];
	uint16_t temps[48];
} ProfileCache_t;  // ~116 bytes

// Cache, erase, rewrite the entire sector with one slot changed
static int rewrite_sector(int target_slot, const uint16_t* new_temps,
                          const char* new_name, int deleting) {
	static ProfileCache_t cache[FLASH_PROFILE_MAX_SLOTS]; // ~3.7KB in BSS
	int cache_valid[FLASH_PROFILE_MAX_SLOTS];
	memset(cache_valid, 0, sizeof(cache_valid));

	// Step 1: Cache all existing profiles from flash
	for (int i = 0; i < FLASH_PROFILE_MAX_SLOTS; i++) {
		if (i == target_slot) {
			if (!deleting && new_temps) {
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
	int target_ok = 0;
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
		if (i == target_slot) target_ok = 1;
	}
	profile_count = new_count;
	return target_ok ? 0 : -1;
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
	printf("\nFlash profiles: %d/%d slots used (sector %d @ 0x%X)",
		profile_count, FLASH_PROFILE_MAX_SLOTS,
		FLASH_PROFILE_SECTOR, FLASH_PROFILE_BASE);
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

	// EEPROM profiles (survive reflashing)
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
