//──────────────────────────────────────────────────────────────────────────
#ifndef LIB_SYSTEM_LOADSAVE
#define LIB_SYSTEM_LOADSAVE

#include <SYSTEM/ADELINE_TYPES.H>

//──────────────────────────────────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif

//──────────────────────────────────────────────────────────────────────────
extern U32 Load(char *name, void *buffer);

//──────────────────────────────────────────────────────────────────────────
extern S32 LoadSize(char *name, void *buffer, S32 size);

//──────────────────────────────────────────────────────────────────────────
extern U32 Save(char *name, void *buffer, U32 size);

//──────────────────────────────────────────────────────────────────────────
// V2.9 (DC NVRAM): peek the save header (numVersion + cubeNum + zero-terminated
// playerName) without decompressing the rest of the file. Used by the save-list
// menu to display each save's player name and cube context.
//
// Returns 1 on success, 0 on failure (file missing, malformed, or
// playerName buffer too small).
//
// On non-DC builds this just opens the file via OpenRead+Read directly. On DC
// the file is wrapped in a 4-byte size prefix (to deal with VMU 512-byte block
// padding), and ReadSaveHeader() transparently skips the prefix.
//
// Keeps `playerName` C-string semantics: writes up to playerNameMax-1 bytes
// then NUL.
extern S32 ReadSaveHeader(const char *fullPath, U8 *outNumVersion, S32 *outNumCube,
                          char *playerName, U32 playerNameMax);

//──────────────────────────────────────────────────────────────────────────
#ifdef __cplusplus
}
#endif

//──────────────────────────────────────────────────────────────────────────
#endif //LIB_SYSTEM_LOADSAVE

//──────────────────────────────────────────────────────────────────────────
