//--------------------------------------------------------------------------//
#ifndef LIB_SYSTEM_LOGPRINT
#define LIB_SYSTEM_LOGPRINT

//----------------------------------------------------------------------------
#include <SYSTEM/ADELINE_TYPES.H>

#include <stdarg.h>
#include <stdio.h>

//--------------------------------------------------------------------------//
#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------
extern U32 Log2File;
extern U32 QuietLog; ///< If TRUE then will not write to console, only to file

//----------------------------------------------------------------------------
extern void CreateLog(const char *filePath);

//----------------------------------------------------------------------------
extern void LogPrintf(const char *format, ...);

//----------------------------------------------------------------------------
extern void LogPuts(const char *string);

//--------------------------------------------------------------------------//
// Dreamcast boot/perf tracing. Compile-time off unless LBA2_DEBUG_PERF is
// defined; intended to be enabled for UART-side diagnostics.
#if defined(LBA2_TARGET_DREAMCAST) && defined(LBA2_DEBUG_PERF)
#  define LBA2_DC_TRACE(...)    LogPrintf(__VA_ARGS__)
#  define LBA2_DC_TRACE_PUTS(s) LogPuts(s)
#else
#  define LBA2_DC_TRACE(...)    ((void)0)
#  define LBA2_DC_TRACE_PUTS(s) ((void)0)
#endif

//--------------------------------------------------------------------------//
#ifdef __cplusplus
}
#endif

//--------------------------------------------------------------------------//
#endif //LIB_SYSTEM_LOGPRINT

//----------------------------------------------------------------------------
