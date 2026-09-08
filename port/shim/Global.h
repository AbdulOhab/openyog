/* PORT shim: minimal Global.h stand-in.
 * The real include/Global.h includes <windows.h>/<commctrl.h> unconditionally
 * and drags in the whole GUI world; core files (wyIni.cpp) only need the
 * basic types. Shadowed via -I port/shim coming before -I include.
 * Grows only when a compiling file proves it needs more.
 */
#ifndef PORT_SHIM_GLOBAL_H
#define PORT_SHIM_GLOBAL_H

#include "Datatype.h"
#include "wyString.h"

#ifndef _WIN32
/* PORT: wyIni.cpp guards INI access with the app-wide critical section
 * (pGlobals->m_csiniglobal). The real GLOBALS lives in the Windows GUI app
 * (Global.cpp — not portable); core builds get a pthread-backed stand-in
 * defined in port/stubs.cpp. */
#include <pthread.h>

typedef pthread_mutex_t CRITICAL_SECTION;
inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }

struct GLOBALS
{
    CRITICAL_SECTION m_csiniglobal;
};

typedef GLOBALS *PGLOBALS;

/* PORT: Win32 last-error + Sleep() used by wyIni.cpp retry loops. */
#include <cerrno>
#include <unistd.h>
#define GetLastError()          errno
#define ERROR_FILE_NOT_FOUND    ENOENT
#define Sleep(ms)               usleep((useconds_t)(ms) * 1000)

/* PORT: declared by upstream expectations (Global.h chain), supplied by
 * port/stubs.cpp. */
wyChar  *itoa(int value, wyChar *str, int radix);
const wyChar *wyWideToUtf8(const wyWChar *wide);   /* static buffer; cs-guarded */

/* PORT: wyIni.cpp SaveFile() writes INI content through the Win32 WriteFile()
 * on wyFile's handle; on Linux that handle is an fd. INI lines are tiny, so
 * the partial-write caveat of write(2) is ignored (same as upstream, which
 * also never checks the result). */
#define WriteFile(fd, buf, len, written, overlapped)                \
    ((*(written) = (write((fd), (buf), (len)) >= 0 ? (len) : 0)), 1)

#endif /* !_WIN32 */

#endif /* PORT_SHIM_GLOBAL_H */
