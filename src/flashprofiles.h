#ifndef FLASHPROFILES_H_
#define FLASHPROFILES_H_

#include <stdint.h>

// Profile storage in flash sector 19 (0x1E000-0x1FFFF, 8KB)
// Each profile occupies one 256-byte block (IAP minimum write size)
#define FLASH_PROFILE_SECTOR     19
#define FLASH_PROFILE_BASE       0x1E000
#define FLASH_PROFILE_END        0x20000
#define FLASH_PROFILE_BLOCK_SIZE 256
#define FLASH_PROFILE_MAX_SLOTS  30      // Reasonable cap (could fit 128)
#define FLASH_PROFILE_NAME_LEN   20      // Including null terminator
#define FLASH_PROFILE_MAGIC      0x50524F46  // "PROF"

// Initialize flash profile subsystem — scans for valid profiles
void FlashProfiles_Init(void);

// Get number of valid profiles currently stored
int FlashProfiles_GetCount(void);

// Get maximum capacity (always FLASH_PROFILE_MAX_SLOTS if sector 10 is free)
int FlashProfiles_GetCapacity(void);

// Read profile from flash slot (0-based). Returns 0 on success, -1 if empty/invalid.
int FlashProfiles_Read(int slot, uint16_t temps[48], char name[FLASH_PROFILE_NAME_LEN]);

// Write profile to flash slot. Erases sector and rewrites all profiles.
// Returns 0 on success, -1 on error.
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
