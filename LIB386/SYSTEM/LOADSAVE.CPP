//──────────────────────────────────────────────────────────────────────────
#include <SYSTEM/LOADSAVE.H>

#include <SYSTEM/ADELINE.H>
#include <SYSTEM/FILES.H>
#include <SYSTEM/LOGPRINT.H>

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

#ifdef LBA2_TARGET_DREAMCAST
#include <stdio.h>
#include <dc/vmu_pkg.h>
#endif

//──────────────────────────────────────────────────────────────────────────
// V2.9 (DC NVRAM): Dreamcast saves to VMU. The KOS /vmu/<port>/<file>
// filesystem rounds file size up to the nearest 512-byte block boundary —
// reading FileSize() back gives us the padded size, not the actual save data
// length. The engine's LZSS decompress in LoadGameNumCube() / LoadGameScreen()
// uses FileSize() as the input length, so unpadded raw writes would feed
// trailing zero bytes into the decompressor and produce garbage output.
//
// Solution: prepend a single 4-byte little-endian length header to every save
// file written to VMU. Save() writes [hdr=size][data]. Load() reads [hdr], then
// reads exactly hdr bytes. FileSize() reads only the [hdr] and returns it.
// All VMU block padding is invisible to the engine.
//
// LBA2 save filenames don't fit DC's 12-character VMU file-name limit; we
// mangle:
//   "autosave.lba" → "/vmu/a1/LBA2_AUTO"
//   "current.lba"  → "/vmu/a1/LBA2_CUR"
//   "<player>.lba" → "/vmu/a1/LBA2_<first 6 alphanumeric upper>"
// The displayed name in the save-list menu comes from the player name stored
// in the file content, not the VMU filename — collisions on the 6-char prefix
// just overwrite, which mirrors original LBA2 behaviour for same-name saves.
#ifdef LBA2_TARGET_DREAMCAST

#define VMU_PORT_PREFIX "/vmu/a1/"

static int IsSaveFilename(const char *name) {
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".lba") == 0);
}

// Strip directory prefix (handles both '/' and '\' separators).
static const char *Basename(const char *path) {
    const char *fwd = strrchr(path, '/');
    const char *bwd = strrchr(path, '\\');
    const char *sep = (fwd && (!bwd || fwd > bwd)) ? fwd : bwd;
    return sep ? sep + 1 : path;
}

// Mangle an LBA save path to a VMU path. Only call when IsSaveFilename(name).
static void MangleSavePathToVmu(const char *lba_path, char *out, size_t out_size) {
    const char *base = Basename(lba_path);

    if (strcasecmp(base, "autosave.lba") == 0) {
        snprintf(out, out_size, "%sLBA2_AUTO", VMU_PORT_PREFIX);
        return;
    }
    if (strcasecmp(base, "current.lba") == 0) {
        snprintf(out, out_size, "%sLBA2_CUR", VMU_PORT_PREFIX);
        return;
    }

    // Player-named save: LBA2_<6 alpha-num upper> from stem (before ".lba")
    char prefix[7];
    int pi = 0;
    for (int i = 0; base[i] && base[i] != '.' && pi < 6; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        prefix[pi++] = c;
    }
    prefix[pi] = 0;
    if (pi == 0) { prefix[0] = 'X'; prefix[1] = 0; }
    snprintf(out, out_size, "%sLBA2_%s", VMU_PORT_PREFIX, prefix);
}

// V2.9.1: load full VMU file into a malloc'd buffer and parse via vmu_pkg_parse.
// Caller owns *outRaw and must free() after extracting pkg.data. Returns 1 on
// success, 0 on missing file / parse error / OOM.
//
// Reason for proper packaging (changed from V2.9 raw 4-byte prefix): KOS' fs_vmu
// emits a `vmu_pkg_parse: expected CRC ... got ...` warning every time it reads
// a non-pkg file, and the printf to dbgio is itself slow enough to be noticed
// per-frame when the menu state check IsExistSavedGame() runs. Switching to the
// real format makes reads silent AND opens the door to BIOS visibility (the
// Dreamcast save manager will recognize our files as "LBA2 Save").
static int LoadVmuPkg(const char *vmu_path, vmu_pkg_t *outPkg, uint8_t **outRaw, size_t *outRawSize) {
    FILE *fp = fopen(vmu_path, "rb");
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long fs = ftell(fp);
    if (fs <= 0) { fclose(fp); return 0; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return 0; }

    uint8_t *raw = (uint8_t *)malloc((size_t)fs);
    if (!raw) { fclose(fp); return 0; }
    size_t r = fread(raw, 1, (size_t)fs, fp);
    fclose(fp);
    if ((long)r != fs) { free(raw); return 0; }

    if (vmu_pkg_parse(raw, (size_t)fs, outPkg) < 0) {
        free(raw);
        return 0;
    }
    *outRaw = raw;
    if (outRawSize) *outRawSize = (size_t)fs;
    return 1;
}

#endif // LBA2_TARGET_DREAMCAST

