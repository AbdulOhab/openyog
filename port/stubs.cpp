/* PORT stubs: Linux stand-in implementations for a handful of core functions
 * whose Windows versions live in GUI-coupled translation units we cannot
 * compile yet. Each stub is tracked for replacement in later phases.
 *
 * AllocateBuff / EncodeBase64 / DecodeBase64 / DecodeQuantum / table64 are
 * copied verbatim from src/CommonHelper.cpp (upstream 13.3.1 GA) so behavior
 * matches the app exactly; the copies die when CommonHelper itself becomes
 * buildable.
 */

#include "CommonHelper.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>

/* Windows CRT functions the Linux path of Datatype.h (lines 104-106) declares
 * as extern — upstream expects the builder to supply them, same as their SJA
 * Linux build did. ASCII-only semantics to match Windows stricmp behavior. */
int stricmp(const char *a, const char *b)
{
    while(*a && *b) {
        int ca = std::tolower((unsigned char)*a), cb = std::tolower((unsigned char)*b);
        if(ca != cb)
            return ca - cb;
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strnicmp(const char *a, const char *b, int count)
{
    while(count-- > 0) {
        int ca = std::tolower((unsigned char)*a), cb = std::tolower((unsigned char)*b);
        if(ca != cb)
            return ca - cb;
        if(!*a)
            return 0;
        ++a; ++b;
    }
    return 0;
}

my_ulonglong _atoi64(const char *nptr)
{
    return (my_ulonglong)std::strtoll(nptr, nullptr, 10);
}

/* Real one writes into the app log directory (CommonHelper.cpp). */
wyBool WriteToLogFile(wyChar *message)
{
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "[corelog %s] %s\n", stamp, message ? message : "(null)");
    return wyTrue;
}

/* Verbatim from src/CommonHelper.cpp:1402 */
wyChar*
AllocateBuff(wyInt32 size)
{
    return((wyChar*)calloc(sizeof(wyChar), size));
}

/* ---- Base64 (verbatim from src/CommonHelper.cpp:1571-1699, incl. table64) --- */

static wyChar table64[]=
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

wyInt32
EncodeBase64( const wyChar *inp, size_t insize, wyChar ** outptr)
{
	wyUChar  ibuf[3];
	wyUChar  obuf[4];
	wyUInt32 len = 0;
	wyInt32  i;
	wyInt32  inputparts;
	wyChar   *output;
	wyChar   *base64data;
	wyChar *indata = (wyChar *)inp;

	*outptr = NULL; /* set to NULL in case of failure before we reach the end */

	if(0 == insize)
		insize = strlen(indata);

	base64data = output =(wyChar*)malloc(insize*4/3+4);
	if(NULL == output)
		return 0;

	while(insize > 0){
		for(i = inputparts = 0; i < 3; i++){
			if(insize > 0){
				inputparts++;
				ibuf[i] = *indata;
				indata++;
				insize--;
			}
			else
				ibuf[i] = 0;
		}

		obuf [0] =(ibuf [0] & 0xFC)>> 2;
		obuf [1] =((ibuf [0] & 0x03)<< 4)|((ibuf [1] & 0xF0)>> 4);
		obuf [2] =((ibuf [1] & 0x0F)<< 2)|((ibuf [2] & 0xC0)>> 6);
		obuf [3] = ibuf [2] & 0x3F;

		switch(inputparts){

	case 1: // only one byte read
		len += sprintf(output, "%c%c==",
			table64[obuf[0]],
			table64[obuf[1]]);
		break;

	case 2: // two bytes read
		len += sprintf(output, "%c%c%c=",
			table64[obuf[0]],
			table64[obuf[1]],
			table64[obuf[2]]);
		break;

	default:
		len += sprintf(output, "%c%c%c%c",
			table64[obuf[0]],
			table64[obuf[1]],
			table64[obuf[2]],
			table64[obuf[3]]);
		break;
		}

		output += 4;
	}

	*output=0;
	*outptr = base64data; // make it return the actual data memory

	return len; // return true
}

static void
DecodeQuantum(wyUChar *dest, const wyChar *src)
{
	wyUInt32    x = 0;
	wyInt32     i;
	for(i = 0; i < 4; i++) {
		if(src[i] >= 'A' && src[i] <= 'Z')
			x = (x << 6) + (wyUInt32)(src[i] - 'A' + 0);
		else if(src[i] >= 'a' && src[i] <= 'z')
			x = (x << 6) + (wyUInt32)(src[i] - 'a' + 26);
		else if(src[i] >= '0' && src[i] <= '9')
			x = (x << 6) + (wyUInt32)(src[i] - '0' + 52);
		else if(src[i] == '+')
			x = (x << 6) + 62;
		else if(src[i] == '/')
			x = (x << 6) + 63;
		else if(src[i] == '=')
			x = (x << 6);
	}

	dest[2] = (wyUChar)(x & 255);
	x >>= 8;
	dest[1] = (wyUChar)(x & 255);
	x >>= 8;
	dest[0] = (wyUChar)(x & 255);
}

size_t
DecodeBase64 (const wyChar *src, wyChar *dest)
{
	wyInt32 length = 0;
	wyInt32 equalsTerm = 0;
	wyInt32 i;
	wyInt32 numQuantums;
	wyUChar lastQuantum[3];
	size_t rawlen=0;

	while((src[length] != '=') && src[length])
		length++;
	while(src[length+equalsTerm] == '=')
		equalsTerm++;

	numQuantums = (length + equalsTerm) / 4;

	rawlen = (numQuantums * 3) - equalsTerm;

	for(i = 0; i < numQuantums - 1; i++) {
		DecodeQuantum((wyUChar *)dest, src);
		dest += 3; src += 4;
	}

	DecodeQuantum(lastQuantum, src);
	for(i = 0; i < 3 - equalsTerm; i++)
		dest[i] = lastQuantum[i];

	return rawlen;
}
