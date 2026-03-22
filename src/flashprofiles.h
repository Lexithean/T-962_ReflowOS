#ifndef FLASHPROFILES_H_
#define FLASHPROFILES_H_

#include <stdint.h>

// LPC2134 sector 1: 8KB at 0x2000-0x3FFF
// Each profile occupies one 256-byte block (IAP minimum write size)
#define FLASH_PROFILE_SECTOR     1
#define FLASH_PROFILE_BASE       0x2000
#define FLASH_PROFILE_END        0x4000
#define FLASH_PROFILE_BLOCK_SIZE 256
#define FLASH_PROFILE_MAX_SLOTS  32      // 8KB / 256 bytes = 32
#define FLASH_PROFILE_NAME_LEN   20      // Including null terminator
#define FLASH_PROFILE_MAGIC      0x50524F46  // "PROF"

// LPC2134 runs at 55.296 MHz
#define IAP_CCLK_KHZ             55296

// Initialize flash profile subsystem — scans sector for valid profiles
void FlashProfiles_Init(void);

// Get number of valid profiles currently stored
int FlashProfiles_GetCount(void);

// Get maximum capacity
int FlashProfiles_GetCapacity(void);

// Read profile from flash slot (0-based). Returns 0 on success, -1 if invalid.
int FlashProfiles_Read(int slot, uint16_t temps[48], char name[FLASH_PROFILE_NAME_LEN]);

// Write profile to flash slot. Erases sector and rewrites all profiles.
int FlashProfiles_WriteProfile(int slot, const uint16_t temps[48], const char* name);

// Delete profile from flash slot. Returns 0 on success.
int FlashProfiles_Delete(int slot);

// Check if a slot contains a valid profile
int FlashProfiles_IsValid(int slot);

// Get the name of a flash profile (returns pointer to flash, or NULL)
const char* FlashProfiles_GetName(int slot);

// Print all profiles (EEPROM + flash) as import-compatible backup text
void FlashProfiles_BackupAll(void);

#endif /* FLASHPROFILES_H_ */