//──────────────────────────────────────────────────────────────────────────
U32 Load(char *name, void *buffer) {
    S32 handle;
    U32 size;

#ifdef LBA2_TARGET_DREAMCAST
    if (IsSaveFilename(name)) {
        char vmu[64];
        MangleSavePathToVmu(name, vmu, sizeof(vmu));
        vmu_pkg_t pkg;
        uint8_t *raw = NULL;
        if (!LoadVmuPkg(vmu, &pkg, &raw, NULL)) {
            return 0;
        }
        memcpy(buffer, pkg.data, (size_t)pkg.data_len);
        U32 result = (U32)pkg.data_len;
        free(raw);
        return result;
    }
#endif

    size = FileSize(name);
    if (!size)
        return 0;

    handle = OpenRead(name);
    if (handle) {
        size = Read(handle, buffer, size);
        Close(handle);
        return size;
    }
    return 0;
}
//──────────────────────────────────────────────────────────────────────────
S32 LoadSize(char *name, void *buffer, S32 size) {
    S32 handle;
    S32 maxsize, sizeread, sizetoread;

#ifdef LBA2_TARGET_DREAMCAST
    if (IsSaveFilename(name)) {
        // For VMU-backed saves, just defer to Load() (which already caps at the
        // size-header value). LoadSize/Load semantics are equivalent on DC
        // because the save data is always written whole.
        U32 r = Load(name, buffer);
        return (S32)r;
    }
#endif

    maxsize = FileSize(name);
    if (!maxsize)
        return (0);

    handle = OpenRead(name);
    if (handle) {
        sizetoread = ade_min(maxsize, size);
        sizeread = Read(handle, buffer, sizetoread);
        Close(handle);
        if (sizeread < sizetoread) {
            if (sizeread < maxsize) {
                // error reading
                sizeread = (S32)INT_MIN; /* min 32-bit int; avoid LONG_MIN on LP64 */
            }
        } else {
            if (maxsize > size) {
                // file too large by...
                sizeread -= maxsize;
            }
        }
        return (sizeread);
    }
    return (0);
}
//──────────────────────────────────────────────────────────────────────────
#ifdef LBA2_TARGET_DREAMCAST
// Forward decl — defined in GAMEMENU.CPP. Called after every successful save
// so the next IsExistSavedGame() picks up the new file.
extern "C" void InvalidateSavedGameCache(void);
#endif

U32 Save(char *name, void *buffer, U32 size) {
    S32 handle;
    U32 written = 0;

#ifdef LBA2_TARGET_DREAMCAST
    if (IsSaveFilename(name)) {
        char vmu[64];
        MangleSavePathToVmu(name, vmu, sizeof(vmu));

        vmu_pkg_t pkg;
        memset(&pkg, 0, sizeof(pkg));
        strcpy(pkg.desc_short, "LBA2 Save");
        strcpy(pkg.desc_long, "Twinsen's Adventure");
        strcpy(pkg.app_id, "LBA2");
        pkg.icon_cnt = 0;
        pkg.icon_anim_speed = 0;
        pkg.eyecatch_type = VMUPKG_EC_NONE;
        pkg.data_len = (int)size;
        pkg.data = (const uint8_t *)buffer;

        uint8_t *out = NULL;
        int out_size = 0;
        if (vmu_pkg_build(&pkg, &out, &out_size) < 0) {
            LogPrintf("DC-NVRAM: vmu_pkg_build failed for %s\n", vmu);
            return 0;
        }

        FILE *fp = fopen(vmu, "wb");
        if (!fp) {
            free(out);
            LogPrintf("DC-NVRAM: Save fopen(%s) failed (no VMU?)\n", vmu);
            return 0;
        }
        size_t w = fwrite(out, 1, (size_t)out_size, fp);
        fclose(fp);
        free(out);
        if ((int)w != out_size) {
            LogPrintf("DC-NVRAM: Save short write to %s: %u of %d\n",
                      vmu, (unsigned)w, out_size);
            return 0;
        }
        LogPrintf("DC-NVRAM: Saved %u bytes to %s (pkg %d bytes)\n",
                  (unsigned)size, vmu, out_size);
        InvalidateSavedGameCache();
        return 1;
    }
#endif

    handle = OpenWrite(name);
    if (handle) {
        written = Write(handle, buffer, size);
        Close(handle);
    } else {
        LogPrintf("Warning: Unable to save game to file '%s'\n", name);
    }

    return (written == size);
}

//──────────────────────────────────────────────────────────────────────────
S32 ReadSaveHeader(const char *fullPath, U8 *outNumVersion, S32 *outNumCube,
                   char *playerName, U32 playerNameMax) {
    if (!fullPath || !outNumVersion || !outNumCube || !playerName || playerNameMax == 0)
        return 0;

#ifdef LBA2_TARGET_DREAMCAST
    if (IsSaveFilename(fullPath)) {
        char vmu[64];
        MangleSavePathToVmu(fullPath, vmu, sizeof(vmu));
        vmu_pkg_t pkg;
        uint8_t *raw = NULL;
        if (!LoadVmuPkg(vmu, &pkg, &raw, NULL)) return 0;
        if (pkg.data_len < 6) { free(raw); return 0; }
        const uint8_t *p = pkg.data;
        *outNumVersion = p[0];
        memcpy(outNumCube, p + 1, 4);
        U32 i = 0;
        U32 j = 5;
        while (j < (U32)pkg.data_len) {
            unsigned char c = p[j++];
            if (i + 1 < playerNameMax) playerName[i++] = (char)c;
            if (c == 0) break;
        }
        playerName[i < playerNameMax ? i : playerNameMax - 1] = 0;
        free(raw);
        return 1;
    }
#endif

    // Generic path: open the raw file and read header bytes verbatim.
    S32 handle = OpenRead(fullPath);
    if (!handle) return 0;
    S32 r = Read(handle, outNumVersion, 1);
    if (r != 1) { Close(handle); return 0; }
    r = Read(handle, outNumCube, 4);
    if (r != 4) { Close(handle); return 0; }
    U32 i = 0;
    for (;;) {
        unsigned char c = 0;
        if (Read(handle, &c, 1) != 1) { Close(handle); return 0; }
        if (i + 1 < playerNameMax) playerName[i++] = (char)c;
        if (c == 0) break;
    }
    playerName[i < playerNameMax ? i : playerNameMax - 1] = 0;
    Close(handle);
    return 1;
}

//──────────────────────────────────────────────────────────────────────────
