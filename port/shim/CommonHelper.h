/* PORT shim: minimal CommonHelper.h stand-in for compiling leaf core files
 * (wyString.cpp) on Linux without the full GUI header chain.
 * The real include/CommonHelper.h needs CommonJobStructures.h, which is
 * missing from the upstream repo (see xnote/2026-09-08-codebase-metrics.md).
 * This shim shadows include/CommonHelper.h via -I port/shim coming first.
 * It must grow or die when Phase 2 starts compiling real UI-adjacent code.
 */
#ifndef PORT_SHIM_COMMONHELPER_H
#define PORT_SHIM_COMMONHELPER_H

#include "Datatype.h"
#include "wyString.h"

/* Localization: real app resolves via L10n (CommonHelper.h defines the same
 * identity fallback when L10n is off). Core/port code runs untranslated. */
#define _(STRING)   (STRING)

/* Real definition: include/Verify.h:28/30 (debug assert / release no-op). */
#include <cassert>
#define VERIFY(f)   assert(f)

/* Real signatures: include/CommonHelper.h:482-489, 609, 618.
 * Implementations live in port/stubs.cpp (AllocateBuff verbatim from
 * CommonHelper.cpp; base64 verbatim from CommonHelper.cpp). */
wyWChar *AllocateBuffWChar(wyInt32 size);
wyChar  *AllocateBuff(wyInt32 size);
size_t   DecodeBase64(const wyChar *src, wyChar *dest);
wyInt32  EncodeBase64(const wyChar *inp, size_t insize, wyChar **outptr);

/* Real signature: include/CommonHelper.h:845. Implementation lives in
 * port/stubs.cpp (stderr writer) until the real logger is ported. */
wyBool WriteToLogFile(wyChar *message);

#endif /* PORT_SHIM_COMMONHELPER_H */
