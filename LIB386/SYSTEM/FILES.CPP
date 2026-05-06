#include <SYSTEM/FILES.H>

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#ifdef LBA2_TARGET_DREAMCAST
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dc/vmu_pkg.h>
#endif

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// --- Private definitions -----------------------------------------------------
#ifndef _WIN32 // Portability - O_BINARY exists only on Windows platform
#define O_BINARY 0
#endif // _WIN32

#define MODE_READ (O_RDONLY | O_BINARY)
#define MODE_WRITE (O_WRONLY | O_BINARY | O_CREAT | O_TRUNC)
#define MODE_RW (O_RDWR | O_BINARY | O_CREAT)

// --- Private functions -------------------------------------------------------
S32 OpenMode(const char *filename, int mode) {
    assert(filename != NULL);
    S32 handle = open(filename, mode, (S_IRUSR | S_IWUSR));
    return (handle == -1) ? 0 : handle;
}

// --- Public functions --------------------------------------------------------
S32 OpenRead(const char *filename) { return OpenMode(filename, MODE_READ); }
S32 OpenWrite(const char *filename) { return OpenMode(filename, MODE_WRITE); }
S32 OpenReadWrite(const char *filename) { return OpenMode(filename, MODE_RW); }

S32 Close(S32 handle) {
    assert(handle > 0);
    return close(handle);
}

S32 Read(S32 handle, void *buffer, U32 maxReadBytes) {
    assert(handle > 0);
    assert(buffer != NULL);
    return read(handle, buffer, maxReadBytes);
}

S32 Write(S32 handle, const void *buffer, U32 maxWriteBytes) {
    assert(handle > 0);
    assert(buffer != NULL);
    return write(handle, buffer, maxWriteBytes);
}

S32 Seek(S32 handle, S32 offset, ESeekOrigin origin) {
    assert(handle > 0);

    int stdOrigin;
    switch (origin) {
    case SEEK_FROM_START:
        stdOrigin = SEEK_SET;
        break;
    case SEEK_FROM_CURRENT:
        stdOrigin = SEEK_CUR;
        break;
    case SEEK_FROM_END:
        stdOrigin = SEEK_END;
        break;
    default:
        assert(false);
        break;
    }

    return lseek(handle, offset, stdOrigin);
}

U32 Delete(const char *filename) {
    assert(filename != NULL);
    S32 result = unlink(filename);
    return (result != -1);
}

S32 Touch(const char *filename) {
    assert(filename != NULL);
    return utime(filename, NULL);
}

// V2.9 (DC NVRAM) — internal helpers shared with LOADSAVE.CPP. Defined here
// statically because we don't want a separate header just for two functions.
#ifdef LBA2_TARGET_DREAMCAST
#define VMU_PORT_PREFIX_FS "/vmu/a1/"

static int FilesIsSaveFilename(const char *name) {
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".lba") == 0);
}

static const char *FilesBasename(const char *path) {
    const char *fwd = strrchr(path, '/');
    const char *bwd = strrchr(path, '\\');
    const char *sep = (fwd && (!bwd || fwd > bwd)) ? fwd : bwd;
    return sep ? sep + 1 : path;
}

static void FilesMangleVmu(const char *lba_path, char *out, size_t out_size) {
    const char *base = FilesBasename(lba_path);
    if (strcasecmp(base, "autosave.lba") == 0) {
        snprintf(out, out_size, "%sLBA2_AUTO", VMU_PORT_PREFIX_FS);
        return;
    }
    if (strcasecmp(base, "current.lba") == 0) {
        snprintf(out, out_size, "%sLBA2_CUR", VMU_PORT_PREFIX_FS);
        return;
    }
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
    snprintf(out, out_size, "%sLBA2_%s", VMU_PORT_PREFIX_FS, prefix);
}
#endif

U32 FileSize(const char *filename) {
    assert(filename != NULL);

#ifdef LBA2_TARGET_DREAMCAST
    // V2.9.1: VMU-backed save files are wrapped in a vmu_pkg (KOS format).
    // FileSize must return the actual save data length (`pkg.data_len`), not
    // the on-disk size which includes the package header + 512-block padding.
    // The engine's LZSS decompressor uses this size to know where compressed
    // data ends.
    if (FilesIsSaveFilename(filename)) {
        char vmu[64];
        FilesMangleVmu(filename, vmu, sizeof(vmu));
        FILE *fp = fopen(vmu, "rb");
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
        vmu_pkg_t pkg;
        int parsed = vmu_pkg_parse(raw, (size_t)fs, &pkg);
        U32 result = (parsed == 0) ? (U32)pkg.data_len : 0;
        free(raw);
        return result;
    }
#endif

    S32 handle = OpenRead(filename);
    if (!handle) {
        return 0;
    }

    S32 fsize = lseek(handle, 0, SEEK_END);
    Close(handle);

    return (U32)((fsize == -1) ? 0 : fsize);
}

bool ExistsFileOrDir(const char *path) {
    assert(path != NULL);

#ifdef LBA2_TARGET_DREAMCAST
    // V2.9: route save-file existence checks to the VMU mangled path.
    if (FilesIsSaveFilename(path)) {
        char vmu[64];
        FilesMangleVmu(path, vmu, sizeof(vmu));
        FILE *fp = fopen(vmu, "rb");
        if (fp) { fclose(fp); return true; }
        return false;
    }
    // KOS' stat() is unreliable on ISO9660 (especially for paths with a
    // trailing '/'). Use opendir / fopen instead — same trick the simulant DC
    // port uses in kfs.cpp::exists().
    size_t len = strlen(path);
    bool trailingSep = (len > 0 && (path[len-1] == '/' || path[len-1] == '\\'));
    if (trailingSep) {
        DIR *d = opendir(path);
        if (d) { closedir(d); return true; }
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    DIR *d = opendir(path);
    if (d) { closedir(d); return true; }
    return false;
#else
    struct stat status = {};
    return (stat(path, &status) == 0);
#endif
}

bool IsDirectory(const char *path) {
    assert(path != NULL);

#ifdef LBA2_TARGET_DREAMCAST
    DIR *d = opendir(path);
    if (d) { closedir(d); return true; }
    return false;
#else
    struct stat status = {};
    return ((stat(path, &status) == 0) && ((status.st_mode & S_IFMT) == S_IFDIR));
#endif
}

// =============================================================================
#ifdef __cplusplus
}
#endif
