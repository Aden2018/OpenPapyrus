// SZLIB.C
// Copyright (C) 2004-2017 Mark Adler
//
// Note: This is the merging of several small module of original zlib for compacting.
//
#define ZLIB_INTERNAL
#include "zlib.h"
#pragma hdrstop

#ifdef _LARGEFILE64_SOURCE
	#ifndef _LARGEFILE_SOURCE
		#define _LARGEFILE_SOURCE 1
	#endif
	#ifdef _FILE_OFFSET_BITS
		#undef _FILE_OFFSET_BITS
	#endif
#endif
#ifdef HAVE_HIDDEN
	#define ZLIB_INTERNAL __attribute__((visibility("hidden")))
#else
	#define ZLIB_INTERNAL
#endif
#ifndef _POSIX_SOURCE
	#define _POSIX_SOURCE
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
	#if defined(UNICODE) || defined(_UNICODE)
		#define WIDECHAR
	#endif
#endif
#ifdef WINAPI_FAMILY
	#define open _open
	#define read _read
	#define write _write
	#define close _close
#endif
#ifdef NO_DEFLATE       /* for compatibility with old definition */
	#define NO_GZCOMPRESS
#endif
#if defined(STDC99) || (defined(__TURBOC__) && __TURBOC__ >= 0x550)
	#ifndef HAVE_VSNPRINTF
		#define HAVE_VSNPRINTF
	#endif
#endif
#if defined(__CYGWIN__)
	#ifndef HAVE_VSNPRINTF
		#define HAVE_VSNPRINTF
	#endif
#endif
#if defined(MSDOS) && defined(__BORLANDC__) && (BORLANDC > 0x410)
	#ifndef HAVE_VSNPRINTF
		#define HAVE_VSNPRINTF
	#endif
#endif
#ifndef HAVE_VSNPRINTF
	#ifdef MSDOS
		// vsnprintf may exist on some MS-DOS compilers (DJGPP?), but for now we just assume it doesn't. 
		#define NO_vsnprintf
	#endif
	#ifdef __TURBOC__
		#define NO_vsnprintf
	#endif
	#ifdef WIN32
		// In Win32, vsnprintf is available as the "non-ANSI" _vsnprintf. 
		#if !defined(vsnprintf) && !defined(NO_vsnprintf)
			#if !defined(_MSC_VER) || ( defined(_MSC_VER) && _MSC_VER < 1500 )
				#define vsnprintf _vsnprintf
			#endif
		#endif
	#endif
	#ifdef __SASC
		#define NO_vsnprintf
	#endif
	#ifdef VMS
		#define NO_vsnprintf
	#endif
	#ifdef __OS400__
		#define NO_vsnprintf
	#endif
	#ifdef __MVS__
		#define NO_vsnprintf
	#endif
#endif
// unlike snprintf (which is required in C99), _snprintf does not guarantee
// null termination of the result -- however this is only used in gzlib.c where
// the result is assured to fit in the space provided 
#if defined(_MSC_VER) && _MSC_VER < 1900
	#define snprintf _snprintf
#endif
/* @sobolev #ifndef local
	#define local static
#endif
/* since "static" is used to mean two completely different things in C, we
   define "local" for the non-static meaning of "static", for readability
   (compile with -Dlocal if your debugger can't find static symbols) */
//
// get errno and strerror definition 
//
#if defined UNDER_CE
	#include <windows.h>
	#define zstrerror() gz_strwinerror((DWORD)GetLastError())
#else
	#ifndef NO_STRERROR
		#define zstrerror() strerror(errno)
	#else
		#define zstrerror() "stdio error (consult errno)"
	#endif
#endif
//
// provide prototypes for these when building zlib without LFS 
//
#if !defined(_LARGEFILE64_SOURCE) || _LFS64_LARGEFILE-0 == 0
	ZEXTERN gzFile ZEXPORT gzopen64(const char *, const char *);
	ZEXTERN z_off64_t ZEXPORT gzseek64(gzFile, z_off64_t, int);
	ZEXTERN z_off64_t ZEXPORT gztell64(gzFile);
	ZEXTERN z_off64_t ZEXPORT gzoffset64(gzFile);
#endif
//
// default memLevel 
//
#if MAX_MEM_LEVEL >= 8
	#define DEF_MEM_LEVEL 8
#else
	#define DEF_MEM_LEVEL  MAX_MEM_LEVEL
#endif
//
// default i/o buffer size -- double this for output when reading (this and
// twice this must be able to fit in an unsigned type) 
//
#define GZBUFSIZE 8192
//
// gzip modes, also provide a little integrity check on the passed structure 
//
#define GZ_NONE 0
#define GZ_READ 7247
#define GZ_WRITE 31153
#define GZ_APPEND 1     /* mode set to GZ_WRITE after the file is opened */
//
// values for gz_state how 
//
#define GZSTATE_LOOK 0 // look for a gzip header 
#define GZSTATE_COPY 1 // copy input directly 
#define GZSTATE_GZIP 2 // decompress a gzip stream 
//
// internal gzip file state data structure 
//
struct gz_state {
	// exposed contents for gzgetc() macro 
	struct gzFile_s x; // "x" for exposed 
	// x.have: number of bytes available at x.next 
	// x.next: next output data to deliver or write 
	// x.pos: current position in uncompressed data 
	// used for both reading and writing 
	int    mode; // see gzip modes above 
	int    fd;   // file descriptor 
	char * path; // path or fd for error messages 
	uint   size; // buffer size, zero if not allocated yet 
	uint   want; // requested buffer size, default is GZBUFSIZE 
	uchar * in;  // input buffer (double-sized when writing) 
	uchar * out; // output buffer (double-sized when reading) 
	int    direct; // 0 if processing gzip, 1 if transparent 
	// just for reading 
	int    how;      // 0: get header, 1: copy, 2: decompress 
	z_off64_t start; // where the gzip data started, for rewinding 
	int    eof;      // true if end of input file reached 
	int    past;     // true if read requested past end 
	// just for writing 
	int    level;    // compression level 
	int    strategy; // compression strategy 
	// seek request 
	z_off64_t skip;  // amount to skip (already rewound if backwards) 
	int    seek;     // true if seek request pending 
	// error information 
	int    err;      // error code 
	char * msg;      // error message 
	// zlib inflate or deflate stream 
	z_stream strm;   // stream structure in-place (not a pointer) */
};
//
// shared functions 
//
void ZLIB_INTERNAL FASTCALL gz_error(gz_state *, int, const char *);
#if defined UNDER_CE
	char ZLIB_INTERNAL * gz_strwinerror(DWORD error);
#endif
// 
// GT_OFF(x), where x is an unsigned value, is true if x > maximum z_off64_t
// value -- needed when comparing unsigned to z_off64_t, which is signed
// (possible z_off64_t types off_t, off64_t, and long are all signed) 
#ifdef INT_MAX
	#define GT_OFF(x) (sizeof(int) == sizeof(z_off64_t) && (x) > INT_MAX)
#else
	unsigned ZLIB_INTERNAL gz_intmax(void);
	#define GT_OFF(x) (sizeof(int) == sizeof(z_off64_t) && (x) > gz_intmax())
#endif
// 
// Function prototypes
// 
typedef enum {
	need_more,  /* block not completed, need more input or more output */
	block_done, /* block flush performed */
	finish_started, /* finish started, need only more output at next deflate */
	finish_done /* finish done, accept no more input or output */
} block_state;
//
// Compression function. Returns the block state after the call. 
//
typedef block_state (*compress_func)(deflate_state *s, int flush);

static void FASTCALL fill_window(deflate_state *s);
static block_state deflate_stored(deflate_state *s, int flush);
static block_state deflate_fast(deflate_state *s, int flush);
#ifndef FASTEST
	static block_state deflate_slow(deflate_state *s, int flush);
#endif
static block_state FASTCALL deflate_rle(deflate_state *s, int flush);
static block_state FASTCALL deflate_huff(deflate_state *s, int flush);
#ifdef ASMV
	#pragma message("Assembler code may have bugs -- use at your own risk")
	void match_init(void);       /* asm code initialization */
	uInt FASTCALL longest_match(deflate_state *s, IPos cur_match);
#else
	static uInt FASTCALL longest_match(deflate_state *s, IPos cur_match);
#endif
#ifdef ZLIB_DEBUG
	static void check_match(deflate_state *s, IPos start, IPos match, int length);
#endif
// 
// Local (static) routines in this file.
// 
static void tr_static_init();
static void init_block(deflate_state *s);
static void pqdownheap(deflate_state *s, ct_data *tree, int k);
static void gen_codes(ct_data *tree, int max_code, ushort *bl_count);
static void compress_block(deflate_state *s, const ct_data *ltree, const ct_data *dtree);
static int  detect_data_type(deflate_state *s);
static uint FASTCALL bi_reverse(uint value, int length);
static void FASTCALL bi_windup(deflate_state *s);
static void FASTCALL bi_flush(deflate_state *s);
#ifdef MAKEFIXED
	#ifndef BUILDFIXED
		#define BUILDFIXED
	#endif
#endif
static int updatewindow(z_streamp strm, const uchar * end, uint copy);
#ifdef BUILDFIXED
	void makefixed(void);
#endif
static uint syncsearch(uint * have, const uchar * buf, uint len);
//
// ADLER
//
#define BASE 65521U     // largest prime smaller than 65536 
#define NMAX 5552       // NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 

#define DO1(buf, i)  {adler += (buf)[i]; sum2 += adler; }
#define DO2(buf, i)  DO1(buf, i); DO1(buf, i+1);
#define DO4(buf, i)  DO2(buf, i); DO2(buf, i+2);
#define DO8(buf, i)  DO4(buf, i); DO4(buf, i+4);
#define DO16(buf)   DO8(buf, 0); DO8(buf, 8);

// use NO_DIVIDE if your processor does not do division in hardware --
// try it both ways to see which is faster 
#ifdef NO_DIVIDE
	// note that this assumes BASE is 65521, where 65536 % 65521 == 15
   // (thank you to John Reiser for pointing this out) 
	#define CHOP(a) \
	do { \
		ulong  tmp = a >> 16; \
		a &= 0xffffUL; \
		a += (tmp << 4) - tmp; \
	} while(0)
#define MOD28(a) do { CHOP(a); if(a >= BASE) a -= BASE; } while(0)
#define MOD(a) do { CHOP(a); MOD28(a); } while(0)
#define MOD63(a) \
	do { \ // this assumes a is not negative 
		z_off64_t tmp = a >> 32; \
		a &= 0xffffffffL; \
		a += (tmp << 8) - (tmp << 5) + tmp; \
		tmp = a >> 16; \
		a &= 0xffffL; \
		a += (tmp << 4) - tmp; \
		tmp = a >> 16; \
		a &= 0xffffL; \
		a += (tmp << 4) - tmp; \
		if(a >= BASE) a -= BASE; \
	} while(0)
#else
	#define MOD(a) a %= BASE
	#define MOD28(a) a %= BASE
	#define MOD63(a) a %= BASE
#endif
//
//
uLong ZEXPORT adler32_z(uLong adler, const Bytef * buf, size_t len)
{
	uint   n;
	// split Adler-32 into component sums 
	ulong  sum2 = (adler >> 16) & 0xffff;
	adler &= 0xffff;
	// in case user likes doing a byte at a time, keep it fast 
	if(len == 1) {
		adler += buf[0];
		if(adler >= BASE)
			adler -= BASE;
		sum2 += adler;
		if(sum2 >= BASE)
			sum2 -= BASE;
		return adler | (sum2 << 16);
	}
	else if(buf == Z_NULL) { // initial Adler-32 value (deferred check for len == 1 speed) 
		return 1L;
	}
	else if(len < 16) { // in case short lengths are provided, keep it somewhat fast 
		while(len--) {
			adler += *buf++;
			sum2 += adler;
		}
		if(adler >= BASE)
			adler -= BASE;
		MOD28(sum2); // only added so many BASE's 
		return adler | (sum2 << 16);
	}
	else {
		// do length NMAX blocks -- requires just one modulo operation 
		while(len >= NMAX) {
			len -= NMAX;
			n = NMAX / 16; // NMAX is divisible by 16 
			do {
				DO16(buf); // 16 sums unrolled 
				buf += 16;
			} while(--n);
			MOD(adler);
			MOD(sum2);
		}
		// do remaining bytes (less than NMAX, still just one modulo) 
		if(len) { // avoid modulos if none remaining 
			while(len >= 16) {
				len -= 16;
				DO16(buf);
				buf += 16;
			}
			while(len--) {
				adler += *buf++;
				sum2 += adler;
			}
			MOD(adler);
			MOD(sum2);
		}
		return (adler | (sum2 << 16)); // return recombined sums 
	}
}

uLong ZEXPORT adler32(uLong adler, const Bytef * buf, uInt len)
{
	return adler32_z(adler, buf, len);
}

static uLong adler32_combine_(uLong adler1, uLong adler2, z_off64_t len2)
{
	ulong  sum1;
	ulong  sum2;
	uint   rem;
	// for negative len, return invalid adler32 as a clue for debugging 
	if(len2 < 0)
		return 0xffffffffUL;
	// the derivation of this formula is left as an exercise for the reader 
	MOD63(len2); // assumes len2 >= 0 
	rem = (uint)len2;
	sum1 = adler1 & 0xffff;
	sum2 = rem * sum1;
	MOD(sum2);
	sum1 += (adler2 & 0xffff) + BASE - 1;
	sum2 += ((adler1 >> 16) & 0xffff) + ((adler2 >> 16) & 0xffff) + BASE - rem;
	if(sum1 >= BASE) 
		sum1 -= BASE;
	if(sum1 >= BASE) 
		sum1 -= BASE;
	if(sum2 >= ((ulong)BASE << 1)) 
		sum2 -= ((ulong)BASE << 1);
	if(sum2 >= BASE) 
		sum2 -= BASE;
	return sum1 | (sum2 << 16);
}

uLong ZEXPORT adler32_combine(uLong adler1, uLong adler2, z_off_t len2)
	{ return adler32_combine_(adler1, adler2, len2); }
uLong ZEXPORT adler32_combine64(uLong adler1, uLong adler2, z_off64_t len2)
	{ return adler32_combine_(adler1, adler2, len2); }

#undef DO1
#undef DO2
#undef DO4
#undef DO8
#undef DO16
//
// CRC32
//
#ifdef MAKECRCH
	#ifndef DYNAMIC_CRC_TABLE
		#define DYNAMIC_CRC_TABLE
	#endif /* !DYNAMIC_CRC_TABLE */
#endif
//
// Definitions for doing the crc four data bytes at a time
//
#if !defined(NOBYFOUR) && defined(Z_U4)
	#define BYFOUR
#endif
#ifdef BYFOUR
	static ulong crc32_little(ulong, const uchar *, size_t);
	static ulong crc32_big(ulong, const uchar *, size_t);
	#define TBLS 8
#else
	#define TBLS 1
#endif /* BYFOUR */

#ifdef DYNAMIC_CRC_TABLE

static volatile int crc_table_empty = 1;
static z_crc_t crc_table[TBLS][256];
static void make_crc_table(void);
#ifdef MAKECRCH
	static void write_table(FILE *, const z_crc_t *);
#endif /* MAKECRCH */
/*
   Generate tables for a byte-wise 32-bit CRC calculation on the polynomial:
   x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1.

   Polynomials over GF(2) are represented in binary, one bit per coefficient,
   with the lowest powers in the most significant bit.  Then adding polynomials
   is just exclusive-or, and multiplying a polynomial by x is a right shift by
   one.  If we call the above polynomial p, and represent a byte as the
   polynomial q, also with the lowest power in the most significant bit (so the
   byte 0xb1 is the polynomial x^7+x^3+x+1), then the CRC is (q*x^32) mod p,
   where a mod b means the remainder after dividing a by b.

   This calculation is done using the shift-register method of multiplying and
   taking the remainder.  The register is initialized to zero, and for each
   incoming bit, x^32 is added mod p to the register if the bit is a one (where
   x^32 mod p is p+x^32 = x^26+...+1), and the register is multiplied mod p by
   x (which is shifting right by one and adding x^32 mod p if the bit shifted
   out is a one).  We start with the highest power (least significant bit) of
   q and repeat for all eight bits of q.

   The first table is simply the CRC of all possible eight bit values.  This is
   all the information needed to generate CRCs on data a byte at a time for all
   combinations of CRC register values and incoming bytes.  The remaining tables
   allow for word-at-a-time CRC calculation for both big-endian and little-
   endian machines, where a word is four bytes.
 */
static void make_crc_table()
{
	z_crc_t c;
	int n, k;
	z_crc_t poly;                   /* polynomial exclusive-or pattern */
	/* terms of polynomial defining this crc (except x^32): */
	static volatile int first = 1;  /* flag to limit concurrent making */
	static const uchar p[] = {0, 1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26};

	/* See if another task is already doing this (not thread-safe, but better
	   than nothing -- significantly reduces duration of vulnerability in
	   case the advice about DYNAMIC_CRC_TABLE is ignored) */
	if(first) {
		first = 0;
		// make exclusive-or pattern from polynomial (0xedb88320UL) 
		poly = 0;
		for(n = 0; n < (int)(sizeof(p)/sizeof(uchar)); n++)
			poly |= (z_crc_t)1 << (31 - p[n]);
		// generate a crc for every 8-bit value 
		for(n = 0; n < 256; n++) {
			c = (z_crc_t)n;
			for(k = 0; k < 8; k++)
				c = c & 1 ? poly ^ (c >> 1) : c >> 1;
			crc_table[0][n] = c;
		}
#ifdef BYFOUR
		// generate crc for each value followed by one, two, and three zeros,
		// and then the byte reversal of those as well as the first table 
		for(n = 0; n < 256; n++) {
			c = crc_table[0][n];
			crc_table[4][n] = ZSWAP32(c);
			for(k = 1; k < 4; k++) {
				c = crc_table[0][c & 0xff] ^ (c >> 8);
				crc_table[k][n] = c;
				crc_table[k + 4][n] = ZSWAP32(c);
			}
		}
#endif /* BYFOUR */

		crc_table_empty = 0;
	}
	else {  /* not first */
		/* wait for the other guy to finish (not efficient, but rare) */
		while(crc_table_empty)
			;
	}

#ifdef MAKECRCH
	/* write out CRC tables to crc32.h */
	{
		FILE * out = fopen("crc32.h", "w");
		if(out == NULL) return;
		fprintf(out, "/* crc32.h -- tables for rapid CRC calculation\n");
		fprintf(out, " * Generated automatically by crc32.c\n */\n\n");
		fprintf(out, "local const z_crc_t FAR ");
		fprintf(out, "crc_table[TBLS][256] =\n{\n  {\n");
		write_table(out, crc_table[0]);
#  ifdef BYFOUR
		fprintf(out, "#ifdef BYFOUR\n");
		for(k = 1; k < 8; k++) {
			fprintf(out, "  },\n  {\n");
			write_table(out, crc_table[k]);
		}
		fprintf(out, "#endif\n");
#  endif /* BYFOUR */
		fprintf(out, "  }\n};\n");
		fclose(out);
	}
#endif /* MAKECRCH */
}

#ifdef MAKECRCH
static void write_table(FILE * out, const z_crc_t  * table)
{
	for(int n = 0; n < 256; n++)
		fprintf(out, "%s0x%08lxUL%s", n % 5 ? "" : "    ", (ulong)(table[n]), n == 255 ? "\n" : (n % 5 == 4 ? ",\n" : ", "));
}
#endif /* MAKECRCH */

#else /* !DYNAMIC_CRC_TABLE */
	//
	// Tables of CRC-32s of all single-byte values, made by make_crc_table().
	//
	//#include "crc32.h"
	// crc32.h -- tables for rapid CRC calculation
	// Generated automatically by crc32.c
	//
	static const z_crc_t crc_table[TBLS][256] =
	{
		{
			0x00000000UL, 0x77073096UL, 0xee0e612cUL, 0x990951baUL, 0x076dc419UL,
			0x706af48fUL, 0xe963a535UL, 0x9e6495a3UL, 0x0edb8832UL, 0x79dcb8a4UL,
			0xe0d5e91eUL, 0x97d2d988UL, 0x09b64c2bUL, 0x7eb17cbdUL, 0xe7b82d07UL,
			0x90bf1d91UL, 0x1db71064UL, 0x6ab020f2UL, 0xf3b97148UL, 0x84be41deUL,
			0x1adad47dUL, 0x6ddde4ebUL, 0xf4d4b551UL, 0x83d385c7UL, 0x136c9856UL,
			0x646ba8c0UL, 0xfd62f97aUL, 0x8a65c9ecUL, 0x14015c4fUL, 0x63066cd9UL,
			0xfa0f3d63UL, 0x8d080df5UL, 0x3b6e20c8UL, 0x4c69105eUL, 0xd56041e4UL,
			0xa2677172UL, 0x3c03e4d1UL, 0x4b04d447UL, 0xd20d85fdUL, 0xa50ab56bUL,
			0x35b5a8faUL, 0x42b2986cUL, 0xdbbbc9d6UL, 0xacbcf940UL, 0x32d86ce3UL,
			0x45df5c75UL, 0xdcd60dcfUL, 0xabd13d59UL, 0x26d930acUL, 0x51de003aUL,
			0xc8d75180UL, 0xbfd06116UL, 0x21b4f4b5UL, 0x56b3c423UL, 0xcfba9599UL,
			0xb8bda50fUL, 0x2802b89eUL, 0x5f058808UL, 0xc60cd9b2UL, 0xb10be924UL,
			0x2f6f7c87UL, 0x58684c11UL, 0xc1611dabUL, 0xb6662d3dUL, 0x76dc4190UL,
			0x01db7106UL, 0x98d220bcUL, 0xefd5102aUL, 0x71b18589UL, 0x06b6b51fUL,
			0x9fbfe4a5UL, 0xe8b8d433UL, 0x7807c9a2UL, 0x0f00f934UL, 0x9609a88eUL,
			0xe10e9818UL, 0x7f6a0dbbUL, 0x086d3d2dUL, 0x91646c97UL, 0xe6635c01UL,
			0x6b6b51f4UL, 0x1c6c6162UL, 0x856530d8UL, 0xf262004eUL, 0x6c0695edUL,
			0x1b01a57bUL, 0x8208f4c1UL, 0xf50fc457UL, 0x65b0d9c6UL, 0x12b7e950UL,
			0x8bbeb8eaUL, 0xfcb9887cUL, 0x62dd1ddfUL, 0x15da2d49UL, 0x8cd37cf3UL,
			0xfbd44c65UL, 0x4db26158UL, 0x3ab551ceUL, 0xa3bc0074UL, 0xd4bb30e2UL,
			0x4adfa541UL, 0x3dd895d7UL, 0xa4d1c46dUL, 0xd3d6f4fbUL, 0x4369e96aUL,
			0x346ed9fcUL, 0xad678846UL, 0xda60b8d0UL, 0x44042d73UL, 0x33031de5UL,
			0xaa0a4c5fUL, 0xdd0d7cc9UL, 0x5005713cUL, 0x270241aaUL, 0xbe0b1010UL,
			0xc90c2086UL, 0x5768b525UL, 0x206f85b3UL, 0xb966d409UL, 0xce61e49fUL,
			0x5edef90eUL, 0x29d9c998UL, 0xb0d09822UL, 0xc7d7a8b4UL, 0x59b33d17UL,
			0x2eb40d81UL, 0xb7bd5c3bUL, 0xc0ba6cadUL, 0xedb88320UL, 0x9abfb3b6UL,
			0x03b6e20cUL, 0x74b1d29aUL, 0xead54739UL, 0x9dd277afUL, 0x04db2615UL,
			0x73dc1683UL, 0xe3630b12UL, 0x94643b84UL, 0x0d6d6a3eUL, 0x7a6a5aa8UL,
			0xe40ecf0bUL, 0x9309ff9dUL, 0x0a00ae27UL, 0x7d079eb1UL, 0xf00f9344UL,
			0x8708a3d2UL, 0x1e01f268UL, 0x6906c2feUL, 0xf762575dUL, 0x806567cbUL,
			0x196c3671UL, 0x6e6b06e7UL, 0xfed41b76UL, 0x89d32be0UL, 0x10da7a5aUL,
			0x67dd4accUL, 0xf9b9df6fUL, 0x8ebeeff9UL, 0x17b7be43UL, 0x60b08ed5UL,
			0xd6d6a3e8UL, 0xa1d1937eUL, 0x38d8c2c4UL, 0x4fdff252UL, 0xd1bb67f1UL,
			0xa6bc5767UL, 0x3fb506ddUL, 0x48b2364bUL, 0xd80d2bdaUL, 0xaf0a1b4cUL,
			0x36034af6UL, 0x41047a60UL, 0xdf60efc3UL, 0xa867df55UL, 0x316e8eefUL,
			0x4669be79UL, 0xcb61b38cUL, 0xbc66831aUL, 0x256fd2a0UL, 0x5268e236UL,
			0xcc0c7795UL, 0xbb0b4703UL, 0x220216b9UL, 0x5505262fUL, 0xc5ba3bbeUL,
			0xb2bd0b28UL, 0x2bb45a92UL, 0x5cb36a04UL, 0xc2d7ffa7UL, 0xb5d0cf31UL,
			0x2cd99e8bUL, 0x5bdeae1dUL, 0x9b64c2b0UL, 0xec63f226UL, 0x756aa39cUL,
			0x026d930aUL, 0x9c0906a9UL, 0xeb0e363fUL, 0x72076785UL, 0x05005713UL,
			0x95bf4a82UL, 0xe2b87a14UL, 0x7bb12baeUL, 0x0cb61b38UL, 0x92d28e9bUL,
			0xe5d5be0dUL, 0x7cdcefb7UL, 0x0bdbdf21UL, 0x86d3d2d4UL, 0xf1d4e242UL,
			0x68ddb3f8UL, 0x1fda836eUL, 0x81be16cdUL, 0xf6b9265bUL, 0x6fb077e1UL,
			0x18b74777UL, 0x88085ae6UL, 0xff0f6a70UL, 0x66063bcaUL, 0x11010b5cUL,
			0x8f659effUL, 0xf862ae69UL, 0x616bffd3UL, 0x166ccf45UL, 0xa00ae278UL,
			0xd70dd2eeUL, 0x4e048354UL, 0x3903b3c2UL, 0xa7672661UL, 0xd06016f7UL,
			0x4969474dUL, 0x3e6e77dbUL, 0xaed16a4aUL, 0xd9d65adcUL, 0x40df0b66UL,
			0x37d83bf0UL, 0xa9bcae53UL, 0xdebb9ec5UL, 0x47b2cf7fUL, 0x30b5ffe9UL,
			0xbdbdf21cUL, 0xcabac28aUL, 0x53b39330UL, 0x24b4a3a6UL, 0xbad03605UL,
			0xcdd70693UL, 0x54de5729UL, 0x23d967bfUL, 0xb3667a2eUL, 0xc4614ab8UL,
			0x5d681b02UL, 0x2a6f2b94UL, 0xb40bbe37UL, 0xc30c8ea1UL, 0x5a05df1bUL,
			0x2d02ef8dUL
	#ifdef BYFOUR
		},
		{
			0x00000000UL, 0x191b3141UL, 0x32366282UL, 0x2b2d53c3UL, 0x646cc504UL,
			0x7d77f445UL, 0x565aa786UL, 0x4f4196c7UL, 0xc8d98a08UL, 0xd1c2bb49UL,
			0xfaefe88aUL, 0xe3f4d9cbUL, 0xacb54f0cUL, 0xb5ae7e4dUL, 0x9e832d8eUL,
			0x87981ccfUL, 0x4ac21251UL, 0x53d92310UL, 0x78f470d3UL, 0x61ef4192UL,
			0x2eaed755UL, 0x37b5e614UL, 0x1c98b5d7UL, 0x05838496UL, 0x821b9859UL,
			0x9b00a918UL, 0xb02dfadbUL, 0xa936cb9aUL, 0xe6775d5dUL, 0xff6c6c1cUL,
			0xd4413fdfUL, 0xcd5a0e9eUL, 0x958424a2UL, 0x8c9f15e3UL, 0xa7b24620UL,
			0xbea97761UL, 0xf1e8e1a6UL, 0xe8f3d0e7UL, 0xc3de8324UL, 0xdac5b265UL,
			0x5d5daeaaUL, 0x44469febUL, 0x6f6bcc28UL, 0x7670fd69UL, 0x39316baeUL,
			0x202a5aefUL, 0x0b07092cUL, 0x121c386dUL, 0xdf4636f3UL, 0xc65d07b2UL,
			0xed705471UL, 0xf46b6530UL, 0xbb2af3f7UL, 0xa231c2b6UL, 0x891c9175UL,
			0x9007a034UL, 0x179fbcfbUL, 0x0e848dbaUL, 0x25a9de79UL, 0x3cb2ef38UL,
			0x73f379ffUL, 0x6ae848beUL, 0x41c51b7dUL, 0x58de2a3cUL, 0xf0794f05UL,
			0xe9627e44UL, 0xc24f2d87UL, 0xdb541cc6UL, 0x94158a01UL, 0x8d0ebb40UL,
			0xa623e883UL, 0xbf38d9c2UL, 0x38a0c50dUL, 0x21bbf44cUL, 0x0a96a78fUL,
			0x138d96ceUL, 0x5ccc0009UL, 0x45d73148UL, 0x6efa628bUL, 0x77e153caUL,
			0xbabb5d54UL, 0xa3a06c15UL, 0x888d3fd6UL, 0x91960e97UL, 0xded79850UL,
			0xc7cca911UL, 0xece1fad2UL, 0xf5facb93UL, 0x7262d75cUL, 0x6b79e61dUL,
			0x4054b5deUL, 0x594f849fUL, 0x160e1258UL, 0x0f152319UL, 0x243870daUL,
			0x3d23419bUL, 0x65fd6ba7UL, 0x7ce65ae6UL, 0x57cb0925UL, 0x4ed03864UL,
			0x0191aea3UL, 0x188a9fe2UL, 0x33a7cc21UL, 0x2abcfd60UL, 0xad24e1afUL,
			0xb43fd0eeUL, 0x9f12832dUL, 0x8609b26cUL, 0xc94824abUL, 0xd05315eaUL,
			0xfb7e4629UL, 0xe2657768UL, 0x2f3f79f6UL, 0x362448b7UL, 0x1d091b74UL,
			0x04122a35UL, 0x4b53bcf2UL, 0x52488db3UL, 0x7965de70UL, 0x607eef31UL,
			0xe7e6f3feUL, 0xfefdc2bfUL, 0xd5d0917cUL, 0xcccba03dUL, 0x838a36faUL,
			0x9a9107bbUL, 0xb1bc5478UL, 0xa8a76539UL, 0x3b83984bUL, 0x2298a90aUL,
			0x09b5fac9UL, 0x10aecb88UL, 0x5fef5d4fUL, 0x46f46c0eUL, 0x6dd93fcdUL,
			0x74c20e8cUL, 0xf35a1243UL, 0xea412302UL, 0xc16c70c1UL, 0xd8774180UL,
			0x9736d747UL, 0x8e2de606UL, 0xa500b5c5UL, 0xbc1b8484UL, 0x71418a1aUL,
			0x685abb5bUL, 0x4377e898UL, 0x5a6cd9d9UL, 0x152d4f1eUL, 0x0c367e5fUL,
			0x271b2d9cUL, 0x3e001cddUL, 0xb9980012UL, 0xa0833153UL, 0x8bae6290UL,
			0x92b553d1UL, 0xddf4c516UL, 0xc4eff457UL, 0xefc2a794UL, 0xf6d996d5UL,
			0xae07bce9UL, 0xb71c8da8UL, 0x9c31de6bUL, 0x852aef2aUL, 0xca6b79edUL,
			0xd37048acUL, 0xf85d1b6fUL, 0xe1462a2eUL, 0x66de36e1UL, 0x7fc507a0UL,
			0x54e85463UL, 0x4df36522UL, 0x02b2f3e5UL, 0x1ba9c2a4UL, 0x30849167UL,
			0x299fa026UL, 0xe4c5aeb8UL, 0xfdde9ff9UL, 0xd6f3cc3aUL, 0xcfe8fd7bUL,
			0x80a96bbcUL, 0x99b25afdUL, 0xb29f093eUL, 0xab84387fUL, 0x2c1c24b0UL,
			0x350715f1UL, 0x1e2a4632UL, 0x07317773UL, 0x4870e1b4UL, 0x516bd0f5UL,
			0x7a468336UL, 0x635db277UL, 0xcbfad74eUL, 0xd2e1e60fUL, 0xf9ccb5ccUL,
			0xe0d7848dUL, 0xaf96124aUL, 0xb68d230bUL, 0x9da070c8UL, 0x84bb4189UL,
			0x03235d46UL, 0x1a386c07UL, 0x31153fc4UL, 0x280e0e85UL, 0x674f9842UL,
			0x7e54a903UL, 0x5579fac0UL, 0x4c62cb81UL, 0x8138c51fUL, 0x9823f45eUL,
			0xb30ea79dUL, 0xaa1596dcUL, 0xe554001bUL, 0xfc4f315aUL, 0xd7626299UL,
			0xce7953d8UL, 0x49e14f17UL, 0x50fa7e56UL, 0x7bd72d95UL, 0x62cc1cd4UL,
			0x2d8d8a13UL, 0x3496bb52UL, 0x1fbbe891UL, 0x06a0d9d0UL, 0x5e7ef3ecUL,
			0x4765c2adUL, 0x6c48916eUL, 0x7553a02fUL, 0x3a1236e8UL, 0x230907a9UL,
			0x0824546aUL, 0x113f652bUL, 0x96a779e4UL, 0x8fbc48a5UL, 0xa4911b66UL,
			0xbd8a2a27UL, 0xf2cbbce0UL, 0xebd08da1UL, 0xc0fdde62UL, 0xd9e6ef23UL,
			0x14bce1bdUL, 0x0da7d0fcUL, 0x268a833fUL, 0x3f91b27eUL, 0x70d024b9UL,
			0x69cb15f8UL, 0x42e6463bUL, 0x5bfd777aUL, 0xdc656bb5UL, 0xc57e5af4UL,
			0xee530937UL, 0xf7483876UL, 0xb809aeb1UL, 0xa1129ff0UL, 0x8a3fcc33UL,
			0x9324fd72UL
		},
		{
			0x00000000UL, 0x01c26a37UL, 0x0384d46eUL, 0x0246be59UL, 0x0709a8dcUL,
			0x06cbc2ebUL, 0x048d7cb2UL, 0x054f1685UL, 0x0e1351b8UL, 0x0fd13b8fUL,
			0x0d9785d6UL, 0x0c55efe1UL, 0x091af964UL, 0x08d89353UL, 0x0a9e2d0aUL,
			0x0b5c473dUL, 0x1c26a370UL, 0x1de4c947UL, 0x1fa2771eUL, 0x1e601d29UL,
			0x1b2f0bacUL, 0x1aed619bUL, 0x18abdfc2UL, 0x1969b5f5UL, 0x1235f2c8UL,
			0x13f798ffUL, 0x11b126a6UL, 0x10734c91UL, 0x153c5a14UL, 0x14fe3023UL,
			0x16b88e7aUL, 0x177ae44dUL, 0x384d46e0UL, 0x398f2cd7UL, 0x3bc9928eUL,
			0x3a0bf8b9UL, 0x3f44ee3cUL, 0x3e86840bUL, 0x3cc03a52UL, 0x3d025065UL,
			0x365e1758UL, 0x379c7d6fUL, 0x35dac336UL, 0x3418a901UL, 0x3157bf84UL,
			0x3095d5b3UL, 0x32d36beaUL, 0x331101ddUL, 0x246be590UL, 0x25a98fa7UL,
			0x27ef31feUL, 0x262d5bc9UL, 0x23624d4cUL, 0x22a0277bUL, 0x20e69922UL,
			0x2124f315UL, 0x2a78b428UL, 0x2bbade1fUL, 0x29fc6046UL, 0x283e0a71UL,
			0x2d711cf4UL, 0x2cb376c3UL, 0x2ef5c89aUL, 0x2f37a2adUL, 0x709a8dc0UL,
			0x7158e7f7UL, 0x731e59aeUL, 0x72dc3399UL, 0x7793251cUL, 0x76514f2bUL,
			0x7417f172UL, 0x75d59b45UL, 0x7e89dc78UL, 0x7f4bb64fUL, 0x7d0d0816UL,
			0x7ccf6221UL, 0x798074a4UL, 0x78421e93UL, 0x7a04a0caUL, 0x7bc6cafdUL,
			0x6cbc2eb0UL, 0x6d7e4487UL, 0x6f38fadeUL, 0x6efa90e9UL, 0x6bb5866cUL,
			0x6a77ec5bUL, 0x68315202UL, 0x69f33835UL, 0x62af7f08UL, 0x636d153fUL,
			0x612bab66UL, 0x60e9c151UL, 0x65a6d7d4UL, 0x6464bde3UL, 0x662203baUL,
			0x67e0698dUL, 0x48d7cb20UL, 0x4915a117UL, 0x4b531f4eUL, 0x4a917579UL,
			0x4fde63fcUL, 0x4e1c09cbUL, 0x4c5ab792UL, 0x4d98dda5UL, 0x46c49a98UL,
			0x4706f0afUL, 0x45404ef6UL, 0x448224c1UL, 0x41cd3244UL, 0x400f5873UL,
			0x4249e62aUL, 0x438b8c1dUL, 0x54f16850UL, 0x55330267UL, 0x5775bc3eUL,
			0x56b7d609UL, 0x53f8c08cUL, 0x523aaabbUL, 0x507c14e2UL, 0x51be7ed5UL,
			0x5ae239e8UL, 0x5b2053dfUL, 0x5966ed86UL, 0x58a487b1UL, 0x5deb9134UL,
			0x5c29fb03UL, 0x5e6f455aUL, 0x5fad2f6dUL, 0xe1351b80UL, 0xe0f771b7UL,
			0xe2b1cfeeUL, 0xe373a5d9UL, 0xe63cb35cUL, 0xe7fed96bUL, 0xe5b86732UL,
			0xe47a0d05UL, 0xef264a38UL, 0xeee4200fUL, 0xeca29e56UL, 0xed60f461UL,
			0xe82fe2e4UL, 0xe9ed88d3UL, 0xebab368aUL, 0xea695cbdUL, 0xfd13b8f0UL,
			0xfcd1d2c7UL, 0xfe976c9eUL, 0xff5506a9UL, 0xfa1a102cUL, 0xfbd87a1bUL,
			0xf99ec442UL, 0xf85cae75UL, 0xf300e948UL, 0xf2c2837fUL, 0xf0843d26UL,
			0xf1465711UL, 0xf4094194UL, 0xf5cb2ba3UL, 0xf78d95faUL, 0xf64fffcdUL,
			0xd9785d60UL, 0xd8ba3757UL, 0xdafc890eUL, 0xdb3ee339UL, 0xde71f5bcUL,
			0xdfb39f8bUL, 0xddf521d2UL, 0xdc374be5UL, 0xd76b0cd8UL, 0xd6a966efUL,
			0xd4efd8b6UL, 0xd52db281UL, 0xd062a404UL, 0xd1a0ce33UL, 0xd3e6706aUL,
			0xd2241a5dUL, 0xc55efe10UL, 0xc49c9427UL, 0xc6da2a7eUL, 0xc7184049UL,
			0xc25756ccUL, 0xc3953cfbUL, 0xc1d382a2UL, 0xc011e895UL, 0xcb4dafa8UL,
			0xca8fc59fUL, 0xc8c97bc6UL, 0xc90b11f1UL, 0xcc440774UL, 0xcd866d43UL,
			0xcfc0d31aUL, 0xce02b92dUL, 0x91af9640UL, 0x906dfc77UL, 0x922b422eUL,
			0x93e92819UL, 0x96a63e9cUL, 0x976454abUL, 0x9522eaf2UL, 0x94e080c5UL,
			0x9fbcc7f8UL, 0x9e7eadcfUL, 0x9c381396UL, 0x9dfa79a1UL, 0x98b56f24UL,
			0x99770513UL, 0x9b31bb4aUL, 0x9af3d17dUL, 0x8d893530UL, 0x8c4b5f07UL,
			0x8e0de15eUL, 0x8fcf8b69UL, 0x8a809decUL, 0x8b42f7dbUL, 0x89044982UL,
			0x88c623b5UL, 0x839a6488UL, 0x82580ebfUL, 0x801eb0e6UL, 0x81dcdad1UL,
			0x8493cc54UL, 0x8551a663UL, 0x8717183aUL, 0x86d5720dUL, 0xa9e2d0a0UL,
			0xa820ba97UL, 0xaa6604ceUL, 0xaba46ef9UL, 0xaeeb787cUL, 0xaf29124bUL,
			0xad6fac12UL, 0xacadc625UL, 0xa7f18118UL, 0xa633eb2fUL, 0xa4755576UL,
			0xa5b73f41UL, 0xa0f829c4UL, 0xa13a43f3UL, 0xa37cfdaaUL, 0xa2be979dUL,
			0xb5c473d0UL, 0xb40619e7UL, 0xb640a7beUL, 0xb782cd89UL, 0xb2cddb0cUL,
			0xb30fb13bUL, 0xb1490f62UL, 0xb08b6555UL, 0xbbd72268UL, 0xba15485fUL,
			0xb853f606UL, 0xb9919c31UL, 0xbcde8ab4UL, 0xbd1ce083UL, 0xbf5a5edaUL,
			0xbe9834edUL
		},
		{
			0x00000000UL, 0xb8bc6765UL, 0xaa09c88bUL, 0x12b5afeeUL, 0x8f629757UL,
			0x37def032UL, 0x256b5fdcUL, 0x9dd738b9UL, 0xc5b428efUL, 0x7d084f8aUL,
			0x6fbde064UL, 0xd7018701UL, 0x4ad6bfb8UL, 0xf26ad8ddUL, 0xe0df7733UL,
			0x58631056UL, 0x5019579fUL, 0xe8a530faUL, 0xfa109f14UL, 0x42acf871UL,
			0xdf7bc0c8UL, 0x67c7a7adUL, 0x75720843UL, 0xcdce6f26UL, 0x95ad7f70UL,
			0x2d111815UL, 0x3fa4b7fbUL, 0x8718d09eUL, 0x1acfe827UL, 0xa2738f42UL,
			0xb0c620acUL, 0x087a47c9UL, 0xa032af3eUL, 0x188ec85bUL, 0x0a3b67b5UL,
			0xb28700d0UL, 0x2f503869UL, 0x97ec5f0cUL, 0x8559f0e2UL, 0x3de59787UL,
			0x658687d1UL, 0xdd3ae0b4UL, 0xcf8f4f5aUL, 0x7733283fUL, 0xeae41086UL,
			0x525877e3UL, 0x40edd80dUL, 0xf851bf68UL, 0xf02bf8a1UL, 0x48979fc4UL,
			0x5a22302aUL, 0xe29e574fUL, 0x7f496ff6UL, 0xc7f50893UL, 0xd540a77dUL,
			0x6dfcc018UL, 0x359fd04eUL, 0x8d23b72bUL, 0x9f9618c5UL, 0x272a7fa0UL,
			0xbafd4719UL, 0x0241207cUL, 0x10f48f92UL, 0xa848e8f7UL, 0x9b14583dUL,
			0x23a83f58UL, 0x311d90b6UL, 0x89a1f7d3UL, 0x1476cf6aUL, 0xaccaa80fUL,
			0xbe7f07e1UL, 0x06c36084UL, 0x5ea070d2UL, 0xe61c17b7UL, 0xf4a9b859UL,
			0x4c15df3cUL, 0xd1c2e785UL, 0x697e80e0UL, 0x7bcb2f0eUL, 0xc377486bUL,
			0xcb0d0fa2UL, 0x73b168c7UL, 0x6104c729UL, 0xd9b8a04cUL, 0x446f98f5UL,
			0xfcd3ff90UL, 0xee66507eUL, 0x56da371bUL, 0x0eb9274dUL, 0xb6054028UL,
			0xa4b0efc6UL, 0x1c0c88a3UL, 0x81dbb01aUL, 0x3967d77fUL, 0x2bd27891UL,
			0x936e1ff4UL, 0x3b26f703UL, 0x839a9066UL, 0x912f3f88UL, 0x299358edUL,
			0xb4446054UL, 0x0cf80731UL, 0x1e4da8dfUL, 0xa6f1cfbaUL, 0xfe92dfecUL,
			0x462eb889UL, 0x549b1767UL, 0xec277002UL, 0x71f048bbUL, 0xc94c2fdeUL,
			0xdbf98030UL, 0x6345e755UL, 0x6b3fa09cUL, 0xd383c7f9UL, 0xc1366817UL,
			0x798a0f72UL, 0xe45d37cbUL, 0x5ce150aeUL, 0x4e54ff40UL, 0xf6e89825UL,
			0xae8b8873UL, 0x1637ef16UL, 0x048240f8UL, 0xbc3e279dUL, 0x21e91f24UL,
			0x99557841UL, 0x8be0d7afUL, 0x335cb0caUL, 0xed59b63bUL, 0x55e5d15eUL,
			0x47507eb0UL, 0xffec19d5UL, 0x623b216cUL, 0xda874609UL, 0xc832e9e7UL,
			0x708e8e82UL, 0x28ed9ed4UL, 0x9051f9b1UL, 0x82e4565fUL, 0x3a58313aUL,
			0xa78f0983UL, 0x1f336ee6UL, 0x0d86c108UL, 0xb53aa66dUL, 0xbd40e1a4UL,
			0x05fc86c1UL, 0x1749292fUL, 0xaff54e4aUL, 0x322276f3UL, 0x8a9e1196UL,
			0x982bbe78UL, 0x2097d91dUL, 0x78f4c94bUL, 0xc048ae2eUL, 0xd2fd01c0UL,
			0x6a4166a5UL, 0xf7965e1cUL, 0x4f2a3979UL, 0x5d9f9697UL, 0xe523f1f2UL,
			0x4d6b1905UL, 0xf5d77e60UL, 0xe762d18eUL, 0x5fdeb6ebUL, 0xc2098e52UL,
			0x7ab5e937UL, 0x680046d9UL, 0xd0bc21bcUL, 0x88df31eaUL, 0x3063568fUL,
			0x22d6f961UL, 0x9a6a9e04UL, 0x07bda6bdUL, 0xbf01c1d8UL, 0xadb46e36UL,
			0x15080953UL, 0x1d724e9aUL, 0xa5ce29ffUL, 0xb77b8611UL, 0x0fc7e174UL,
			0x9210d9cdUL, 0x2aacbea8UL, 0x38191146UL, 0x80a57623UL, 0xd8c66675UL,
			0x607a0110UL, 0x72cfaefeUL, 0xca73c99bUL, 0x57a4f122UL, 0xef189647UL,
			0xfdad39a9UL, 0x45115eccUL, 0x764dee06UL, 0xcef18963UL, 0xdc44268dUL,
			0x64f841e8UL, 0xf92f7951UL, 0x41931e34UL, 0x5326b1daUL, 0xeb9ad6bfUL,
			0xb3f9c6e9UL, 0x0b45a18cUL, 0x19f00e62UL, 0xa14c6907UL, 0x3c9b51beUL,
			0x842736dbUL, 0x96929935UL, 0x2e2efe50UL, 0x2654b999UL, 0x9ee8defcUL,
			0x8c5d7112UL, 0x34e11677UL, 0xa9362eceUL, 0x118a49abUL, 0x033fe645UL,
			0xbb838120UL, 0xe3e09176UL, 0x5b5cf613UL, 0x49e959fdUL, 0xf1553e98UL,
			0x6c820621UL, 0xd43e6144UL, 0xc68bceaaUL, 0x7e37a9cfUL, 0xd67f4138UL,
			0x6ec3265dUL, 0x7c7689b3UL, 0xc4caeed6UL, 0x591dd66fUL, 0xe1a1b10aUL,
			0xf3141ee4UL, 0x4ba87981UL, 0x13cb69d7UL, 0xab770eb2UL, 0xb9c2a15cUL,
			0x017ec639UL, 0x9ca9fe80UL, 0x241599e5UL, 0x36a0360bUL, 0x8e1c516eUL,
			0x866616a7UL, 0x3eda71c2UL, 0x2c6fde2cUL, 0x94d3b949UL, 0x090481f0UL,
			0xb1b8e695UL, 0xa30d497bUL, 0x1bb12e1eUL, 0x43d23e48UL, 0xfb6e592dUL,
			0xe9dbf6c3UL, 0x516791a6UL, 0xccb0a91fUL, 0x740cce7aUL, 0x66b96194UL,
			0xde0506f1UL
		},
		{
			0x00000000UL, 0x96300777UL, 0x2c610eeeUL, 0xba510999UL, 0x19c46d07UL,
			0x8ff46a70UL, 0x35a563e9UL, 0xa395649eUL, 0x3288db0eUL, 0xa4b8dc79UL,
			0x1ee9d5e0UL, 0x88d9d297UL, 0x2b4cb609UL, 0xbd7cb17eUL, 0x072db8e7UL,
			0x911dbf90UL, 0x6410b71dUL, 0xf220b06aUL, 0x4871b9f3UL, 0xde41be84UL,
			0x7dd4da1aUL, 0xebe4dd6dUL, 0x51b5d4f4UL, 0xc785d383UL, 0x56986c13UL,
			0xc0a86b64UL, 0x7af962fdUL, 0xecc9658aUL, 0x4f5c0114UL, 0xd96c0663UL,
			0x633d0ffaUL, 0xf50d088dUL, 0xc8206e3bUL, 0x5e10694cUL, 0xe44160d5UL,
			0x727167a2UL, 0xd1e4033cUL, 0x47d4044bUL, 0xfd850dd2UL, 0x6bb50aa5UL,
			0xfaa8b535UL, 0x6c98b242UL, 0xd6c9bbdbUL, 0x40f9bcacUL, 0xe36cd832UL,
			0x755cdf45UL, 0xcf0dd6dcUL, 0x593dd1abUL, 0xac30d926UL, 0x3a00de51UL,
			0x8051d7c8UL, 0x1661d0bfUL, 0xb5f4b421UL, 0x23c4b356UL, 0x9995bacfUL,
			0x0fa5bdb8UL, 0x9eb80228UL, 0x0888055fUL, 0xb2d90cc6UL, 0x24e90bb1UL,
			0x877c6f2fUL, 0x114c6858UL, 0xab1d61c1UL, 0x3d2d66b6UL, 0x9041dc76UL,
			0x0671db01UL, 0xbc20d298UL, 0x2a10d5efUL, 0x8985b171UL, 0x1fb5b606UL,
			0xa5e4bf9fUL, 0x33d4b8e8UL, 0xa2c90778UL, 0x34f9000fUL, 0x8ea80996UL,
			0x18980ee1UL, 0xbb0d6a7fUL, 0x2d3d6d08UL, 0x976c6491UL, 0x015c63e6UL,
			0xf4516b6bUL, 0x62616c1cUL, 0xd8306585UL, 0x4e0062f2UL, 0xed95066cUL,
			0x7ba5011bUL, 0xc1f40882UL, 0x57c40ff5UL, 0xc6d9b065UL, 0x50e9b712UL,
			0xeab8be8bUL, 0x7c88b9fcUL, 0xdf1ddd62UL, 0x492dda15UL, 0xf37cd38cUL,
			0x654cd4fbUL, 0x5861b24dUL, 0xce51b53aUL, 0x7400bca3UL, 0xe230bbd4UL,
			0x41a5df4aUL, 0xd795d83dUL, 0x6dc4d1a4UL, 0xfbf4d6d3UL, 0x6ae96943UL,
			0xfcd96e34UL, 0x468867adUL, 0xd0b860daUL, 0x732d0444UL, 0xe51d0333UL,
			0x5f4c0aaaUL, 0xc97c0dddUL, 0x3c710550UL, 0xaa410227UL, 0x10100bbeUL,
			0x86200cc9UL, 0x25b56857UL, 0xb3856f20UL, 0x09d466b9UL, 0x9fe461ceUL,
			0x0ef9de5eUL, 0x98c9d929UL, 0x2298d0b0UL, 0xb4a8d7c7UL, 0x173db359UL,
			0x810db42eUL, 0x3b5cbdb7UL, 0xad6cbac0UL, 0x2083b8edUL, 0xb6b3bf9aUL,
			0x0ce2b603UL, 0x9ad2b174UL, 0x3947d5eaUL, 0xaf77d29dUL, 0x1526db04UL,
			0x8316dc73UL, 0x120b63e3UL, 0x843b6494UL, 0x3e6a6d0dUL, 0xa85a6a7aUL,
			0x0bcf0ee4UL, 0x9dff0993UL, 0x27ae000aUL, 0xb19e077dUL, 0x44930ff0UL,
			0xd2a30887UL, 0x68f2011eUL, 0xfec20669UL, 0x5d5762f7UL, 0xcb676580UL,
			0x71366c19UL, 0xe7066b6eUL, 0x761bd4feUL, 0xe02bd389UL, 0x5a7ada10UL,
			0xcc4add67UL, 0x6fdfb9f9UL, 0xf9efbe8eUL, 0x43beb717UL, 0xd58eb060UL,
			0xe8a3d6d6UL, 0x7e93d1a1UL, 0xc4c2d838UL, 0x52f2df4fUL, 0xf167bbd1UL,
			0x6757bca6UL, 0xdd06b53fUL, 0x4b36b248UL, 0xda2b0dd8UL, 0x4c1b0aafUL,
			0xf64a0336UL, 0x607a0441UL, 0xc3ef60dfUL, 0x55df67a8UL, 0xef8e6e31UL,
			0x79be6946UL, 0x8cb361cbUL, 0x1a8366bcUL, 0xa0d26f25UL, 0x36e26852UL,
			0x95770cccUL, 0x03470bbbUL, 0xb9160222UL, 0x2f260555UL, 0xbe3bbac5UL,
			0x280bbdb2UL, 0x925ab42bUL, 0x046ab35cUL, 0xa7ffd7c2UL, 0x31cfd0b5UL,
			0x8b9ed92cUL, 0x1daede5bUL, 0xb0c2649bUL, 0x26f263ecUL, 0x9ca36a75UL,
			0x0a936d02UL, 0xa906099cUL, 0x3f360eebUL, 0x85670772UL, 0x13570005UL,
			0x824abf95UL, 0x147ab8e2UL, 0xae2bb17bUL, 0x381bb60cUL, 0x9b8ed292UL,
			0x0dbed5e5UL, 0xb7efdc7cUL, 0x21dfdb0bUL, 0xd4d2d386UL, 0x42e2d4f1UL,
			0xf8b3dd68UL, 0x6e83da1fUL, 0xcd16be81UL, 0x5b26b9f6UL, 0xe177b06fUL,
			0x7747b718UL, 0xe65a0888UL, 0x706a0fffUL, 0xca3b0666UL, 0x5c0b0111UL,
			0xff9e658fUL, 0x69ae62f8UL, 0xd3ff6b61UL, 0x45cf6c16UL, 0x78e20aa0UL,
			0xeed20dd7UL, 0x5483044eUL, 0xc2b30339UL, 0x612667a7UL, 0xf71660d0UL,
			0x4d476949UL, 0xdb776e3eUL, 0x4a6ad1aeUL, 0xdc5ad6d9UL, 0x660bdf40UL,
			0xf03bd837UL, 0x53aebca9UL, 0xc59ebbdeUL, 0x7fcfb247UL, 0xe9ffb530UL,
			0x1cf2bdbdUL, 0x8ac2bacaUL, 0x3093b353UL, 0xa6a3b424UL, 0x0536d0baUL,
			0x9306d7cdUL, 0x2957de54UL, 0xbf67d923UL, 0x2e7a66b3UL, 0xb84a61c4UL,
			0x021b685dUL, 0x942b6f2aUL, 0x37be0bb4UL, 0xa18e0cc3UL, 0x1bdf055aUL,
			0x8def022dUL
		},
		{
			0x00000000UL, 0x41311b19UL, 0x82623632UL, 0xc3532d2bUL, 0x04c56c64UL,
			0x45f4777dUL, 0x86a75a56UL, 0xc796414fUL, 0x088ad9c8UL, 0x49bbc2d1UL,
			0x8ae8effaUL, 0xcbd9f4e3UL, 0x0c4fb5acUL, 0x4d7eaeb5UL, 0x8e2d839eUL,
			0xcf1c9887UL, 0x5112c24aUL, 0x1023d953UL, 0xd370f478UL, 0x9241ef61UL,
			0x55d7ae2eUL, 0x14e6b537UL, 0xd7b5981cUL, 0x96848305UL, 0x59981b82UL,
			0x18a9009bUL, 0xdbfa2db0UL, 0x9acb36a9UL, 0x5d5d77e6UL, 0x1c6c6cffUL,
			0xdf3f41d4UL, 0x9e0e5acdUL, 0xa2248495UL, 0xe3159f8cUL, 0x2046b2a7UL,
			0x6177a9beUL, 0xa6e1e8f1UL, 0xe7d0f3e8UL, 0x2483dec3UL, 0x65b2c5daUL,
			0xaaae5d5dUL, 0xeb9f4644UL, 0x28cc6b6fUL, 0x69fd7076UL, 0xae6b3139UL,
			0xef5a2a20UL, 0x2c09070bUL, 0x6d381c12UL, 0xf33646dfUL, 0xb2075dc6UL,
			0x715470edUL, 0x30656bf4UL, 0xf7f32abbUL, 0xb6c231a2UL, 0x75911c89UL,
			0x34a00790UL, 0xfbbc9f17UL, 0xba8d840eUL, 0x79dea925UL, 0x38efb23cUL,
			0xff79f373UL, 0xbe48e86aUL, 0x7d1bc541UL, 0x3c2ade58UL, 0x054f79f0UL,
			0x447e62e9UL, 0x872d4fc2UL, 0xc61c54dbUL, 0x018a1594UL, 0x40bb0e8dUL,
			0x83e823a6UL, 0xc2d938bfUL, 0x0dc5a038UL, 0x4cf4bb21UL, 0x8fa7960aUL,
			0xce968d13UL, 0x0900cc5cUL, 0x4831d745UL, 0x8b62fa6eUL, 0xca53e177UL,
			0x545dbbbaUL, 0x156ca0a3UL, 0xd63f8d88UL, 0x970e9691UL, 0x5098d7deUL,
			0x11a9ccc7UL, 0xd2fae1ecUL, 0x93cbfaf5UL, 0x5cd76272UL, 0x1de6796bUL,
			0xdeb55440UL, 0x9f844f59UL, 0x58120e16UL, 0x1923150fUL, 0xda703824UL,
			0x9b41233dUL, 0xa76bfd65UL, 0xe65ae67cUL, 0x2509cb57UL, 0x6438d04eUL,
			0xa3ae9101UL, 0xe29f8a18UL, 0x21cca733UL, 0x60fdbc2aUL, 0xafe124adUL,
			0xeed03fb4UL, 0x2d83129fUL, 0x6cb20986UL, 0xab2448c9UL, 0xea1553d0UL,
			0x29467efbUL, 0x687765e2UL, 0xf6793f2fUL, 0xb7482436UL, 0x741b091dUL,
			0x352a1204UL, 0xf2bc534bUL, 0xb38d4852UL, 0x70de6579UL, 0x31ef7e60UL,
			0xfef3e6e7UL, 0xbfc2fdfeUL, 0x7c91d0d5UL, 0x3da0cbccUL, 0xfa368a83UL,
			0xbb07919aUL, 0x7854bcb1UL, 0x3965a7a8UL, 0x4b98833bUL, 0x0aa99822UL,
			0xc9fab509UL, 0x88cbae10UL, 0x4f5def5fUL, 0x0e6cf446UL, 0xcd3fd96dUL,
			0x8c0ec274UL, 0x43125af3UL, 0x022341eaUL, 0xc1706cc1UL, 0x804177d8UL,
			0x47d73697UL, 0x06e62d8eUL, 0xc5b500a5UL, 0x84841bbcUL, 0x1a8a4171UL,
			0x5bbb5a68UL, 0x98e87743UL, 0xd9d96c5aUL, 0x1e4f2d15UL, 0x5f7e360cUL,
			0x9c2d1b27UL, 0xdd1c003eUL, 0x120098b9UL, 0x533183a0UL, 0x9062ae8bUL,
			0xd153b592UL, 0x16c5f4ddUL, 0x57f4efc4UL, 0x94a7c2efUL, 0xd596d9f6UL,
			0xe9bc07aeUL, 0xa88d1cb7UL, 0x6bde319cUL, 0x2aef2a85UL, 0xed796bcaUL,
			0xac4870d3UL, 0x6f1b5df8UL, 0x2e2a46e1UL, 0xe136de66UL, 0xa007c57fUL,
			0x6354e854UL, 0x2265f34dUL, 0xe5f3b202UL, 0xa4c2a91bUL, 0x67918430UL,
			0x26a09f29UL, 0xb8aec5e4UL, 0xf99fdefdUL, 0x3accf3d6UL, 0x7bfde8cfUL,
			0xbc6ba980UL, 0xfd5ab299UL, 0x3e099fb2UL, 0x7f3884abUL, 0xb0241c2cUL,
			0xf1150735UL, 0x32462a1eUL, 0x73773107UL, 0xb4e17048UL, 0xf5d06b51UL,
			0x3683467aUL, 0x77b25d63UL, 0x4ed7facbUL, 0x0fe6e1d2UL, 0xccb5ccf9UL,
			0x8d84d7e0UL, 0x4a1296afUL, 0x0b238db6UL, 0xc870a09dUL, 0x8941bb84UL,
			0x465d2303UL, 0x076c381aUL, 0xc43f1531UL, 0x850e0e28UL, 0x42984f67UL,
			0x03a9547eUL, 0xc0fa7955UL, 0x81cb624cUL, 0x1fc53881UL, 0x5ef42398UL,
			0x9da70eb3UL, 0xdc9615aaUL, 0x1b0054e5UL, 0x5a314ffcUL, 0x996262d7UL,
			0xd85379ceUL, 0x174fe149UL, 0x567efa50UL, 0x952dd77bUL, 0xd41ccc62UL,
			0x138a8d2dUL, 0x52bb9634UL, 0x91e8bb1fUL, 0xd0d9a006UL, 0xecf37e5eUL,
			0xadc26547UL, 0x6e91486cUL, 0x2fa05375UL, 0xe836123aUL, 0xa9070923UL,
			0x6a542408UL, 0x2b653f11UL, 0xe479a796UL, 0xa548bc8fUL, 0x661b91a4UL,
			0x272a8abdUL, 0xe0bccbf2UL, 0xa18dd0ebUL, 0x62defdc0UL, 0x23efe6d9UL,
			0xbde1bc14UL, 0xfcd0a70dUL, 0x3f838a26UL, 0x7eb2913fUL, 0xb924d070UL,
			0xf815cb69UL, 0x3b46e642UL, 0x7a77fd5bUL, 0xb56b65dcUL, 0xf45a7ec5UL,
			0x370953eeUL, 0x763848f7UL, 0xb1ae09b8UL, 0xf09f12a1UL, 0x33cc3f8aUL,
			0x72fd2493UL
		},
		{
			0x00000000UL, 0x376ac201UL, 0x6ed48403UL, 0x59be4602UL, 0xdca80907UL,
			0xebc2cb06UL, 0xb27c8d04UL, 0x85164f05UL, 0xb851130eUL, 0x8f3bd10fUL,
			0xd685970dUL, 0xe1ef550cUL, 0x64f91a09UL, 0x5393d808UL, 0x0a2d9e0aUL,
			0x3d475c0bUL, 0x70a3261cUL, 0x47c9e41dUL, 0x1e77a21fUL, 0x291d601eUL,
			0xac0b2f1bUL, 0x9b61ed1aUL, 0xc2dfab18UL, 0xf5b56919UL, 0xc8f23512UL,
			0xff98f713UL, 0xa626b111UL, 0x914c7310UL, 0x145a3c15UL, 0x2330fe14UL,
			0x7a8eb816UL, 0x4de47a17UL, 0xe0464d38UL, 0xd72c8f39UL, 0x8e92c93bUL,
			0xb9f80b3aUL, 0x3cee443fUL, 0x0b84863eUL, 0x523ac03cUL, 0x6550023dUL,
			0x58175e36UL, 0x6f7d9c37UL, 0x36c3da35UL, 0x01a91834UL, 0x84bf5731UL,
			0xb3d59530UL, 0xea6bd332UL, 0xdd011133UL, 0x90e56b24UL, 0xa78fa925UL,
			0xfe31ef27UL, 0xc95b2d26UL, 0x4c4d6223UL, 0x7b27a022UL, 0x2299e620UL,
			0x15f32421UL, 0x28b4782aUL, 0x1fdeba2bUL, 0x4660fc29UL, 0x710a3e28UL,
			0xf41c712dUL, 0xc376b32cUL, 0x9ac8f52eUL, 0xada2372fUL, 0xc08d9a70UL,
			0xf7e75871UL, 0xae591e73UL, 0x9933dc72UL, 0x1c259377UL, 0x2b4f5176UL,
			0x72f11774UL, 0x459bd575UL, 0x78dc897eUL, 0x4fb64b7fUL, 0x16080d7dUL,
			0x2162cf7cUL, 0xa4748079UL, 0x931e4278UL, 0xcaa0047aUL, 0xfdcac67bUL,
			0xb02ebc6cUL, 0x87447e6dUL, 0xdefa386fUL, 0xe990fa6eUL, 0x6c86b56bUL,
			0x5bec776aUL, 0x02523168UL, 0x3538f369UL, 0x087faf62UL, 0x3f156d63UL,
			0x66ab2b61UL, 0x51c1e960UL, 0xd4d7a665UL, 0xe3bd6464UL, 0xba032266UL,
			0x8d69e067UL, 0x20cbd748UL, 0x17a11549UL, 0x4e1f534bUL, 0x7975914aUL,
			0xfc63de4fUL, 0xcb091c4eUL, 0x92b75a4cUL, 0xa5dd984dUL, 0x989ac446UL,
			0xaff00647UL, 0xf64e4045UL, 0xc1248244UL, 0x4432cd41UL, 0x73580f40UL,
			0x2ae64942UL, 0x1d8c8b43UL, 0x5068f154UL, 0x67023355UL, 0x3ebc7557UL,
			0x09d6b756UL, 0x8cc0f853UL, 0xbbaa3a52UL, 0xe2147c50UL, 0xd57ebe51UL,
			0xe839e25aUL, 0xdf53205bUL, 0x86ed6659UL, 0xb187a458UL, 0x3491eb5dUL,
			0x03fb295cUL, 0x5a456f5eUL, 0x6d2fad5fUL, 0x801b35e1UL, 0xb771f7e0UL,
			0xeecfb1e2UL, 0xd9a573e3UL, 0x5cb33ce6UL, 0x6bd9fee7UL, 0x3267b8e5UL,
			0x050d7ae4UL, 0x384a26efUL, 0x0f20e4eeUL, 0x569ea2ecUL, 0x61f460edUL,
			0xe4e22fe8UL, 0xd388ede9UL, 0x8a36abebUL, 0xbd5c69eaUL, 0xf0b813fdUL,
			0xc7d2d1fcUL, 0x9e6c97feUL, 0xa90655ffUL, 0x2c101afaUL, 0x1b7ad8fbUL,
			0x42c49ef9UL, 0x75ae5cf8UL, 0x48e900f3UL, 0x7f83c2f2UL, 0x263d84f0UL,
			0x115746f1UL, 0x944109f4UL, 0xa32bcbf5UL, 0xfa958df7UL, 0xcdff4ff6UL,
			0x605d78d9UL, 0x5737bad8UL, 0x0e89fcdaUL, 0x39e33edbUL, 0xbcf571deUL,
			0x8b9fb3dfUL, 0xd221f5ddUL, 0xe54b37dcUL, 0xd80c6bd7UL, 0xef66a9d6UL,
			0xb6d8efd4UL, 0x81b22dd5UL, 0x04a462d0UL, 0x33cea0d1UL, 0x6a70e6d3UL,
			0x5d1a24d2UL, 0x10fe5ec5UL, 0x27949cc4UL, 0x7e2adac6UL, 0x494018c7UL,
			0xcc5657c2UL, 0xfb3c95c3UL, 0xa282d3c1UL, 0x95e811c0UL, 0xa8af4dcbUL,
			0x9fc58fcaUL, 0xc67bc9c8UL, 0xf1110bc9UL, 0x740744ccUL, 0x436d86cdUL,
			0x1ad3c0cfUL, 0x2db902ceUL, 0x4096af91UL, 0x77fc6d90UL, 0x2e422b92UL,
			0x1928e993UL, 0x9c3ea696UL, 0xab546497UL, 0xf2ea2295UL, 0xc580e094UL,
			0xf8c7bc9fUL, 0xcfad7e9eUL, 0x9613389cUL, 0xa179fa9dUL, 0x246fb598UL,
			0x13057799UL, 0x4abb319bUL, 0x7dd1f39aUL, 0x3035898dUL, 0x075f4b8cUL,
			0x5ee10d8eUL, 0x698bcf8fUL, 0xec9d808aUL, 0xdbf7428bUL, 0x82490489UL,
			0xb523c688UL, 0x88649a83UL, 0xbf0e5882UL, 0xe6b01e80UL, 0xd1dadc81UL,
			0x54cc9384UL, 0x63a65185UL, 0x3a181787UL, 0x0d72d586UL, 0xa0d0e2a9UL,
			0x97ba20a8UL, 0xce0466aaUL, 0xf96ea4abUL, 0x7c78ebaeUL, 0x4b1229afUL,
			0x12ac6fadUL, 0x25c6adacUL, 0x1881f1a7UL, 0x2feb33a6UL, 0x765575a4UL,
			0x413fb7a5UL, 0xc429f8a0UL, 0xf3433aa1UL, 0xaafd7ca3UL, 0x9d97bea2UL,
			0xd073c4b5UL, 0xe71906b4UL, 0xbea740b6UL, 0x89cd82b7UL, 0x0cdbcdb2UL,
			0x3bb10fb3UL, 0x620f49b1UL, 0x55658bb0UL, 0x6822d7bbUL, 0x5f4815baUL,
			0x06f653b8UL, 0x319c91b9UL, 0xb48adebcUL, 0x83e01cbdUL, 0xda5e5abfUL,
			0xed3498beUL
		},
		{
			0x00000000UL, 0x6567bcb8UL, 0x8bc809aaUL, 0xeeafb512UL, 0x5797628fUL,
			0x32f0de37UL, 0xdc5f6b25UL, 0xb938d79dUL, 0xef28b4c5UL, 0x8a4f087dUL,
			0x64e0bd6fUL, 0x018701d7UL, 0xb8bfd64aUL, 0xddd86af2UL, 0x3377dfe0UL,
			0x56106358UL, 0x9f571950UL, 0xfa30a5e8UL, 0x149f10faUL, 0x71f8ac42UL,
			0xc8c07bdfUL, 0xada7c767UL, 0x43087275UL, 0x266fcecdUL, 0x707fad95UL,
			0x1518112dUL, 0xfbb7a43fUL, 0x9ed01887UL, 0x27e8cf1aUL, 0x428f73a2UL,
			0xac20c6b0UL, 0xc9477a08UL, 0x3eaf32a0UL, 0x5bc88e18UL, 0xb5673b0aUL,
			0xd00087b2UL, 0x6938502fUL, 0x0c5fec97UL, 0xe2f05985UL, 0x8797e53dUL,
			0xd1878665UL, 0xb4e03addUL, 0x5a4f8fcfUL, 0x3f283377UL, 0x8610e4eaUL,
			0xe3775852UL, 0x0dd8ed40UL, 0x68bf51f8UL, 0xa1f82bf0UL, 0xc49f9748UL,
			0x2a30225aUL, 0x4f579ee2UL, 0xf66f497fUL, 0x9308f5c7UL, 0x7da740d5UL,
			0x18c0fc6dUL, 0x4ed09f35UL, 0x2bb7238dUL, 0xc518969fUL, 0xa07f2a27UL,
			0x1947fdbaUL, 0x7c204102UL, 0x928ff410UL, 0xf7e848a8UL, 0x3d58149bUL,
			0x583fa823UL, 0xb6901d31UL, 0xd3f7a189UL, 0x6acf7614UL, 0x0fa8caacUL,
			0xe1077fbeUL, 0x8460c306UL, 0xd270a05eUL, 0xb7171ce6UL, 0x59b8a9f4UL,
			0x3cdf154cUL, 0x85e7c2d1UL, 0xe0807e69UL, 0x0e2fcb7bUL, 0x6b4877c3UL,
			0xa20f0dcbUL, 0xc768b173UL, 0x29c70461UL, 0x4ca0b8d9UL, 0xf5986f44UL,
			0x90ffd3fcUL, 0x7e5066eeUL, 0x1b37da56UL, 0x4d27b90eUL, 0x284005b6UL,
			0xc6efb0a4UL, 0xa3880c1cUL, 0x1ab0db81UL, 0x7fd76739UL, 0x9178d22bUL,
			0xf41f6e93UL, 0x03f7263bUL, 0x66909a83UL, 0x883f2f91UL, 0xed589329UL,
			0x546044b4UL, 0x3107f80cUL, 0xdfa84d1eUL, 0xbacff1a6UL, 0xecdf92feUL,
			0x89b82e46UL, 0x67179b54UL, 0x027027ecUL, 0xbb48f071UL, 0xde2f4cc9UL,
			0x3080f9dbUL, 0x55e74563UL, 0x9ca03f6bUL, 0xf9c783d3UL, 0x176836c1UL,
			0x720f8a79UL, 0xcb375de4UL, 0xae50e15cUL, 0x40ff544eUL, 0x2598e8f6UL,
			0x73888baeUL, 0x16ef3716UL, 0xf8408204UL, 0x9d273ebcUL, 0x241fe921UL,
			0x41785599UL, 0xafd7e08bUL, 0xcab05c33UL, 0x3bb659edUL, 0x5ed1e555UL,
			0xb07e5047UL, 0xd519ecffUL, 0x6c213b62UL, 0x094687daUL, 0xe7e932c8UL,
			0x828e8e70UL, 0xd49eed28UL, 0xb1f95190UL, 0x5f56e482UL, 0x3a31583aUL,
			0x83098fa7UL, 0xe66e331fUL, 0x08c1860dUL, 0x6da63ab5UL, 0xa4e140bdUL,
			0xc186fc05UL, 0x2f294917UL, 0x4a4ef5afUL, 0xf3762232UL, 0x96119e8aUL,
			0x78be2b98UL, 0x1dd99720UL, 0x4bc9f478UL, 0x2eae48c0UL, 0xc001fdd2UL,
			0xa566416aUL, 0x1c5e96f7UL, 0x79392a4fUL, 0x97969f5dUL, 0xf2f123e5UL,
			0x05196b4dUL, 0x607ed7f5UL, 0x8ed162e7UL, 0xebb6de5fUL, 0x528e09c2UL,
			0x37e9b57aUL, 0xd9460068UL, 0xbc21bcd0UL, 0xea31df88UL, 0x8f566330UL,
			0x61f9d622UL, 0x049e6a9aUL, 0xbda6bd07UL, 0xd8c101bfUL, 0x366eb4adUL,
			0x53090815UL, 0x9a4e721dUL, 0xff29cea5UL, 0x11867bb7UL, 0x74e1c70fUL,
			0xcdd91092UL, 0xa8beac2aUL, 0x46111938UL, 0x2376a580UL, 0x7566c6d8UL,
			0x10017a60UL, 0xfeaecf72UL, 0x9bc973caUL, 0x22f1a457UL, 0x479618efUL,
			0xa939adfdUL, 0xcc5e1145UL, 0x06ee4d76UL, 0x6389f1ceUL, 0x8d2644dcUL,
			0xe841f864UL, 0x51792ff9UL, 0x341e9341UL, 0xdab12653UL, 0xbfd69aebUL,
			0xe9c6f9b3UL, 0x8ca1450bUL, 0x620ef019UL, 0x07694ca1UL, 0xbe519b3cUL,
			0xdb362784UL, 0x35999296UL, 0x50fe2e2eUL, 0x99b95426UL, 0xfcdee89eUL,
			0x12715d8cUL, 0x7716e134UL, 0xce2e36a9UL, 0xab498a11UL, 0x45e63f03UL,
			0x208183bbUL, 0x7691e0e3UL, 0x13f65c5bUL, 0xfd59e949UL, 0x983e55f1UL,
			0x2106826cUL, 0x44613ed4UL, 0xaace8bc6UL, 0xcfa9377eUL, 0x38417fd6UL,
			0x5d26c36eUL, 0xb389767cUL, 0xd6eecac4UL, 0x6fd61d59UL, 0x0ab1a1e1UL,
			0xe41e14f3UL, 0x8179a84bUL, 0xd769cb13UL, 0xb20e77abUL, 0x5ca1c2b9UL,
			0x39c67e01UL, 0x80fea99cUL, 0xe5991524UL, 0x0b36a036UL, 0x6e511c8eUL,
			0xa7166686UL, 0xc271da3eUL, 0x2cde6f2cUL, 0x49b9d394UL, 0xf0810409UL,
			0x95e6b8b1UL, 0x7b490da3UL, 0x1e2eb11bUL, 0x483ed243UL, 0x2d596efbUL,
			0xc3f6dbe9UL, 0xa6916751UL, 0x1fa9b0ccUL, 0x7ace0c74UL, 0x9461b966UL,
			0xf10605deUL
	#endif
		}
	};
#endif /* DYNAMIC_CRC_TABLE */
//
// This function can be used by asm versions of crc32()
//
const z_crc_t  * ZEXPORT get_crc_table()
{
#ifdef DYNAMIC_CRC_TABLE
	if(crc_table_empty)
		make_crc_table();
#endif /* DYNAMIC_CRC_TABLE */
	return (const z_crc_t *)crc_table;
}

#define DO1 crc = crc_table[0][((int)crc ^ (*buf++)) & 0xff] ^ (crc >> 8)
#define DO8 DO1; DO1; DO1; DO1; DO1; DO1; DO1; DO1

ulong ZEXPORT crc32_z(ulong crc, const uchar  * buf, size_t len)
{
	if(buf == Z_NULL) 
		return 0UL;
#ifdef DYNAMIC_CRC_TABLE
	if(crc_table_empty)
		make_crc_table();
#endif
#ifdef BYFOUR
	if(sizeof(void *) == sizeof(ptrdiff_t)) {
		z_crc_t endian = 1;
		if(*((uchar *)(&endian)))
			return crc32_little(crc, buf, len);
		else
			return crc32_big(crc, buf, len);
	}
#endif
	crc = crc ^ 0xffffffffUL;
	while(len >= 8) {
		DO8;
		len -= 8;
	}
	if(len) do {
		DO1;
	} while(--len);
	return crc ^ 0xffffffffUL;
}

ulong ZEXPORT crc32(ulong crc, const uchar  * buf, uInt len)
{
	return crc32_z(crc, buf, len);
}

#ifdef BYFOUR
// 
// This BYFOUR code accesses the passed uchar * buffer with a 32-bit
// integer pointer type. This violates the strict aliasing rule, where a
// compiler can assume, for optimization purposes, that two pointers to
// fundamentally different types won't ever point to the same memory. This can
// manifest as a problem only if one of the pointers is written to. This code
// only reads from those pointers. So long as this code remains isolated in
// this compilation unit, there won't be a problem. For this reason, this code
// should not be copied and pasted into a compilation unit in which other code
// writes to the buffer that is passed to these routines.
// 
#define DOLIT4 c ^= *buf4++; c = crc_table[3][c & 0xff] ^ crc_table[2][(c >> 8) & 0xff] ^ crc_table[1][(c >> 16) & 0xff] ^ crc_table[0][c >> 24]
#define DOLIT32 DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4; DOLIT4

static ulong crc32_little(ulong crc, const uchar * buf, size_t len)
{
	const z_crc_t * buf4;
	z_crc_t c = (z_crc_t)crc;
	c = ~c;
	while(len && ((ptrdiff_t)buf & 3)) {
		c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
		len--;
	}
	buf4 = reinterpret_cast<const z_crc_t *>(buf);
	while(len >= 32) {
		DOLIT32;
		len -= 32;
	}
	while(len >= 4) {
		DOLIT4;
		len -= 4;
	}
	buf = reinterpret_cast<const uchar *>(buf4);
	if(len) do {
		c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
	} while(--len);
	c = ~c;
	return (ulong)c;
}

#define DOBIG4 c ^= *buf4++; c = crc_table[4][c & 0xff] ^ crc_table[5][(c >> 8) & 0xff] ^ crc_table[6][(c >> 16) & 0xff] ^ crc_table[7][c >> 24]
#define DOBIG32 DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4; DOBIG4

static ulong crc32_big(ulong crc, const uchar * buf, size_t len)
{
	const z_crc_t * buf4;
	z_crc_t c = ZSWAP32((z_crc_t)crc);
	c = ~c;
	while(len && ((ptrdiff_t)buf & 3)) {
		c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
		len--;
	}
	buf4 = reinterpret_cast<const z_crc_t *>(buf);
	while(len >= 32) {
		DOBIG32;
		len -= 32;
	}
	while(len >= 4) {
		DOBIG4;
		len -= 4;
	}
	buf = reinterpret_cast<const uchar *>(buf4);
	if(len) do {
		c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
	} while(--len);
	c = ~c;
	return (ulong)(ZSWAP32(c));
}

#endif /* BYFOUR */

#define GF2_DIM 32      /* dimension of GF(2) vectors (length of CRC) */

static ulong FASTCALL gf2_matrix_times(ulong * mat, ulong vec)
{
	ulong sum = 0;
	while(vec) {
		if(vec & 1)
			sum ^= *mat;
		vec >>= 1;
		mat++;
	}
	return sum;
}

static void FASTCALL gf2_matrix_square(ulong * square, ulong * mat)
{
	for(int n = 0; n < GF2_DIM; n++)
		square[n] = gf2_matrix_times(mat, mat[n]);
}

static uLong crc32_combine_(uLong crc1, uLong crc2, z_off64_t len2)
{
	int n;
	ulong  row;
	ulong  even[GF2_DIM]; /* even-power-of-two zeros operator */
	ulong  odd[GF2_DIM]; /* odd-power-of-two zeros operator */
	// degenerate case (also disallow negative lengths) 
	if(len2 <= 0)
		return crc1;
	// put operator for one zero bit in odd 
	odd[0] = 0xedb88320UL;      /* CRC-32 polynomial */
	row = 1;
	for(n = 1; n < GF2_DIM; n++) {
		odd[n] = row;
		row <<= 1;
	}
	// put operator for two zero bits in even 
	gf2_matrix_square(even, odd);
	// put operator for four zero bits in odd 
	gf2_matrix_square(odd, even);
	// apply len2 zeros to crc1 (first square will put the operator for one
	// zero byte, eight zero bits, in even) 
	do {
		/* apply zeros operator for this bit of len2 */
		gf2_matrix_square(even, odd);
		if(len2 & 1)
			crc1 = gf2_matrix_times(even, crc1);
		len2 >>= 1;
		/* if no more bits set, then done */
		if(len2 == 0)
			break;
		/* another iteration of the loop with odd and even swapped */
		gf2_matrix_square(odd, even);
		if(len2 & 1)
			crc1 = gf2_matrix_times(odd, crc1);
		len2 >>= 1;
		/* if no more bits set, then done */
	} while(len2 != 0);
	/* return combined crc */
	crc1 ^= crc2;
	return crc1;
}

uLong ZEXPORT crc32_combine(uLong crc1, uLong crc2, z_off_t len2)
	{ return crc32_combine_(crc1, crc2, len2); }
uLong ZEXPORT crc32_combine64(uLong crc1, uLong crc2, z_off64_t len2)
	{ return crc32_combine_(crc1, crc2, len2); }
//
// ZUTIL
//
const char * const z_errmsg[10] = {
	"need dictionary",  /* Z_NEED_DICT       2  */
	"stream end",       /* Z_STREAM_END      1  */
	"",                 /* Z_OK              0  */
	"file error",       /* Z_ERRNO         (-1) */
	"stream error",     /* Z_STREAM_ERROR  (-2) */
	"data error",       /* Z_DATA_ERROR    (-3) */
	"insufficient memory", /* Z_MEM_ERROR     (-4) */
	"buffer error",     /* Z_BUF_ERROR     (-5) */
	"incompatible version", /* Z_VERSION_ERROR (-6) */
	""
};

const char * ZEXPORT zlibVersion()
{
	return ZLIB_VERSION;
}

uLong ZEXPORT zlibCompileFlags()
{
	uLong flags = 0;
	switch((int)(sizeof(uInt))) {
		case 2:     break;
		case 4:     flags += 1;     break;
		case 8:     flags += 2;     break;
		default:    flags += 3;
	}
	switch((int)(sizeof(uLong))) {
		case 2:     break;
		case 4:     flags += 1 << 2;        break;
		case 8:     flags += 2 << 2;        break;
		default:    flags += 3 << 2;
	}
	switch((int)(sizeof(void *))) {
		case 2:     break;
		case 4:     flags += 1 << 4;        break;
		case 8:     flags += 2 << 4;        break;
		default:    flags += 3 << 4;
	}
	switch((int)(sizeof(z_off_t))) {
		case 2:     break;
		case 4:     flags += 1 << 6;        break;
		case 8:     flags += 2 << 6;        break;
		default:    flags += 3 << 6;
	}
#ifdef ZLIB_DEBUG
	flags += 1 << 8;
#endif
#if defined(ASMV) || defined(ASMINF)
	flags += 1 << 9;
#endif
#ifdef ZLIB_WINAPI
	flags += 1 << 10;
#endif
#ifdef BUILDFIXED
	flags += 1 << 12;
#endif
#ifdef DYNAMIC_CRC_TABLE
	flags += 1 << 13;
#endif
#ifdef NO_GZCOMPRESS
	flags += 1L << 16;
#endif
#ifdef NO_GZIP
	flags += 1L << 17;
#endif
#ifdef PKZIP_BUG_WORKAROUND
	flags += 1L << 20;
#endif
#ifdef FASTEST
	flags += 1L << 21;
#endif
#if defined(STDC) || defined(Z_HAVE_STDARG_H)
#  ifdef NO_vsnprintf
	flags += 1L << 25;
#    ifdef HAS_vsprintf_void
	flags += 1L << 26;
#    endif
#  else
#    ifdef HAS_vsnprintf_void
	flags += 1L << 26;
#    endif
#  endif
#else
	flags += 1L << 24;
#  ifdef NO_snprintf
	flags += 1L << 25;
#    ifdef HAS_sprintf_void
	flags += 1L << 26;
#    endif
#  else
#    ifdef HAS_snprintf_void
	flags += 1L << 26;
#    endif
#  endif
#endif
	return flags;
}

#ifdef ZLIB_DEBUG
	#include <stdlib.h>
	#ifndef verbose
		#define verbose 0
	#endif
	int ZLIB_INTERNAL z_verbose = verbose;
	void ZLIB_INTERNAL z_error(char * m)
	{
		fprintf(stderr, "%s\n", m);
		exit(1);
	}
#endif
//
// exported to allow conversion of error code to string for compress() and uncompress()
//
const char * ZEXPORT zError(int err)
{
	return ERR_MSG(err);
}

#if defined(_WIN32_WCE)
	// The Microsoft C Run-Time Library for Windows CE doesn't have
	// errno.  We define it as a global variable to simplify porting.
	// Its value is always 0 and should not be used.
	int errno = 0;
#endif
#ifndef HAVE_MEMCPY
	/*void ZLIB_INTERNAL zmemcpy_Removed(Bytef* dest, const Bytef* source, uInt len)
	{
		if(len) {
			do {
				*dest++ = *source++; // ??? to be unrolled 
			} while(--len != 0);
		}
	}*/
	int ZLIB_INTERNAL zmemcmp(const Bytef* s1, const Bytef* s2, uInt len)
	{
		uInt j;
		for(j = 0; j < len; j++) {
			if(s1[j] != s2[j]) return 2*(s1[j] > s2[j])-1;
		}
		return 0;
	}
	/* void ZLIB_INTERNAL zmemzero_Removed(Bytef* dest, uInt len)
	{
		if(len) {
			do {
				*dest++ = 0; // ??? to be unrolled 
			} while(--len != 0);
		}
	}*/
#endif
#ifndef Z_SOLO
	#ifdef SYS16BIT // {
		#ifdef __TURBOC__ // Turbo C in 16-bit mode 
			#define MY_ZCALLOC
			// Turbo C malloc() does not allow dynamic allocation of 64K bytes
			// and farmalloc(64K) returns a pointer with an offset of 8, so we
			// must fix the pointer. Warning: the pointer must be put back to its
			// original form in order to free it, use zcfree().
			// 
			#define MAX_PTR 10 // 10*64K = 640K 

			static int next_ptr = 0;

			typedef struct ptr_table_s {
				voidpf org_ptr;
				voidpf new_ptr;
			} ptr_table;

			static ptr_table table[MAX_PTR];
			/* This table is used to remember the original form of pointers
			* to large buffers (64K). Such pointers are normalized with a zero offset.
			* Since MSDOS is not a preemptive multitasking OS, this table is not
			* protected from concurrent access. This hack doesn't work anyway on
			* a protected system like OS/2. Use Microsoft C instead.
			*/
			voidpf ZLIB_INTERNAL zcalloc(void * opaque, uint items, uint size)
			{
				voidpf buf;
				ulong bsize = (ulong)items*size;
				(void)opaque;
				/* If we allocate less than 65520 bytes, we assume that farmalloc
				* will return a usable pointer which doesn't have to be normalized.
				*/
				if(bsize < 65520L) {
					buf = farmalloc(bsize);
					if(*(ushort *)&buf != 0) return buf;
				}
				else {
					buf = farmalloc(bsize + 16L);
				}
				if(buf == NULL || next_ptr >= MAX_PTR) 
					return NULL;
				table[next_ptr].org_ptr = buf;
				/* Normalize the pointer to seg:0 */
				*((ushort *)&buf+1) += ((ushort)((uchar *)buf-0) + 15) >> 4;
				*(ushort *)&buf = 0;
				table[next_ptr++].new_ptr = buf;
				return buf;
			}
			void ZLIB_INTERNAL zcfree(voidpf opaque, voidpf ptr)
			{
				int n;
				(void)opaque;
				if(*(ushort *)&ptr != 0) { /* object < 64K */
					farfree(ptr);
					return;
				}
				// Find the original pointer 
				for(n = 0; n < next_ptr; n++) {
					if(ptr != table[n].new_ptr) 
						continue;
					farfree(table[n].org_ptr);
					while(++n < next_ptr) {
						table[n-1] = table[n];
					}
					next_ptr--;
					return;
				}
				Assert(0, "zcfree: ptr not found");
			}
		#endif /* __TURBOC__ */
		#ifdef M_I86
			// Microsoft C in 16-bit mode 
			#define MY_ZCALLOC
			#if (!defined(_MSC_VER) || (_MSC_VER <= 600))
				#define _halloc  halloc
				#define _hfree   hfree
			#endif

			voidpf ZLIB_INTERNAL zcalloc(void * opaque, uInt items, uInt size)
			{
				(void)opaque;
				return _halloc((long)items, size);
			}
			void ZLIB_INTERNAL zcfree(voidpf opaque, voidpf ptr)
			{
				(void)opaque;
				_hfree(ptr);
			}
		#endif /* M_I86 */
	#endif // } SYS16BIT 
	#ifndef MY_ZCALLOC // Any system without a special alloc function 
		#ifndef STDC
			extern void * malloc(uInt size);
			extern void * calloc(uInt items, uInt size);
			extern void free(void * ptr);
		#endif
		void * ZLIB_INTERNAL zcalloc(void * opaque, uint items, uint size)
		{
			(void)opaque;
			return sizeof(uInt) > 2 ? SAlloc::M(items * size) : SAlloc::C(items, size);
		}
		void ZLIB_INTERNAL zcfree(void * opaque, void * ptr)
		{
			(void)opaque;
			SAlloc::F(ptr);
		}
	#endif
#endif /* !Z_SOLO */
//
// UNCOMPR
//
// Decompresses the source buffer into the destination buffer.  *sourceLen is the byte length of 
// the source buffer. Upon entry, *destLen is the total size of the destination buffer, which 
// must be large enough to hold the entire uncompressed data. (The size of the uncompressed data 
// must have been saved previously by the compressor and transmitted to the decompressor by some
// mechanism outside the scope of this compression library.) Upon exit, *destLen is the size of 
// the decompressed data and *sourceLen is the number of source bytes consumed. Upon return, 
// source + *sourceLen points to the first unused input byte.
//
// uncompress returns Z_OK if success, Z_MEM_ERROR if there was not enough memory, Z_BUF_ERROR 
// if there was not enough room in the output buffer, or Z_DATA_ERROR if the input data was corrupted, 
// including if the input data is an incomplete zlib stream.
//
int ZEXPORT uncompress2(Bytef * dest, uLongf * destLen, const Bytef * source, uLong * sourceLen)
{
	z_stream stream;
	int err;
	const uInt max = (uInt)-1;
	uLong len, left;
	Byte buf[1]; /* for detection of incomplete stream when *destLen == 0 */
	len = *sourceLen;
	if(*destLen) {
		left = *destLen;
		*destLen = 0;
	}
	else {
		left = 1;
		dest = buf;
	}
	stream.next_in = source;
	stream.avail_in = 0;
	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;
	stream.opaque = (void *)0;
	err = inflateInit(&stream);
	if(err != Z_OK) 
		return err;
	stream.next_out = dest;
	stream.avail_out = 0;
	do {
		if(stream.avail_out == 0) {
			stream.avail_out = left > (uLong)max ? max : (uInt)left;
			left -= stream.avail_out;
		}
		if(stream.avail_in == 0) {
			stream.avail_in = len > (uLong)max ? max : (uInt)len;
			len -= stream.avail_in;
		}
		err = inflate(&stream, Z_NO_FLUSH);
	} while(err == Z_OK);
	*sourceLen -= len + stream.avail_in;
	if(dest != buf)
		*destLen = stream.total_out;
	else if(stream.total_out && err == Z_BUF_ERROR)
		left = 1;
	inflateEnd(&stream);
	return (err == Z_STREAM_END) ? Z_OK : (err == Z_NEED_DICT) ? Z_DATA_ERROR : (err == Z_BUF_ERROR && (left + stream.avail_out)) ? Z_DATA_ERROR : err;
}

int ZEXPORT uncompress(Bytef * dest, uLongf * destLen, const Bytef * source, uLong sourceLen)
{
	return uncompress2(dest, destLen, source, &sourceLen);
}
//
// INFTREES
//
#define MAXBITS 15

const char inflate_copyright[] = " inflate 1.2.11 Copyright 1995-2017 Mark Adler ";
/*
   If you use the zlib library in a product, an acknowledgment is welcome
   in the documentation of your product. If for some reason you cannot
   include such an acknowledgment, I would appreciate that you keep this
   copyright string in the executable of your product.
 */
/*
   Build a set of tables to decode the provided canonical Huffman code.
   The code lengths are lens[0..codes-1].  The result starts at *table,
   whose indices are 0..2^bits-1.  work is a writable array of at least
   lens shorts, which is used as a work area.  type is the type of code
   to be generated, CODES, LENS, or DISTS.  On return, zero is success,
   -1 is an invalid code, and +1 means that ENOUGH isn't enough.  table
   on return points to the next available entry's address.  bits is the
   requested root table index bits, and on return it is the actual root
   table index bits.  It will differ if the request is greater than the
   longest code or if it is less than the shortest code.
 */
int ZLIB_INTERNAL inflate_table(codetype type, ushort  * lens, uint codes, ZInfTreesCode ** table, uint * bits, ushort  * work)
{
	uint   len;           // a code's length in bits 
	uint   sym;           // index of code symbols 
	uint   _min, _max;    // minimum and maximum code lengths 
	uint   root;          // number of index bits for root table 
	uint   curr;          // number of index bits for current table 
	uint   drop;          // code bits to drop for sub-table 
	int    left;          // number of prefix codes available 
	uint   used;          /* code entries in table used */
	uint   huff;          /* Huffman code */
	uint   incr;          /* for incrementing code, index */
	uint   fill;          /* index for replicating entries */
	uint   low;           /* low bits for current root entry */
	uint   mask;          /* mask for low root bits */
	ZInfTreesCode here;      // table entry for duplication 
	ZInfTreesCode * p_next;  // next available space in table 
	const  ushort * p_base;  // base value table to use 
	const  ushort * p_extra; // extra bits table to use 
	uint   match;            // use base and extra for symbol >= match 
	ushort count[MAXBITS+1]; // number of codes of each length 
	ushort offs[MAXBITS+1];  // offsets in table for each length 
	// Length codes 257..285 base 
	static const ushort lbase[31] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0 };
	// Length codes 257..285 extra 
	static const ushort lext[31] = { 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 16, 77, 202 };
	// Distance codes 0..29 base 
	static const ushort dbase[32] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0 };
	// Distance codes 0..29 extra 
	static const ushort dext[32] = { 16, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 64, 64 };
	// 
	// Process a set of code lengths to create a canonical Huffman code.  The
	// code lengths are lens[0..codes-1].  Each length corresponds to the
	// symbols 0..codes-1.  The Huffman code is generated by first sorting the
	// symbols by length from short to long, and retaining the symbol order
	// for codes with equal lengths.  Then the code starts with all zero bits
	// for the first code of the shortest length, and the codes are integer
	// increments for the same length, and zeros are appended as the length
	// increases.  For the deflate format, these bits are stored backwards
	// from their more natural integer increment ordering, and so when the
	// decoding tables are built in the large loop below, the integer codes are incremented backwards.
	// 
	// This routine assumes, but does not check, that all of the entries in
	// lens[] are in the range 0..MAXBITS.  The caller must assure this.
	// 1..MAXBITS is interpreted as that code length.  zero means that that
	// symbol does not occur in this code.
	// 
	// The codes are sorted by computing a count of codes for each length,
	// creating from that a table of starting indices for each length in the
	// sorted table, and then entering the symbols in order in the sorted
	// table.  The sorted table is work[], with that space being provided by the caller.
	// 
	// The length counts are used for other purposes as well, i.e. finding
	// the minimum and maximum length codes, determining if there are any
	// codes at all, checking for a valid set of lengths, and looking ahead
	// at length counts to determine sub-table sizes when building the decoding tables.
	// 
	// accumulate lengths for codes (assumes lens[] all in 0..MAXBITS) 
	for(len = 0; len <= MAXBITS; len++)
		count[len] = 0;
	for(sym = 0; sym < codes; sym++)
		count[lens[sym]]++;
	// bound code lengths, force root to be within code lengths 
	root = *bits;
	for(_max = MAXBITS; _max >= 1; _max--)
		if(count[_max] != 0) 
			break;
	SETMIN(root, _max);
	if(_max == 0) { // no symbols to code at all 
		here.op = (uchar)64; // invalid code marker
		here.bits = (uchar)1;
		here.val = (ushort)0;
		*(*table)++ = here; // make a table to force an error 
		*(*table)++ = here;
		*bits = 1;
		return 0; // no symbols, but wait for decoding to report error 
	}
	else {
		for(_min = 1; _min < _max; _min++)
			if(count[_min] != 0) 
				break;
		SETMAX(root, _min);
		// check for an over-subscribed or incomplete set of lengths 
		left = 1;
		for(len = 1; len <= MAXBITS; len++) {
			left <<= 1;
			left -= count[len];
			if(left < 0) 
				return -1; // over-subscribed 
		}
		if(left > 0 && (type == CODES || _max != 1))
			return -1; // incomplete set 
		// generate offsets into symbol table for each length for sorting 
		offs[1] = 0;
		for(len = 1; len < MAXBITS; len++)
			offs[len + 1] = offs[len] + count[len];
		// sort symbols by length, by symbol order within each length 
		for(sym = 0; sym < codes; sym++)
			if(lens[sym]) 
				work[offs[lens[sym]]++] = (ushort)sym;
		// 
		// Create and fill in decoding tables.  In this loop, the table being
		// filled is at next and has curr index bits.  The code being used is huff
		// with length len.  That code is converted to an index by dropping drop
		// bits off of the bottom.  For codes where len is less than drop + curr,
		// those top drop + curr - len bits are incremented through all values to
		// fill the table with replicated entries.
		//
		// root is the number of index bits for the root table.  When len exceeds
		// root, sub-tables are created pointed to by the root entry with an index
		// of the low root bits of huff.  This is saved in low to check for when a
		// new sub-table should be started.  drop is zero when the root table is
		// being filled, and drop is root when sub-tables are being filled.
		//
		// When a new sub-table is needed, it is necessary to look ahead in the
		// code lengths to determine what size sub-table is needed.  The length
		// counts are used for this, and so count[] is decremented as codes are
		// entered in the tables.
		//
		// used keeps track of how many table entries have been allocated from the
		// provided *table space.  It is checked for LENS and DIST tables against
		// the constants ENOUGH_LENS and ENOUGH_DISTS to guard against changes in
		// the initial root table size constants.  See the comments in inftrees.h
		// for more information.
		//
		// sym increments through all symbols, and the loop terminates when
		// all codes of length max, i.e. all codes, have been processed.  This
		// routine permits incomplete codes, so another loop after this one fills
		// in the rest of the decoding tables with invalid code markers.
		// 
		// set up for code type 
		switch(type) {
			case CODES:
				p_base = p_extra = work; /* dummy value--not used */
				match = 20;
				break;
			case LENS:
				p_base = lbase;
				p_extra = lext;
				match = 257;
				break;
			default: /* DISTS */
				p_base = dbase;
				p_extra = dext;
				match = 0;
		}
		// initialize state for loop 
		huff = 0;        // starting code 
		sym  = 0;        // starting code symbol 
		len  = _min;     // starting code length 
		p_next = *table; // current table to fill in 
		curr = root;     // current table index bits 
		drop = 0;        // current bits to drop from code for index 
		low = (uint)(-1); // trigger new sub-table when len > root 
		used = 1U << root;    // use root table entries 
		mask = used - 1;      // mask for comparing low 
		// check available table space 
		if((type == LENS && used > ENOUGH_LENS) || (type == DISTS && used > ENOUGH_DISTS))
			return 1;
		else {
			// process all codes and make table entries 
			for(;; ) {
				// create table entry 
				here.bits = (uchar)(len - drop);
				if(work[sym] + 1U < match) {
					here.op = (uchar)0;
					here.val = work[sym];
				}
				else if(work[sym] >= match) {
					here.op = (uchar)(p_extra[work[sym] - match]);
					here.val = p_base[work[sym] - match];
				}
				else {
					here.op = (uchar)(32 + 64); /* end of block */
					here.val = 0;
				}
				// replicate for those indices with low len bits equal to huff 
				incr = 1U << (len - drop);
				fill = 1U << curr;
				_min = fill; // save offset to next table 
				do {
					fill -= incr;
					p_next[(huff >> drop) + fill] = here;
				} while(fill != 0);
				// backwards increment the len-bit code huff 
				incr = 1U << (len - 1);
				while(huff & incr)
					incr >>= 1;
				if(incr != 0) {
					huff &= incr - 1;
					huff += incr;
				}
				else
					huff = 0;
				// go to next symbol, update count, len 
				sym++;
				if(--(count[len]) == 0) {
					if(len == _max) 
						break;
					len = lens[work[sym]];
				}
				// create new sub-table if needed 
				if(len > root && (huff & mask) != low) {
					// if first time, transition to sub-tables 
					SETIFZ(drop, root);
					// increment past last table 
					p_next += _min; // here min is 1 << curr 
					// determine length of next table 
					curr = len - drop;
					left = (int)(1 << curr);
					while((curr + drop) < _max) {
						left -= count[curr + drop];
						if(left <= 0) 
							break;
						curr++;
						left <<= 1;
					}
					// check for enough space 
					used += 1U << curr;
					if((type == LENS && used > ENOUGH_LENS) || (type == DISTS && used > ENOUGH_DISTS))
						return 1;
					// point entry in root table to sub-table 
					low = huff & mask;
					(*table)[low].op = (uchar)curr;
					(*table)[low].bits = (uchar)root;
					(*table)[low].val = (ushort)(p_next - *table);
				}
			}
			// fill in remaining table entry if code is incomplete (guaranteed to have
			// at most one remaining entry, since if the code is incomplete, the
			// maximum code length that was allowed to get this far is one bit) 
			if(huff) {
				here.op = (uchar)64;    /* invalid code marker */
				here.bits = (uchar)(len - drop);
				here.val = (ushort)0;
				p_next[huff] = here;
			}
			// set return parameters 
			*table += used;
			*bits = root;
			return 0;
		}
	}
}
//
// COMPRESS
//
// Compresses the source buffer into the destination buffer. The level
// parameter has the same meaning as in deflateInit.  sourceLen is the byte
// length of the source buffer. Upon entry, destLen is the total size of the
// destination buffer, which must be at least 0.1% larger than sourceLen plus
// 12 bytes. Upon exit, destLen is the actual size of the compressed buffer.
//
// compress2 returns Z_OK if success, Z_MEM_ERROR if there was not enough
// memory, Z_BUF_ERROR if there was not enough room in the output buffer,
// Z_STREAM_ERROR if the level parameter is invalid.
//
int ZEXPORT compress2(Bytef * dest, uLongf * destLen, const Bytef * source, uLong sourceLen, int level)
{
	z_stream stream;
	int err;
	const uInt max = (uInt)-1;
	uLong left = *destLen;
	*destLen = 0;
	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;
	stream.opaque = (void *)0;
	err = deflateInit(&stream, level);
	if(err != Z_OK) 
		return err;
	stream.next_out = dest;
	stream.avail_out = 0;
	stream.next_in = source;
	stream.avail_in = 0;
	do {
		if(stream.avail_out == 0) {
			stream.avail_out = (left > (uLong)max) ? max : (uInt)left;
			left -= stream.avail_out;
		}
		if(stream.avail_in == 0) {
			stream.avail_in = (sourceLen > (uLong)max) ? max : (uInt)sourceLen;
			sourceLen -= stream.avail_in;
		}
		err = deflate(&stream, sourceLen ? Z_NO_FLUSH : Z_FINISH);
	} while(err == Z_OK);
	*destLen = stream.total_out;
	deflateEnd(&stream);
	return (err == Z_STREAM_END) ? Z_OK : err;
}

int ZEXPORT compress(Bytef * dest, uLongf * destLen, const Bytef * source, uLong sourceLen)
{
	return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}
//
// If the default memLevel or windowBits for deflateInit() is changed, then this function needs to be updated.
//
uLong ZEXPORT compressBound(uLong sourceLen)
{
	return (sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) + 13);
}
//
// GZWRITE
//
// Initialize state for writing a gzip file.  Mark initialization by setting
// state->size to non-zero.  Return -1 on a memory allocation failure, or 0 on success. 
//
static int gz_init(gz_state * state)
{
	int ret;
	z_stream * p_strm = &(state->strm);
	// allocate input buffer (double size for gzprintf) 
	state->in = (uchar *)SAlloc::M(state->want << 1);
	if(state->in == NULL) {
		gz_error(state, Z_MEM_ERROR, "out of memory");
		return -1;
	}
	else {
		// only need output buffer and deflate state if compressing 
		if(!state->direct) {
			// allocate output buffer 
			state->out = (uchar *)SAlloc::M(state->want);
			if(state->out == NULL) {
				SAlloc::F(state->in);
				gz_error(state, Z_MEM_ERROR, "out of memory");
				return -1;
			}
			// allocate deflate memory, set up for gzip compression 
			p_strm->zalloc = Z_NULL;
			p_strm->zfree = Z_NULL;
			p_strm->opaque = Z_NULL;
			ret = deflateInit2(p_strm, state->level, Z_DEFLATED, MAX_WBITS + 16, DEF_MEM_LEVEL, state->strategy);
			if(ret != Z_OK) {
				SAlloc::F(state->out);
				SAlloc::F(state->in);
				gz_error(state, Z_MEM_ERROR, "out of memory");
				return -1;
			}
			p_strm->next_in = NULL;
		}
		// mark state as initialized 
		state->size = state->want;
		// initialize write buffer if compressing 
		if(!state->direct) {
			p_strm->avail_out = state->size;
			p_strm->next_out = state->out;
			state->x.next = p_strm->next_out;
		}
		return 0;
	}
}
// 
// Compress whatever is at avail_in and next_in and write to the output file.
// Return -1 if there is an error writing to the output file or if gz_init()
// fails to allocate memory, otherwise 0.  flush is assumed to be a valid
// deflate() flush value.  If flush is Z_FINISH, then the deflate() state is
// reset to start a new gzip stream.  If gz->direct is true, then simply write
// to the output file without compressing, and ignore flush.
// 
static int FASTCALL gz_comp(gz_state * state, int flush)
{
	const uint max_ = (static_cast<uint>(-1) >> 2) + 1;
	z_stream * p_strm = &(state->strm);
	// allocate memory if this is the first time through 
	if(state->size == 0 && gz_init(state) == -1)
		return -1;
	else if(state->direct) { // write directly if requested 
		while(p_strm->avail_in) {
			const uint put = (p_strm->avail_in > max_) ? max_ : p_strm->avail_in;
			const int writ = write(state->fd, p_strm->next_in, put);
			if(writ < 0) {
				gz_error(state, Z_ERRNO, zstrerror());
				return -1;
			}
			p_strm->avail_in -= (uint)writ;
			p_strm->next_in += writ;
		}
		return 0;
	}
	else {
		// run deflate() on provided input until it produces no more output 
		int ret = Z_OK;
		uint have;
		do {
			// write out current buffer contents if full, or if flushing, but if
			// doing Z_FINISH then don't write until we get to Z_STREAM_END 
			if(p_strm->avail_out == 0 || (flush != Z_NO_FLUSH && (flush != Z_FINISH || ret == Z_STREAM_END))) {
				while(p_strm->next_out > state->x.next) {
					const uint put = (p_strm->next_out - state->x.next > (int)max_) ? max_ : (uint)(p_strm->next_out - state->x.next);
					const int writ = write(state->fd, state->x.next, put);
					if(writ < 0) {
						gz_error(state, Z_ERRNO, zstrerror());
						return -1;
					}
					state->x.next += writ;
				}
				if(p_strm->avail_out == 0) {
					p_strm->avail_out = state->size;
					p_strm->next_out = state->out;
					state->x.next = state->out;
				}
			}
			// compress 
			have = p_strm->avail_out;
			ret = deflate(p_strm, flush);
			if(ret == Z_STREAM_ERROR) {
				gz_error(state, Z_STREAM_ERROR, "internal error: deflate stream corrupt");
				return -1;
			}
			have -= p_strm->avail_out;
		} while(have);
		// if that completed a deflate stream, allow another to start 
		if(flush == Z_FINISH)
			deflateReset(p_strm);
		return 0; // all done, no errors 
	}
}
//
// Compress len zeros to output.  Return -1 on a write error or memory
// allocation failure by gz_comp(), or 0 on success. 
//
static int FASTCALL gz_zero(gz_state * state, z_off64_t len)
{
	z_streamp strm = &(state->strm);
	// consume whatever's left in the input buffer 
	if(strm->avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
		return -1;
	else {
		// compress len zeros (len guaranteed > 0) 
		int first = 1;
		while(len) {
			const uint n = (GT_OFF(state->size) || (z_off64_t)state->size > len) ? (uint)len : state->size;
			if(first) {
				memzero(state->in, n);
				first = 0;
			}
			strm->avail_in = n;
			strm->next_in = state->in;
			state->x.pos += n;
			if(gz_comp(state, Z_NO_FLUSH) == -1)
				return -1;
			len -= n;
		}
		return 0;
	}
}
// 
// Write len bytes from buf to file.  Return the number of bytes written.  If
// the returned value is less than len, then there was an error.
// 
static size_t FASTCALL gz_write(gz_state * state, voidpc buf, size_t len)
{
	size_t put = len;
	// if len is zero, avoid unnecessary operations 
	if(len == 0)
		return 0;
	// allocate memory if this is the first time through 
	if(state->size == 0 && gz_init(state) == -1)
		return 0;
	// check for seek request 
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return 0;
	}
	// for small len, copy to input buffer, otherwise compress directly 
	if(len < state->size) {
		// copy to input buffer, compress when full 
		do {
			uint have, copy;
			if(state->strm.avail_in == 0)
				state->strm.next_in = state->in;
			have = (uint)((state->strm.next_in + state->strm.avail_in) - state->in);
			copy = state->size - have;
			SETMIN(copy, len);
			memcpy(state->in + have, buf, copy);
			state->strm.avail_in += copy;
			state->x.pos += copy;
			buf = (const char *)buf + copy;
			len -= copy;
			if(len && gz_comp(state, Z_NO_FLUSH) == -1)
				return 0;
		} while(len);
	}
	else {
		// consume whatever's left in the input buffer 
		if(state->strm.avail_in && gz_comp(state, Z_NO_FLUSH) == -1)
			return 0;
		// directly compress user buffer to file 
		state->strm.next_in = static_cast<const Bytef *>(buf);
		do {
			uint n = static_cast<uint>(-1);
			SETMIN(n, len);
			state->strm.avail_in = n;
			state->x.pos += n;
			if(gz_comp(state, Z_NO_FLUSH) == -1)
				return 0;
			len -= n;
		} while(len);
	}
	// input was all buffered or compressed 
	return put;
}

int ZEXPORT gzwrite(gzFile file, voidpc buf, uint len)
{
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return 0;
	state = (gz_state *)file;
	// check that we're writing and that there's no error 
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return 0;
	// since an int is returned, make sure len fits in one, otherwise return
	// with an error (this avoids a flaw in the interface) 
	if((int)len < 0) {
		gz_error(state, Z_DATA_ERROR, "requested length does not fit in int");
		return 0;
	}
	return (int)gz_write(state, buf, len); // write len bytes from buf (the return value will fit in an int) 
}

size_t ZEXPORT gzfwrite(voidpc buf, size_t size, size_t nitems, gzFile file)
{
	size_t len = 0;
	if(file) {
		// get internal structure 
		gz_state * state = (gz_state *)file;
		// check that we're writing and that there's no error 
		if(state->mode == GZ_WRITE && state->err == Z_OK) {
			// compute bytes to read -- error on overflow 
			len = nitems * size;
			if(size && (len / size) != nitems) {
				gz_error(state, Z_STREAM_ERROR, "request does not fit in a size_t");
				len = 0;
			}
			else {
				// write len bytes to buf, return the number of full items written 
				len = len ? (gz_write(state, buf, len) / size) : 0;
			}
		}
	}
	return len;
}

int ZEXPORT gzputc(gzFile file, int c)
{
	uint have;
	uchar buf[1];
	gz_state * state;
	z_streamp strm;
	// get internal structure 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	strm = &(state->strm);
	// check that we're writing and that there's no error 
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return -1;
	// check for seek request 
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return -1;
	}
	// try writing to input buffer for speed (state->size == 0 if buffer not initialized) 
	if(state->size) {
		if(strm->avail_in == 0)
			strm->next_in = state->in;
		have = (uint)((strm->next_in + strm->avail_in) - state->in);
		if(have < state->size) {
			state->in[have] = static_cast<uchar>(c);
			strm->avail_in++;
			state->x.pos++;
			return c & 0xff;
		}
	}
	// no room in buffer or not initialized, use gz_write() 
	buf[0] = static_cast<uchar>(c);
	return (gz_write(state, buf, 1) != 1) ? -1 : (c & 0xff);
}

int ZEXPORT gzputs(gzFile file, const char * str)
{
	if(file == NULL)
		return -1;
	else {
		gz_state * state = (gz_state *)file; // get internal structure 
		// check that we're writing and that there's no error 
		if(state->mode != GZ_WRITE || state->err != Z_OK)
			return -1;
		else {
			// write string 
			size_t len = sstrlen(str);
			int    ret = gz_write(state, str, len);
			return (ret == 0 && len != 0) ? -1 : ret;
		}
	}
}

#if defined(STDC) || defined(Z_HAVE_STDARG_H)
//#include <stdarg.h>

int ZEXPORTVA gzvprintf(gzFile file, const char * format, va_list va)
{
	int    len;
	uint   left;
	char * next;
	gz_state * state;
	z_streamp strm;
	// get internal structure 
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	strm = &(state->strm);
	// check that we're writing and that there's no error 
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return Z_STREAM_ERROR;
	// make sure we have some buffer space 
	if(state->size == 0 && gz_init(state) == -1)
		return state->err;
	// check for seek request 
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return state->err;
	}
	// 
	// do the printf() into the input buffer, put length in len -- the input
	// buffer is double-sized just for this function, so there is guaranteed to
	// be state->size bytes available after the current contents 
	// 
	if(strm->avail_in == 0)
		strm->next_in = state->in;
	next = (char *)(state->in + (strm->next_in - state->in) + strm->avail_in);
	next[state->size - 1] = 0;
#ifdef NO_vsnprintf
#ifdef HAS_vsprintf_void
	(void)vsprintf(next, format, va);
	for(len = 0; len < state->size; len++)
		if(next[len] == 0) break;
#else
	len = vsprintf(next, format, va);
#endif
#else
#ifdef HAS_vsnprintf_void
	(void)vsnprintf(next, state->size, format, va);
	len = strlen(next);
#else
	len = vsnprintf(next, state->size, format, va);
#endif
#endif
	// check that printf() results fit in buffer
	if(len == 0 || (uint)len >= state->size || next[state->size - 1] != 0)
		return 0;
	// update buffer and position, compress first half if past that 
	strm->avail_in += (uint)len;
	state->x.pos += len;
	if(strm->avail_in >= state->size) {
		left = strm->avail_in - state->size;
		strm->avail_in = state->size;
		if(gz_comp(state, Z_NO_FLUSH) == -1)
			return state->err;
		memcpy(state->in, state->in + state->size, left);
		strm->next_in = state->in;
		strm->avail_in = left;
	}
	return len;
}

int ZEXPORTVA gzprintf(gzFile file, const char * format, ...)
{
	va_list va;
	int ret;
	va_start(va, format);
	ret = gzvprintf(file, format, va);
	va_end(va);
	return ret;
}

#else /* !STDC && !Z_HAVE_STDARG_H */

/* -- see zlib.h -- */
int ZEXPORTVA gzprintf(gzFile file, const char * format, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9,
    int a10, int a11, int a12, int a13, int a14, int a15, int a16, int a17, int a18, int a19, int a20)
{
	uint len, left;
	char * next;
	gz_state * state;
	z_streamp strm;
	// get internal structure 
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	strm = &(state->strm);
	// check that can really pass pointer in ints 
	if(sizeof(int) != sizeof(void *))
		return Z_STREAM_ERROR;
	// check that we're writing and that there's no error 
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return Z_STREAM_ERROR;
	// make sure we have some buffer space 
	if(state->size == 0 && gz_init(state) == -1)
		return state->error;
	// check for seek request 
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return state->error;
	}
	/* do the printf() into the input buffer, put length in len -- the input
	   buffer is double-sized just for this function, so there is guaranteed to
	   be state->size bytes available after the current contents */
	if(strm->avail_in == 0)
		strm->next_in = state->in;
	next = (char *)(strm->next_in + strm->avail_in);
	next[state->size - 1] = 0;
#ifdef NO_snprintf
#  ifdef HAS_sprintf_void
	sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
	for(len = 0; len < size; len++)
		if(next[len] == 0)
			break;
#  else
	len = sprintf(next, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
#  endif
#else
#  ifdef HAS_snprintf_void
	snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
	len = strlen(next);
#  else
	len = snprintf(next, state->size, format, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
#  endif
#endif
	/* check that printf() results fit in buffer */
	if(len == 0 || len >= state->size || next[state->size - 1] != 0)
		return 0;
	/* update buffer and position, compress first half if past that */
	strm->avail_in += len;
	state->x.pos += len;
	if(strm->avail_in >= state->size) {
		left = strm->avail_in - state->size;
		strm->avail_in = state->size;
		if(gz_comp(state, Z_NO_FLUSH) == -1)
			return state->err;
		memcpy(state->in, state->in + state->size, left);
		strm->next_in = state->in;
		strm->avail_in = left;
	}
	return (int)len;
}

#endif

int ZEXPORT gzflush(gzFile file, int flush)
{
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	// check that we're writing and that there's no error 
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return Z_STREAM_ERROR;
	// check flush parameter 
	if(flush < 0 || flush > Z_FINISH)
		return Z_STREAM_ERROR;
	// check for seek request 
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return state->err;
	}
	// compress remaining data with requested flush 
	(void)gz_comp(state, flush);
	return state->err;
}

int ZEXPORT gzsetparams(gzFile file, int level, int strategy)
{
	gz_state * state;
	z_streamp strm;
	/* get internal structure */
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	strm = &(state->strm);
	/* check that we're writing and that there's no error */
	if(state->mode != GZ_WRITE || state->err != Z_OK)
		return Z_STREAM_ERROR;
	/* if no change is requested, then do nothing */
	if(level == state->level && strategy == state->strategy)
		return Z_OK;
	/* check for seek request */
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			return state->err;
	}
	/* change compression parameters for subsequent input */
	if(state->size) {
		/* flush previous input with previous parameters before changing */
		if(strm->avail_in && gz_comp(state, Z_BLOCK) == -1)
			return state->err;
		deflateParams(strm, level, strategy);
	}
	state->level = level;
	state->strategy = strategy;
	return Z_OK;
}

int ZEXPORT gzclose_w(gzFile file)
{
	int ret = Z_OK;
	gz_state * state;
	/* get internal structure */
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	/* check that we're writing */
	if(state->mode != GZ_WRITE)
		return Z_STREAM_ERROR;
	/* check for seek request */
	if(state->seek) {
		state->seek = 0;
		if(gz_zero(state, state->skip) == -1)
			ret = state->err;
	}
	/* flush, free memory, and close file */
	if(gz_comp(state, Z_FINISH) == -1)
		ret = state->err;
	if(state->size) {
		if(!state->direct) {
			(void)deflateEnd(&(state->strm));
			SAlloc::F(state->out);
		}
		SAlloc::F(state->in);
	}
	gz_error(state, Z_OK, 0);
	SAlloc::F(state->path);
	if(close(state->fd) == -1)
		ret = Z_ERRNO;
	SAlloc::F(state);
	return ret;
}
//
// GZREAD
//
// Use read() to load a buffer -- return -1 on error, otherwise 0.  Read from
// state->fd, and update state->eof, state->err, and state->msg as appropriate.
// This function needs to loop on read(), since read() is not guaranteed to
// read the number of bytes requested, depending on the type of descriptor. 
// 
static int FASTCALL gz_load(gz_state * state, uchar * buf, uint len, uint * have)
{
	int ret;
	const uint _max = (static_cast<uint>(-1) >> 2) + 1;
	*have = 0;
	do {
		uint get = len - *have;
		SETMIN(get, _max);
		ret = read(state->fd, buf + *have, get);
		if(ret <= 0)
			break;
		*have += (uint)ret;
	} while(*have < len);
	if(ret < 0) {
		gz_error(state, Z_ERRNO, zstrerror());
		return -1;
	}
	else {
		if(!ret)
			state->eof = 1;
		return 0;
	}
}
// 
// Load up input buffer and set eof flag if last data loaded -- return -1 on
// error, 0 otherwise.  Note that the eof flag is set when the end of the input
// file is reached, even though there may be unused data in the buffer.  Once
// that data has been used, no more attempts will be made to read the file.
// If strm->avail_in != 0, then the current data is moved to the beginning of
// the input buffer, and then the remainder of the buffer is loaded with the
// available data from the input file.
// 
static int FASTCALL gz_avail(gz_state * state)
{
	uint   got;
	z_streamp strm = &(state->strm);
	if(state->err != Z_OK && state->err != Z_BUF_ERROR)
		return -1;
	if(state->eof == 0) {
		if(strm->avail_in) { /* copy what's there to the start */
			uchar * p = state->in;
			const uchar * q = strm->next_in;
			uint  n = strm->avail_in;
			do {
				*p++ = *q++;
			} while(--n);
		}
		if(gz_load(state, state->in + strm->avail_in, state->size - strm->avail_in, &got) == -1)
			return -1;
		strm->avail_in += got;
		strm->next_in = state->in;
	}
	return 0;
}
// 
// Look for gzip header, set up for inflate or copy.  state->x.have must be 0.
// If this is the first time in, allocate required memory.  state->how will be
// left unchanged if there is no more input data available, will be set to COPY
// if there is no gzip header and direct copying will be performed, or it will
// be set to GZIP for decompression.  If direct copying, then leftover input
// data from the input buffer will be copied to the output buffer.  In that
// case, all further file reads will be directly to either the output buffer or
// a user buffer.  If decompressing, the inflate state will be initialized.
// gz_look() will return 0 on success or -1 on failure. 
// 
static int FASTCALL gz_look(gz_state * state)
{
	z_streamp strm = &(state->strm);
	// allocate read buffers and inflate memory 
	if(state->size == 0) {
		// allocate buffers 
		state->in = (uchar *)SAlloc::M(state->want);
		state->out = (uchar *)SAlloc::M(state->want << 1);
		if(state->in == NULL || state->out == NULL) {
			SAlloc::F(state->out);
			SAlloc::F(state->in);
			gz_error(state, Z_MEM_ERROR, "out of memory");
			return -1;
		}
		state->size = state->want;
		// allocate inflate memory 
		state->strm.zalloc = Z_NULL;
		state->strm.zfree = Z_NULL;
		state->strm.opaque = Z_NULL;
		state->strm.avail_in = 0;
		state->strm.next_in = Z_NULL;
		if(inflateInit2(&(state->strm), 15 + 16) != Z_OK) { /* gunzip */
			SAlloc::F(state->out);
			SAlloc::F(state->in);
			state->size = 0;
			gz_error(state, Z_MEM_ERROR, "out of memory");
			return -1;
		}
	}
	// get at least the magic bytes in the input buffer 
	if(strm->avail_in < 2) {
		if(gz_avail(state) == -1)
			return -1;
		else if(strm->avail_in == 0)
			return 0;
	}
	// look for gzip magic bytes -- if there, do gzip decoding (note: there is
	// a logical dilemma here when considering the case of a partially written
	// gzip file, to wit, if a single 31 byte is written, then we cannot tell
	// whether this is a single-byte file, or just a partially written gzip
	// file -- for here we assume that if a gzip file is being written, then
	// the header will be written in a single operation, so that reading a
	// single byte is sufficient indication that it is not a gzip file) 
	if(strm->avail_in > 1 && strm->next_in[0] == 31 && strm->next_in[1] == 139) {
		inflateReset(strm);
		state->how = GZSTATE_GZIP;
		state->direct = 0;
		return 0;
	}
	// no gzip header -- if we were decoding gzip before, then this is trailing
	// garbage.  Ignore the trailing garbage and finish. 
	if(state->direct == 0) {
		strm->avail_in = 0;
		state->eof = 1;
		state->x.have = 0;
		return 0;
	}
	// doing raw i/o, copy any leftover input to output -- this assumes that
	// the output buffer is larger than the input buffer, which also assures space for gzungetc() 
	state->x.next = state->out;
	if(strm->avail_in) {
		memcpy(state->x.next, strm->next_in, strm->avail_in);
		state->x.have = strm->avail_in;
		strm->avail_in = 0;
	}
	state->how = GZSTATE_COPY;
	state->direct = 1;
	return 0;
}
// 
// Decompress from input to the provided next_out and avail_out in the state.
// On return, state->x.have and state->x.next point to the just decompressed
// data.  If the gzip stream completes, state->how is reset to LOOK to look for
// the next gzip stream or raw data, once state->x.have is depleted.  
// Returns 0 on success, -1 on failure. 
// 
static int gz_decomp(gz_state * state)
{
	int ret = Z_OK;
	z_streamp strm = &(state->strm);
	// fill output buffer up to end of deflate stream 
	uint had = strm->avail_out;
	do {
		// get more input for inflate() 
		if(strm->avail_in == 0 && gz_avail(state) == -1)
			return -1;
		if(strm->avail_in == 0) {
			gz_error(state, Z_BUF_ERROR, "unexpected end of file");
			break;
		}
		// decompress and handle errors 
		ret = inflate(strm, Z_NO_FLUSH);
		if(ret == Z_STREAM_ERROR || ret == Z_NEED_DICT) {
			gz_error(state, Z_STREAM_ERROR, "internal error: inflate stream corrupt");
			return -1;
		}
		if(ret == Z_MEM_ERROR) {
			gz_error(state, Z_MEM_ERROR, "out of memory");
			return -1;
		}
		if(ret == Z_DATA_ERROR) { // deflate stream invalid 
			gz_error(state, Z_DATA_ERROR, strm->msg == NULL ? "compressed data error" : strm->msg);
			return -1;
		}
	} while(strm->avail_out && ret != Z_STREAM_END);
	// update available output 
	state->x.have = had - strm->avail_out;
	state->x.next = strm->next_out - state->x.have;
	// if the gzip stream completed successfully, look for another 
	if(ret == Z_STREAM_END)
		state->how = GZSTATE_LOOK;
	return 0; // good decompression 
}
// 
// Fetch data and put it in the output buffer.  Assumes state->x.have is 0.
// Data is either copied from the input file or decompressed from the input
// file depending on state->how.  If state->how is LOOK, then a gzip header is
// looked for to determine whether to copy or decompress.  Returns -1 on error,
// otherwise 0.  gz_fetch() will leave state->how as COPY or GZIP unless the
// end of the input file has been reached and all data has been processed.  
// 
static int FASTCALL gz_fetch(gz_state * state)
{
	z_streamp strm = &(state->strm);
	do {
		switch(state->how) {
			case GZSTATE_LOOK: /* -> GZSTATE_LOOK, GZSTATE_COPY (only if never GZSTATE_GZIP), or GZSTATE_GZIP */
			    if(gz_look(state) == -1)
				    return -1;
			    if(state->how == GZSTATE_LOOK)
				    return 0;
			    break;
			case GZSTATE_COPY: /* -> GZSTATE_COPY */
			    if(gz_load(state, state->out, state->size << 1, &(state->x.have)) == -1)
				    return -1;
			    state->x.next = state->out;
			    return 0;
			case GZSTATE_GZIP: /* -> GZSTATE_GZIP or GZSTATE_LOOK (if end of gzip stream) */
			    strm->avail_out = state->size << 1;
			    strm->next_out = state->out;
			    if(gz_decomp(state) == -1)
				    return -1;
		}
	} while(state->x.have == 0 && (!state->eof || strm->avail_in));
	return 0;
}
//
// Skip len uncompressed bytes of output.  Return -1 on error, 0 on success. 
//
static int FASTCALL gz_skip(gz_state * state, z_off64_t len)
{
	while(len) { // skip over len bytes or reach end-of-file, whichever comes first 
		if(state->x.have) { // skip over whatever is in output buffer 
			const uint n = (GT_OFF(state->x.have) || (z_off64_t)state->x.have > len) ? (uint)len : state->x.have;
			state->x.have -= n;
			state->x.next += n;
			state->x.pos += n;
			len -= n;
		}
		else if(state->eof && state->strm.avail_in == 0) // output buffer empty -- return if we're at the end of the input 
			break;
		else { // need more data to skip -- load up output buffer 
			if(gz_fetch(state) == -1) // get more output, looking for header if required 
				return -1;
		}
	}
	return 0;
}
// 
// Read len bytes into buf from file, or less than len up to the end of the
// input.  Return the number of bytes read.  If zero is returned, either the
// end of file was reached, or there was an error.  state->err must be
// consulted in that case to determine which. 
// 
static size_t FASTCALL gz_read(gz_state * state, void * buf, size_t len)
{
	size_t got = 0;
	// if len is zero, avoid unnecessary operations 
	if(len) {
		// process a skip request 
		if(state->seek) {
			state->seek = 0;
			if(gz_skip(state, state->skip) == -1)
				return 0;
		}
		// get len bytes to buf, or less than len if at the end 
		do {
			// set n to the maximum amount of len that fits in an unsigned int 
			uint   n = static_cast<uint>(-1);
			if(n > len)
				n = len;
			// first just try copying data from the output buffer 
			if(state->x.have) {
				if(state->x.have < n)
					n = state->x.have;
				memcpy(buf, state->x.next, n);
				state->x.next += n;
				state->x.have -= n;
			}
			// output buffer empty -- return if we're at the end of the input 
			else if(state->eof && state->strm.avail_in == 0) {
				state->past = 1; /* tried to read past end */
				break;
			}
			// need output data -- for small len or new stream load up our output buffer 
			else if(state->how == GZSTATE_LOOK || n < (state->size << 1)) {
				// get more output, looking for header if required 
				if(gz_fetch(state) == -1)
					return 0;
				continue; // no progress yet -- go back to copy above 
				// the copy above assures that we will leave with space in the output buffer, allowing at least one gzungetc() to succeed 
			}
			// large len -- read directly into user buffer 
			else if(state->how == GZSTATE_COPY) { // read directly 
				if(gz_load(state, (uchar *)buf, n, &n) == -1)
					return 0;
			}
			// large len -- decompress directly into user buffer 
			else { // state->how == GZSTATE_GZIP 
				state->strm.avail_out = n;
				state->strm.next_out = (uchar *)buf;
				if(gz_decomp(state) == -1)
					return 0;
				n = state->x.have;
				state->x.have = 0;
			}
			// update progress 
			len -= n;
			buf = (char *)buf + n;
			got += n;
			state->x.pos += n;
		} while(len);
	}
	return got; // return number of bytes read into user buffer 
}

int ZEXPORT gzread(gzFile file, void * buf, uint len)
{
	if(file == NULL)
		return -1;
	else {
		gz_state * state = (gz_state *)file; // get internal structure 
		// check that we're reading and that there's no (serious) error 
		if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
			return -1;
		// since an int is returned, make sure len fits in one, otherwise return
		// with an error (this avoids a flaw in the interface) 
		if((int)len < 0) {
			gz_error(state, Z_STREAM_ERROR, "request does not fit in an int");
			return -1;
		}
		len = gz_read(state, buf, len); // read len or fewer bytes to buf 
		// check for an error 
		if(len == 0 && state->err != Z_OK && state->err != Z_BUF_ERROR)
			return -1;
		return (int)len; // return the number of bytes read (this is assured to fit in an int) 
	}
}

size_t ZEXPORT gzfread(void * buf, size_t size, size_t nitems, gzFile file)
{
	size_t len;
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return 0;
	state = (gz_state *)file;
	// check that we're reading and that there's no (serious) error 
	if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
		return 0;
	// compute bytes to read -- error on overflow 
	len = nitems * size;
	if(size && len / size != nitems) {
		gz_error(state, Z_STREAM_ERROR, "request does not fit in a size_t");
		return 0;
	}
	// read len or fewer bytes to buf, return the number of full items read 
	return len ? gz_read(state, buf, len) / size : 0;
}

#ifdef Z_PREFIX_SET
	#undef z_gzgetc
#else
	#undef gzgetc
#endif

int ZEXPORT gzgetc(gzFile file)
{
	int ret;
	uchar buf[1];
	// get internal structure 
	if(file == NULL)
		return -1;
	else {
		gz_state * state = (gz_state *)file;
		// check that we're reading and that there's no (serious) error 
		if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
			return -1;
		else {
			// try output buffer (no need to check for skip request) 
			if(state->x.have) {
				state->x.have--;
				state->x.pos++;
				return *(state->x.next)++;
			}
			else {
				// nothing there -- try gz_read() 
				ret = gz_read(state, buf, 1);
				return (ret < 1) ? -1 : buf[0];
			}
		}
	}
}

int ZEXPORT gzgetc_(gzFile file)
{
	return gzgetc(file);
}

int ZEXPORT gzungetc(int c, gzFile file)
{
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	// check that we're reading and that there's no (serious) error 
	if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
		return -1;
	// process a skip request 
	if(state->seek) {
		state->seek = 0;
		if(gz_skip(state, state->skip) == -1)
			return -1;
	}
	// can't push EOF 
	if(c < 0)
		return -1;
	// if output buffer empty, put byte at end (allows more pushing) 
	if(state->x.have == 0) {
		state->x.have = 1;
		state->x.next = state->out + (state->size << 1) - 1;
		state->x.next[0] = static_cast<uchar>(c);
		state->x.pos--;
		state->past = 0;
		return c;
	}
	/* if no room, give up (must have already done a gzungetc()) */
	if(state->x.have == (state->size << 1)) {
		gz_error(state, Z_DATA_ERROR, "out of room to push characters");
		return -1;
	}
	// slide output data if needed and insert byte before existing data 
	if(state->x.next == state->out) {
		uchar * src = state->out + state->x.have;
		uchar * dest = state->out + (state->size << 1);
		while(src > state->out)
			*--dest = *--src;
		state->x.next = dest;
	}
	state->x.have++;
	state->x.next--;
	state->x.next[0] = static_cast<uchar>(c);
	state->x.pos--;
	state->past = 0;
	return c;
}
//
// -- see zlib.h -- 
//
char * ZEXPORT gzgets(gzFile file, char * buf, int len)
{
	char * str = 0;
	uint left, n;
	uchar * eol;
	gz_state * state;
	// check parameters and get internal structure 
	if(file == NULL || buf == NULL || len < 1)
		return NULL;
	state = (gz_state *)file;
	// check that we're reading and that there's no (serious) error 
	if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
		return NULL;
	// process a skip request 
	if(state->seek) {
		state->seek = 0;
		if(gz_skip(state, state->skip) == -1)
			return NULL;
	}
	// copy output bytes up to new line or len - 1, whichever comes first --
	// append a terminating zero to the string (we don't check for a zero in
	// the contents, let the user worry about that) 
	str = buf;
	left = (uint)len - 1;
	if(left) do {
		// assure that something is in the output buffer 
		if(state->x.have == 0 && gz_fetch(state) == -1)
			return NULL;  /* error */
		if(state->x.have == 0) { /* end of file */
			state->past = 1; /* read past end */
			break;  /* return what we have */
		}
		/* look for end-of-line in current output buffer */
		n = state->x.have > left ? left : state->x.have;
		eol = (uchar *)memchr(state->x.next, '\n', n);
		if(eol != NULL)
			n = (uint)(eol - state->x.next) + 1;
		/* copy through end-of-line, or remainder if not found */
		memcpy(buf, state->x.next, n);
		state->x.have -= n;
		state->x.next += n;
		state->x.pos += n;
		left -= n;
		buf += n;
	} while(left && eol == NULL);
	// return terminated string, or if nothing, end of file 
	if(buf == str)
		return NULL;
	else {
		buf[0] = 0;
		return str;
	}
}

int ZEXPORT gzdirect(gzFile file)
{
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return 0;
	state = (gz_state *)file;
	// if the state is not known, but we can find out, then do so (this is mainly for right after a gzopen() or gzdopen()) 
	if(state->mode == GZ_READ && state->how == GZSTATE_LOOK && state->x.have == 0)
		(void)gz_look(state);
	return state->direct; // return 1 if transparent, 0 if processing a gzip stream 
}
//
// -- see zlib.h -- 
//
int ZEXPORT gzclose_r(gzFile file)
{
	int ret, err;
	gz_state * state;
	/* get internal structure */
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	/* check that we're reading */
	if(state->mode != GZ_READ)
		return Z_STREAM_ERROR;
	/* free memory and close file */
	if(state->size) {
		inflateEnd(&(state->strm));
		SAlloc::F(state->out);
		SAlloc::F(state->in);
	}
	err = state->err == Z_BUF_ERROR ? Z_BUF_ERROR : Z_OK;
	gz_error(state, Z_OK, 0);
	SAlloc::F(state->path);
	ret = close(state->fd);
	SAlloc::F(state);
	return ret ? Z_ERRNO : err;
}
//
// GZCLOSE
//
int ZEXPORT gzclose(gzFile file)
{
#ifndef NO_GZCOMPRESS
	gz_state * state;
	if(file == NULL)
		return Z_STREAM_ERROR;
	state = (gz_state *)file;
	return state->mode == GZ_READ ? gzclose_r(file) : gzclose_w(file);
#else
	return gzclose_r(file);
#endif
}
//
// GZLIB
//
#if defined(_WIN32) && !defined(__BORLANDC__) && !defined(__MINGW32__)
	#define LSEEK _lseeki64
#else
	#if defined(_LARGEFILE64_SOURCE) && _LFS64_LARGEFILE-0
		#define LSEEK lseek64
	#else
		#define LSEEK lseek
	#endif
#endif

#if defined UNDER_CE

/* Map the Windows error number in ERROR to a locale-dependent error message
   string and return a pointer to it.  Typically, the values for ERROR come
   from GetLastError.

   The string pointed to shall not be modified by the application, but may be
   overwritten by a subsequent call to gz_strwinerror

   The gz_strwinerror function does not change the current setting of
   GetLastError. */
char ZLIB_INTERNAL * gz_strwinerror(DWORD error)
{
	static char buf[1024];
	wchar_t * msgbuf;
	DWORD lasterr = GetLastError();
	DWORD chars = ::FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, error, 0/* Default language */, (LPVOID)&msgbuf, 0, 0);
	if(chars != 0) {
		/* If there is an \r\n appended, zap it.  */
		if(chars >= 2 && msgbuf[chars - 2] == '\r' && msgbuf[chars - 1] == '\n') {
			chars -= 2;
			msgbuf[chars] = 0;
		}
		if(chars > sizeof(buf) - 1) {
			chars = sizeof(buf) - 1;
			msgbuf[chars] = 0;
		}
		wcstombs(buf, msgbuf, chars + 1);
		LocalFree(msgbuf);
	}
	else {
		sprintf(buf, "unknown win32 error (%ld)", error);
	}
	SetLastError(lasterr);
	return buf;
}

#endif /* UNDER_CE */
//
// Reset gzip file state 
//
static void gz_reset(gz_state * state)
{
	state->x.have = 0;          /* no output data available */
	if(state->mode == GZ_READ) { /* for reading ... */
		state->eof = 0;     /* not at end of file */
		state->past = 0;    /* have not read past end yet */
		state->how = GZSTATE_LOOK;  /* look for gzip header */
	}
	state->seek = 0;            /* no seek request pending */
	gz_error(state, Z_OK, 0); /* clear error */
	state->x.pos = 0;           /* no uncompressed data yet */
	state->strm.avail_in = 0;   /* no input data yet */
}
//
// Open a gzip file either by name or file descriptor. 
//
static gzFile gz_open(const void * path, int fd, const char * mode)
{
	gz_state * state;
	size_t len;
	int oflag;
#ifdef O_CLOEXEC
	int cloexec = 0;
#endif
#ifdef O_EXCL
	int exclusive = 0;
#endif
	/* check input */
	if(path == NULL)
		return NULL;
	/* allocate gzFile structure to return */
	state = static_cast<gz_state *>(SAlloc::M(sizeof(gz_state)));
	if(state == NULL)
		return NULL;
	state->size = 0;        /* no buffers allocated yet */
	state->want = GZBUFSIZE; /* requested buffer size */
	state->msg = NULL;      /* no error message yet */
	/* interpret mode */
	state->mode = GZ_NONE;
	state->level = Z_DEFAULT_COMPRESSION;
	state->strategy = Z_DEFAULT_STRATEGY;
	state->direct = 0;
	while(*mode) {
		if(*mode >= '0' && *mode <= '9')
			state->level = *mode - '0';
		else
			switch(*mode) {
				case 'r': state->mode = GZ_READ; break;
#ifndef NO_GZCOMPRESS
				case 'w': state->mode = GZ_WRITE; break;
				case 'a': state->mode = GZ_APPEND; break;
#endif
				case '+': /* can't read and write at the same time */
				    SAlloc::F(state);
				    return NULL;
				case 'b': /* ignore -- will request binary anyway */
				    break;
#ifdef O_CLOEXEC
				case 'e':
				    cloexec = 1;
				    break;
#endif
#ifdef O_EXCL
				case 'x': exclusive = 1; break;
#endif
				case 'f': state->strategy = Z_FILTERED; break;
				case 'h': state->strategy = Z_HUFFMAN_ONLY; break;
				case 'R': state->strategy = Z_RLE; break;
				case 'F': state->strategy = Z_FIXED; break;
				case 'T': state->direct = 1; break;
				default: /* could consider as an error, but just ignore */
				    ;
			}
		mode++;
	}
	/* must provide an "r", "w", or "a" */
	if(state->mode == GZ_NONE) {
		SAlloc::F(state);
		return NULL;
	}
	/* can't force transparent read */
	if(state->mode == GZ_READ) {
		if(state->direct) {
			SAlloc::F(state);
			return NULL;
		}
		state->direct = 1; /* for empty file */
	}
	/* save the path name for error messages */
#ifdef WIDECHAR
	if(fd == -2) {
		len = wcstombs(NULL, static_cast<const wchar_t *>(path), 0);
		if(len == (size_t)-1)
			len = 0;
	}
	else
#endif
	len = strlen((const char *)path);
	state->path = static_cast<char *>(SAlloc::M(len+1));
	if(state->path == NULL) {
		SAlloc::F(state);
		return NULL;
	}
#ifdef WIDECHAR
	if(fd == -2)
		if(len)
			wcstombs(state->path, static_cast<const wchar_t *>(path), len + 1);
		else
			*(state->path) = 0;
	else
#endif
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
	(void)snprintf(state->path, len + 1, "%s", (const char *)path);
#else
	strcpy(state->path, path);
#endif
	// compute the flags for open() 
	oflag =
#ifdef O_LARGEFILE
	    O_LARGEFILE |
#endif
#ifdef O_BINARY
	    O_BINARY |
#endif
#ifdef O_CLOEXEC
	    (cloexec ? O_CLOEXEC : 0) |
#endif
	    (state->mode == GZ_READ ? O_RDONLY : (O_WRONLY | O_CREAT |
#ifdef O_EXCL
		    (exclusive ? O_EXCL : 0) |
#endif
		    (state->mode == GZ_WRITE ? O_TRUNC : O_APPEND)));

	/* open the file with the appropriate flags (or just use fd) */
	state->fd = fd > -1 ? fd : (
#ifdef WIDECHAR
	    fd == -2 ? _wopen(static_cast<const wchar_t *>(path), oflag, 0666) :
#endif
	    open(static_cast<const char *>(path), oflag, 0666));
	if(state->fd == -1) {
		SAlloc::F(state->path);
		SAlloc::F(state);
		return NULL;
	}
	if(state->mode == GZ_APPEND) {
		LSEEK(state->fd, 0, SEEK_END); /* so gzoffset() is correct */
		state->mode = GZ_WRITE; /* simplify later checks */
	}
	/* save the current position for rewinding (only if reading) */
	if(state->mode == GZ_READ) {
		state->start = LSEEK(state->fd, 0, SEEK_CUR);
		if(state->start == -1) state->start = 0;
	}
	gz_reset(state); /* initialize stream */
	return (gzFile)state; /* return stream */
}

gzFile ZEXPORT gzopen(const char * path, const char * mode)   { return gz_open(path, -1, mode); }
gzFile ZEXPORT gzopen64(const char * path, const char * mode) { return gz_open(path, -1, mode); }

gzFile ZEXPORT gzdopen(int fd, const char * mode)
{
	char * path;    /* identifier for error messages */
	gzFile gz;
	if(fd == -1 || (path = static_cast<char *>(SAlloc::M(7 + 3 * sizeof(int)))) == NULL)
		return NULL;
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
	(void)snprintf(path, 7 + 3 * sizeof(int), "<fd:%d>", fd);
#else
	sprintf(path, "<fd:%d>", fd); /* for debugging */
#endif
	gz = gz_open(path, fd, mode);
	SAlloc::F(path);
	return gz;
}

#ifdef WIDECHAR
	gzFile ZEXPORT gzopen_w(const wchar_t * path, const char * mode) { return gz_open(path, -2, mode); }
#endif

int ZEXPORT gzbuffer(gzFile file, uint size)
{
	gz_state * state;
	/* get internal structure and check integrity */
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return -1;
	/* make sure we haven't already allocated memory */
	if(state->size != 0)
		return -1;
	/* check and set requested size */
	if((size << 1) < size)
		return -1;      /* need to be able to double it */
	if(size < 2)
		size = 2;       /* need two bytes to check magic header */
	state->want = size;
	return 0;
}

int ZEXPORT gzrewind(gzFile file)
{
	gz_state * state;
	// get internal structure 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	// check that we're reading and that there's no error 
	if(state->mode != GZ_READ || (state->err != Z_OK && state->err != Z_BUF_ERROR))
		return -1;
	// back up and start over 
	if(LSEEK(state->fd, state->start, SEEK_SET) == -1)
		return -1;
	gz_reset(state);
	return 0;
}

z_off64_t ZEXPORT gzseek64(gzFile file, z_off64_t offset, int whence)
{
	uint n;
	z_off64_t ret;
	gz_state * state;
	// get internal structure and check integrity 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return -1;
	if(state->err != Z_OK && state->err != Z_BUF_ERROR) // check that there's no error 
		return -1;
	if(whence != SEEK_SET && whence != SEEK_CUR) // can only seek from start or relative to current position 
		return -1;
	if(whence == SEEK_SET) // normalize offset to a SEEK_CUR specification 
		offset -= state->x.pos;
	else if(state->seek)
		offset += state->skip;
	state->seek = 0;
	// if within raw area while reading, just go there 
	if(state->mode == GZ_READ && state->how == GZSTATE_COPY && (state->x.pos + offset) >= 0) {
		ret = LSEEK(state->fd, offset - state->x.have, SEEK_CUR);
		if(ret == -1)
			return -1;
		state->x.have = 0;
		state->eof = 0;
		state->past = 0;
		state->seek = 0;
		gz_error(state, Z_OK, 0);
		state->strm.avail_in = 0;
		state->x.pos += offset;
		return state->x.pos;
	}
	// calculate skip amount, rewinding if needed for back seek when reading 
	if(offset < 0) {
		if(state->mode != GZ_READ)  /* writing -- can't go backwards */
			return -1;
		offset += state->x.pos;
		if(offset < 0)              /* before start of file! */
			return -1;
		if(gzrewind(file) == -1)    /* rewind, then skip to offset */
			return -1;
	}
	// if reading, skip what's in output buffer (one less gzgetc() check) 
	if(state->mode == GZ_READ) {
		n = GT_OFF(state->x.have) || (z_off64_t)state->x.have > offset ? (uint)offset : state->x.have;
		state->x.have -= n;
		state->x.next += n;
		state->x.pos += n;
		offset -= n;
	}
	// request skip (if not zero) 
	if(offset) {
		state->seek = 1;
		state->skip = offset;
	}
	return state->x.pos + offset;
}

z_off_t ZEXPORT gzseek(gzFile file, z_off_t offset, int whence)
{
	z_off64_t ret = gzseek64(file, (z_off64_t)offset, whence);
	return ret == (z_off_t)ret ? (z_off_t)ret : -1;
}

z_off64_t ZEXPORT gztell64(gzFile file)
{
	gz_state * state;
	// get internal structure and check integrity 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return -1;
	// return position 
	return state->x.pos + (state->seek ? state->skip : 0);
}

z_off_t ZEXPORT gztell(gzFile file)
{
	z_off64_t ret = gztell64(file);
	return (ret == (z_off_t)ret) ? (z_off_t)ret : -1;
}

z_off64_t ZEXPORT gzoffset64(gzFile file)
{
	z_off64_t offset;
	gz_state * state;
	// get internal structure and check integrity 
	if(file == NULL)
		return -1;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return -1;
	// compute and return effective offset in file 
	offset = LSEEK(state->fd, 0, SEEK_CUR);
	if(offset == -1)
		return -1;
	if(state->mode == GZ_READ)          /* reading */
		offset -= state->strm.avail_in;  /* don't count buffered input */
	return offset;
}

z_off_t ZEXPORT gzoffset(gzFile file)
{
	z_off64_t ret = gzoffset64(file);
	return ret == (z_off_t)ret ? (z_off_t)ret : -1;
}

int ZEXPORT gzeof(gzFile file)
{
	gz_state * state;
	// get internal structure and check integrity 
	if(file == NULL)
		return 0;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return 0;
	// return end-of-file state 
	return (state->mode == GZ_READ) ? state->past : 0;
}

const char * ZEXPORT gzerror(gzFile file, int * errnum)
{
	gz_state * state;
	// get internal structure and check integrity 
	if(file == NULL)
		return NULL;
	state = (gz_state *)file;
	if(state->mode != GZ_READ && state->mode != GZ_WRITE)
		return NULL;
	// return error information 
	ASSIGN_PTR(errnum, state->err);
	return (state->err == Z_MEM_ERROR) ? "out of memory" : (state->msg == NULL ? "" : state->msg);
}

void ZEXPORT gzclearerr(gzFile file)
{
	if(file) { 
		gz_state * state = reinterpret_cast<gz_state *>(file); // get internal structure and check integrity 
		if(state->mode == GZ_READ || state->mode == GZ_WRITE) {
			// clear error and end-of-file 
			if(state->mode == GZ_READ) {
				state->eof = 0;
				state->past = 0;
			}
			gz_error(state, Z_OK, 0);
		}
	}
}
// 
// Create an error message in allocated memory and set state->err and
// state->msg accordingly.  Free any previous error message already there.  Do
// not try to free or allocate space if the error is Z_MEM_ERROR (out of
// memory).  Simply save the error message as a static string.  If there is an
// allocation failure constructing the error message, then convert the error to out of memory. 
// 
void ZLIB_INTERNAL FASTCALL gz_error(gz_state * state, int err, const char * msg)
{
	// free previously allocated message and clear 
	if(state->msg != NULL) {
		if(state->err != Z_MEM_ERROR)
			SAlloc::F(state->msg);
		state->msg = NULL;
	}
	// if fatal, set state->x.have to 0 so that the gzgetc() macro fails 
	if(err != Z_OK && err != Z_BUF_ERROR)
		state->x.have = 0;
	// set error code, and if no message, then done 
	state->err = err;
	if(msg == NULL)
		return;
	// for an out of memory error, return literal string when requested 
	if(err == Z_MEM_ERROR)
		return;
	// construct error message with path 
	if((state->msg = static_cast<char *>(SAlloc::M(strlen(state->path) + strlen(msg) + 3))) == NULL) {
		state->err = Z_MEM_ERROR;
		return;
	}
#if !defined(NO_snprintf) && !defined(NO_vsnprintf)
	(void)snprintf(state->msg, strlen(state->path) + strlen(msg) + 3, "%s%s%s", state->path, ": ", msg);
#else
	strcpy(state->msg, state->path);
	strcat(state->msg, ": ");
	strcat(state->msg, msg);
#endif
}

#ifndef INT_MAX
//
// portably return maximum value for an int (when limits.h presumed not
// available) -- we need to do this to cover cases where 2's complement not
// used, since C standard permits 1's complement and sign-bit representations,
// otherwise we could just use (static_cast<uint>(-1)) >> 1 
//
uint ZLIB_INTERNAL gz_intmax()
{
	uint q;
	uint p = 1;
	do {
		q = p;
		p <<= 1;
		p++;
	} while(p > q);
	return q >> 1;
}

#endif
//
// INFBACK
//
// inflate using a call-back interface
// Copyright (C) 1995-2016 Mark Adler
// For conditions of distribution and use, see copyright notice in zlib.h
// 
// This code is largely copied from inflate.c.  Normally either infback.o or
// inflate.o would be linked into an application--not both.  The interface
// with inffast.c is retained so that optimized assembler-coded versions of
// inflate_fast() can be used with either inflate.c or infback.c.
// 
// 
// strm provides memory allocation functions in zalloc and zfree, or
// Z_NULL to use the library memory allocation functions.
// 
// windowBits is in the range 8..15, and window is a user-supplied
// window and output buffer that is 2**windowBits bytes.
// 
int ZEXPORT inflateBackInit_(z_streamp strm, int windowBits, uchar  * window, const char * version, int stream_size)
{
	struct inflate_state * state;
	if(version == Z_NULL || version[0] != ZLIB_VERSION[0] || stream_size != (int)(sizeof(z_stream)))
		return Z_VERSION_ERROR;
	if(strm == Z_NULL || window == Z_NULL || windowBits < 8 || windowBits > 15)
		return Z_STREAM_ERROR;
	strm->msg = Z_NULL;             /* in case we return an error */
	if(strm->zalloc == (alloc_func)0) {
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zalloc = zcalloc;
		strm->opaque = (void *)0;
#endif
	}
	if(strm->zfree == (free_func)0)
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zfree = zcfree;
#endif
	state = static_cast<struct inflate_state *>(ZLIB_ALLOC(strm, 1, sizeof(struct inflate_state)));
	if(state == Z_NULL) 
		return Z_MEM_ERROR;
	Tracev((stderr, "inflate: allocated\n"));
	strm->state = reinterpret_cast<struct internal_state *>(state);
	state->dmax = 32768U;
	state->wbits = (uInt)windowBits;
	state->wsize = 1U << windowBits;
	state->window = window;
	state->wnext = 0;
	state->whave = 0;
	return Z_OK;
}
// 
// Return state with length and distance decoding tables and index sizes set to
// fixed code decoding.  Normally this returns fixed tables from inffixed.h.
// If BUILDFIXED is defined, then instead this routine builds the tables the
// first time it's called, and returns those tables the first time and
// thereafter.  This reduces the size of the code by about 2K bytes, in
// exchange for a little execution time.  However, BUILDFIXED should not be
// used for threaded applications, since the rewriting of the tables and virgin may not be thread-safe.
// 
static void FASTCALL fixedtables(struct inflate_state * state)
{
#ifdef BUILDFIXED
	static int virgin = 1;
	static code * lenfix, * distfix;
	static code fixed[544];
	// build fixed huffman tables if first call (may not be thread safe) 
	if(virgin) {
		uint   bits;
		static code * next;
		// literal/length table 
		uint   sym = 0;
		while(sym < 144) state->lens[sym++] = 8;
		while(sym < 256) state->lens[sym++] = 9;
		while(sym < 280) state->lens[sym++] = 7;
		while(sym < 288) state->lens[sym++] = 8;
		next = fixed;
		lenfix = next;
		bits = 9;
		inflate_table(LENS, state->lens, 288, &(next), &(bits), state->work);
		// distance table 
		sym = 0;
		while(sym < 32) 
			state->lens[sym++] = 5;
		distfix = next;
		bits = 5;
		inflate_table(DISTS, state->lens, 32, &(next), &(bits), state->work);
		// do this just once 
		virgin = 0;
	}
#else /* !BUILDFIXED */
	#include "inffixed.h"
#endif /* BUILDFIXED */
	state->lencode = lenfix;
	state->lenbits = 9;
	state->distcode = distfix;
	state->distbits = 5;
}
// 
// Macros for inflateBack()
// 
// Load returned state from inflate_fast() 
// 
#define LOAD() \
	do { \
		put = strm->next_out; \
		left = strm->avail_out;	\
		next = strm->next_in; \
		have = strm->avail_in; \
		hold = state->hold; \
		bits = state->bits; \
	} while(0)
// 
// Set state from registers for inflate_fast() 
// 
#define RESTORE() \
	do { \
		strm->next_out = put; \
		strm->avail_out = left;	\
		strm->next_in = next; \
		strm->avail_in = have; \
		state->hold = hold; \
		state->bits = bits; \
	} while(0)
// 
// Clear the input bit accumulator 
// 
#define INITBITS() do { hold = 0; bits = 0; } while(0)
// 
// Assure that some input is available.  If input is requested, but denied,
// then return a Z_BUF_ERROR from inflateBack()
// 
#define PULL() \
	do { \
		if(have == 0) {	\
			have = in(in_desc, &next); \
			if(have == 0) {	\
				next = Z_NULL; \
				ret = Z_BUF_ERROR; \
				goto inf_leave;	\
			} \
		} \
	} while(0)
// 
// Get a byte of input into the bit accumulator, or return from inflateBack()
// with an error if there is no input available
// 
#define PULLBYTE() \
	do { \
		PULL();	\
		have--;	\
		hold += static_cast<ulong>(*next++) << bits; \
		bits += 8; \
	} while(0)
// 
// Assure that there are at least n bits in the bit accumulator.  If there is
// not enough available input to do that, then return from inflateBack() with an error
// 
#define NEEDBITS(n) do { while(bits < static_cast<uint>(n)) PULLBYTE(); } while(0)
//
// Return the low n bits of the bit accumulator (n < 16) 
//
#define BITS(n)	((uint)hold & ((1U << (n)) - 1))
//
// Remove n bits from the bit accumulator 
//
#define DROPBITS(n) do { hold >>= (n); bits -= (uint)(n); } while(0)
//
// Remove zero to seven bits as needed to go to a byte boundary 
//
#define BYTEBITS() do { hold >>= bits & 7; bits -= bits & 7; } while(0)
// 
// Assure that some output space is available, by writing out the window
// if it's full.  If the write fails, return from inflateBack() with a Z_BUF_ERROR. 
// 
#define ROOM() \
	do { \
		if(left == 0) {	\
			put = state->window; \
			left = state->wsize; \
			state->whave = left; \
			if(out(out_desc, put, left)) { \
				ret = Z_BUF_ERROR; \
				goto inf_leave;	\
			} \
		} \
	} while(0)
/*
   strm provides the memory allocation functions and window buffer on input,
   and provides information on the unused input on return.  For Z_DATA_ERROR
   returns, strm will also provide an error message.

   in() and out() are the call-back input and output functions.  When
   inflateBack() needs more input, it calls in().  When inflateBack() has
   filled the window with output, or when it completes with data in the
   window, it calls out() to write out the data.  The application must not
   change the provided input until in() is called again or inflateBack()
   returns.  The application must not change the window/output buffer until
   inflateBack() returns.

   in() and out() are called with a descriptor parameter provided in the
   inflateBack() call.  This parameter can be a structure that provides the
   information required to do the read or write, as well as accumulated
   information on the input and output such as totals and check values.

   in() should return zero on failure.  out() should return non-zero on
   failure.  If either in() or out() fails, than inflateBack() returns a
   Z_BUF_ERROR.  strm->next_in can be checked for Z_NULL to see whether it
   was in() or out() that caused in the error.  Otherwise,  inflateBack()
   returns Z_STREAM_END on success, Z_DATA_ERROR for an deflate format
   error, or Z_MEM_ERROR if it could not allocate memory for the state.
   inflateBack() can also return Z_STREAM_ERROR if the input parameters
   are not correct, i.e. strm is Z_NULL or the state was not initialized.
 */
int ZEXPORT inflateBack(z_streamp strm, in_func in, void  * in_desc, out_func out, void  * out_desc)
{
	struct inflate_state * state;
	const uchar * next; /* next input */
	uchar  * put; /* next output */
	uint have, left;    /* available input and output */
	ulong  hold;     /* bit buffer */
	uint bits;          /* bits in bit buffer */
	uint copy;          /* number of stored or match bytes to copy */
	uchar  * from; /* where to copy match bytes from */
	ZInfTreesCode here; // current decoding table entry 
	ZInfTreesCode last; // parent table entry 
	uint len;           /* length to copy for repeats, bits to drop */
	int ret;                /* return code */
	static const ushort order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 }; // permutation of code lengths 
	// Check that the strm exists and that the state was initialized 
	if(strm == Z_NULL || strm->state == Z_NULL)
		return Z_STREAM_ERROR;
	state = reinterpret_cast<struct inflate_state *>(strm->state);
	//
	// Reset the state 
	//
	strm->msg = Z_NULL;
	state->mode = TYPE;
	state->last = 0;
	state->whave = 0;
	next = strm->next_in;
	have = (next != Z_NULL) ? strm->avail_in : 0;
	hold = 0;
	bits = 0;
	put = state->window;
	left = state->wsize;
	// Inflate until end of block marked as last 
	for(;; )
		switch(state->mode) {
			case TYPE: // determine and dispatch block type 
			    if(state->last) {
				    BYTEBITS();
				    state->mode = DONE;
				    break;
			    }
			    NEEDBITS(3);
			    state->last = BITS(1);
			    DROPBITS(1);
			    switch(BITS(2)) {
				    case 0:     /* stored block */
						Tracev((stderr, "inflate:     stored block%s\n", state->last ? " (last)" : ""));
						state->mode = STORED;
						break;
				    case 1:     /* fixed block */
						fixedtables(state);
						Tracev((stderr, "inflate:     fixed codes block%s\n", state->last ? " (last)" : ""));
						state->mode = LEN; /* decode codes */
						break;
				    case 2:     /* dynamic block */
						Tracev((stderr, "inflate:     dynamic codes block%s\n", state->last ? " (last)" : ""));
						state->mode = TABLE;
						break;
				    case 3:
						strm->msg = "invalid block type";
						state->mode = BAD;
			    }
			    DROPBITS(2);
			    break;
			case STORED: // get and verify stored block length 
			    BYTEBITS(); // go to byte boundary 
			    NEEDBITS(32);
			    if((hold & 0xffff) != ((hold >> 16) ^ 0xffff)) {
				    strm->msg = "invalid stored block lengths";
				    state->mode = BAD;
				    break;
			    }
			    state->length = (uint)hold & 0xffff;
			    Tracev((stderr, "inflate:       stored length %u\n", state->length));
			    INITBITS();
			    // copy stored block from input to output 
			    while(state->length != 0) {
				    copy = state->length;
				    PULL();
				    ROOM();
					SETMIN(copy, have);
					SETMIN(copy, left);
				    memcpy(put, next, copy);
				    have -= copy;
				    next += copy;
				    left -= copy;
				    put += copy;
				    state->length -= copy;
			    }
			    Tracev((stderr, "inflate:       stored end\n"));
			    state->mode = TYPE;
			    break;
			case TABLE: // get dynamic table entries descriptor 
			    NEEDBITS(14);
			    state->nlen = BITS(5) + 257;
			    DROPBITS(5);
			    state->ndist = BITS(5) + 1;
			    DROPBITS(5);
			    state->ncode = BITS(4) + 4;
			    DROPBITS(4);
#ifndef PKZIP_BUG_WORKAROUND
			    if(state->nlen > 286 || state->ndist > 30) {
				    strm->msg = "too many length or distance symbols";
				    state->mode = BAD;
				    break;
			    }
#endif
			    Tracev((stderr, "inflate:       table sizes ok\n"));
			    // get code length code lengths (not a typo) 
			    state->have = 0;
			    while(state->have < state->ncode) {
				    NEEDBITS(3);
				    state->lens[order[state->have++]] = (ushort)BITS(3);
				    DROPBITS(3);
			    }
			    while(state->have < 19)
				    state->lens[order[state->have++]] = 0;
			    state->next = state->codes;
			    state->lencode = (ZInfTreesCode const *)(state->next);
			    state->lenbits = 7;
			    ret = inflate_table(CODES, state->lens, 19, &(state->next), &(state->lenbits), state->work);
			    if(ret) {
				    strm->msg = "invalid code lengths set";
				    state->mode = BAD;
				    break;
			    }
			    Tracev((stderr, "inflate:       code lengths ok\n"));
			    // get length and distance code code lengths 
			    state->have = 0;
			    while(state->have < state->nlen + state->ndist) {
				    for(;; ) {
					    here = state->lencode[BITS(state->lenbits)];
					    if((uint)(here.bits) <= bits) 
							break;
					    PULLBYTE();
				    }
				    if(here.val < 16) {
					    DROPBITS(here.bits);
					    state->lens[state->have++] = here.val;
				    }
				    else {
					    if(here.val == 16) {
						    NEEDBITS(here.bits + 2);
						    DROPBITS(here.bits);
						    if(state->have == 0) {
							    strm->msg = "invalid bit length repeat";
							    state->mode = BAD;
							    break;
						    }
						    len = (uint)(state->lens[state->have - 1]);
						    copy = 3 + BITS(2);
						    DROPBITS(2);
					    }
					    else if(here.val == 17) {
						    NEEDBITS(here.bits + 3);
						    DROPBITS(here.bits);
						    len = 0;
						    copy = 3 + BITS(3);
						    DROPBITS(3);
					    }
					    else {
						    NEEDBITS(here.bits + 7);
						    DROPBITS(here.bits);
						    len = 0;
						    copy = 11 + BITS(7);
						    DROPBITS(7);
					    }
					    if(state->have + copy > state->nlen + state->ndist) {
						    strm->msg = "invalid bit length repeat";
						    state->mode = BAD;
						    break;
					    }
					    while(copy--)
						    state->lens[state->have++] = (ushort)len;
				    }
			    }
			    // handle error breaks in while 
			    if(state->mode == BAD) 
					break;
			    // check for end-of-block code (better have one) 
			    if(state->lens[256] == 0) {
				    strm->msg = "invalid code -- missing end-of-block";
				    state->mode = BAD;
				    break;
			    }
			    // build code tables -- note: do not change the lenbits or distbits
			    // values here (9 and 6) without reading the comments in inftrees.h
			    // concerning the ENOUGH constants, which depend on those values 
			    state->next = state->codes;
			    state->lencode = (ZInfTreesCode const *)(state->next);
			    state->lenbits = 9;
			    ret = inflate_table(LENS, state->lens, state->nlen, &(state->next), &(state->lenbits), state->work);
			    if(ret) {
				    strm->msg = "invalid literal/lengths set";
				    state->mode = BAD;
				    break;
			    }
			    state->distcode = state->next;
			    state->distbits = 6;
			    ret = inflate_table(DISTS, state->lens + state->nlen, state->ndist, &(state->next), &(state->distbits), state->work);
			    if(ret) {
				    strm->msg = "invalid distances set";
				    state->mode = BAD;
				    break;
			    }
			    Tracev((stderr, "inflate:       codes ok\n"));
			    state->mode = LEN;
			case LEN:
			    // use inflate_fast() if we have enough input and output 
			    if(have >= 6 && left >= 258) {
				    RESTORE();
				    if(state->whave < state->wsize)
					    state->whave = state->wsize - left;
				    inflate_fast(strm, state->wsize);
				    LOAD();
				    break;
			    }
			    // get a literal, length, or end-of-block code 
			    for(;; ) {
				    here = state->lencode[BITS(state->lenbits)];
				    if((uint)(here.bits) <= bits) 
						break;
				    PULLBYTE();
			    }
			    if(here.op && (here.op & 0xf0) == 0) {
				    last = here;
				    for(;; ) {
					    here = state->lencode[last.val + (BITS(last.bits + last.op) >> last.bits)];
					    if((uint)(last.bits + here.bits) <= bits) 
							break;
					    PULLBYTE();
				    }
				    DROPBITS(last.bits);
			    }
			    DROPBITS(here.bits);
			    state->length = (uint)here.val;
			    // process literal 
			    if(here.op == 0) {
				    Tracevv((stderr, here.val >= 0x20 && here.val < 0x7f ? "inflate:         literal '%c'\n" : "inflate:         literal 0x%02x\n", here.val));
				    ROOM();
				    *put++ = (uchar)(state->length);
				    left--;
				    state->mode = LEN;
				    break;
			    }
			    // process end of block 
			    if(here.op & 32) {
				    Tracevv((stderr, "inflate:         end of block\n"));
				    state->mode = TYPE;
				    break;
			    }
			    // invalid code 
			    if(here.op & 64) {
				    strm->msg = "invalid literal/length code";
				    state->mode = BAD;
				    break;
			    }
			    // length code -- get extra bits, if any 
			    state->extra = (uint)(here.op) & 15;
			    if(state->extra != 0) {
				    NEEDBITS(state->extra);
				    state->length += BITS(state->extra);
				    DROPBITS(state->extra);
			    }
			    Tracevv((stderr, "inflate:         length %u\n", state->length));
			    // get distance code 
			    for(;; ) {
				    here = state->distcode[BITS(state->distbits)];
				    if((uint)(here.bits) <= bits) 
						break;
				    PULLBYTE();
			    }
			    if((here.op & 0xf0) == 0) {
				    last = here;
				    for(;; ) {
					    here = state->distcode[last.val + (BITS(last.bits + last.op) >> last.bits)];
					    if((uint)(last.bits + here.bits) <= bits) 
							break;
					    PULLBYTE();
				    }
				    DROPBITS(last.bits);
			    }
			    DROPBITS(here.bits);
			    if(here.op & 64) {
				    strm->msg = "invalid distance code";
				    state->mode = BAD;
				    break;
			    }
			    state->offset = (uint)here.val;
			    // get distance extra bits, if any 
			    state->extra = (uint)(here.op) & 15;
			    if(state->extra != 0) {
				    NEEDBITS(state->extra);
				    state->offset += BITS(state->extra);
				    DROPBITS(state->extra);
			    }
			    if(state->offset > state->wsize - (state->whave < state->wsize ? left : 0)) {
				    strm->msg = "invalid distance too far back";
				    state->mode = BAD;
				    break;
			    }
			    Tracevv((stderr, "inflate:         distance %u\n", state->offset));
			    // copy match from window to output 
			    do {
				    ROOM();
				    copy = state->wsize - state->offset;
				    if(copy < left) {
					    from = put + copy;
					    copy = left - copy;
				    }
				    else {
					    from = put - state->offset;
					    copy = left;
				    }
				    if(copy > state->length) copy = state->length;
				    state->length -= copy;
				    left -= copy;
				    do {
					    *put++ = *from++;
				    } while(--copy);
			    } while(state->length != 0);
			    break;
			case DONE: // inflate stream terminated properly -- write leftover output 
			    ret = Z_STREAM_END;
			    if(left < state->wsize) {
				    if(out(out_desc, state->window, state->wsize - left))
					    ret = Z_BUF_ERROR;
			    }
			    goto inf_leave;
			case BAD:
			    ret = Z_DATA_ERROR;
			    goto inf_leave;
			default: // can't happen, but makes compilers happy 
			    ret = Z_STREAM_ERROR;
			    goto inf_leave;
		}
	// Return unused input 
inf_leave:
	strm->next_in = next;
	strm->avail_in = have;
	return ret;
}

int ZEXPORT inflateBackEnd(z_streamp strm)
{
	if(strm == Z_NULL || strm->state == Z_NULL || strm->zfree == (free_func)0)
		return Z_STREAM_ERROR;
	else {
		ZLIB_FREE(strm, strm->state);
		strm->state = Z_NULL;
		Tracev((stderr, "inflate: end\n"));
		return Z_OK;
	}
}
//
// DEFLATE -- compress data using the deflation algorithm
// Copyright (C) 1995-2017 Jean-loup Gailly and Mark Adler
// 
/*
 *  ALGORITHM
 *
 * The "deflation" process depends on being able to identify portions
 * of the input text which are identical to earlier input (within a
 * sliding window trailing behind the input currently being processed).
 *
 * The most straightforward technique turns out to be the fastest for
 * most input files: try all possible matches and select the longest.
 * The key feature of this algorithm is that insertions into the string
 * dictionary are very simple and thus fast, and deletions are avoided
 * completely. Insertions are performed at each input character, whereas
 * string matches are performed only when the previous match ends. So it
 * is preferable to spend more time in matches to allow very fast string
 * insertions and avoid deletions. The matching algorithm for small
 * strings is inspired from that of Rabin & Karp. A brute force approach
 * is used to find longer strings when a small match has been found.
 * A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 * (by Leonid Broukhis).
 *    A previous version of this file used a more sophisticated algorithm
 * (by Fiala and Greene) which is guaranteed to run in linear amortized
 * time, but has a larger average cost, uses more memory and is patented.
 * However the F&G algorithm may be faster for some highly redundant
 * files if the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 * The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 * I found it in 'freeze' written by Leonid Broukhis.
 * Thanks to many people for bug reports and testing.
 *
 *  REFERENCES
 *
 * Deutsch, L.P.,"DEFLATE Compressed Data Format Specification".
 * Available in http://tools.ietf.org/html/rfc1951
 *
 * A description of the Rabin and Karp algorithm is given in the book "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 * Fiala,E.R., and Greene,D.H. Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 */
const char deflate_copyright[] = " deflate 1.2.11 Copyright 1995-2017 Jean-loup Gailly and Mark Adler ";
// 
// If you use the zlib library in a product, an acknowledgment is welcome
// in the documentation of your product. If for some reason you cannot
// include such an acknowledgment, I would appreciate that you keep this
// copyright string in the executable of your product.
// 
// Local data
// 
#define NIL 0 // Tail of hash chains 
#ifndef TOO_FAR
	#define TOO_FAR 4096
#endif
// Matches of length 3 are discarded if their distance exceeds TOO_FAR 
// 
// Values for max_lazy_match, good_match and max_chain_length, depending on
// the desired pack level (0..9). The values given below have been tuned to
// exclude worst case performance for pathological files. Better values may be
// found for specific files.
// 
typedef struct config_s {
	ushort good_length; /* reduce lazy search above this match length */
	ushort max_lazy; /* do not perform lazy search above this match length */
	ushort nice_length; /* quit search above this match length */
	ushort max_chain;
	compress_func func;
} config;

#ifdef FASTEST
static const config configuration_table[2] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, deflate_stored},  /* store only */
/* 1 */ {4,    4,  8,    4, deflate_fast}
};                                          /* max speed, no lazy matches */
#else
static const config configuration_table[10] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, deflate_stored},  /* store only */
/* 1 */ {4,    4,  8,    4, deflate_fast}, /* max speed, no lazy matches */
/* 2 */ {4,    5, 16,    8, deflate_fast},
/* 3 */ {4,    6, 32,   32, deflate_fast},

/* 4 */ {4,    4, 16,   16, deflate_slow},  /* lazy matches */
/* 5 */ {8,   16, 32,   32, deflate_slow},
/* 6 */ {8,   16, 128, 128, deflate_slow},
/* 7 */ {8,   32, 128, 256, deflate_slow},
/* 8 */ {32, 128, 258, 1024, deflate_slow},
/* 9 */ {32, 258, 258, 4096, deflate_slow}
};                                           /* max compression */
#endif
// 
// Note: the deflate() code requires max_lazy >= MIN_MATCH and max_chain >= 4
// For deflate_fast() (levels <= 3) good is ignored and lazy has a different meaning.
// 
// rank Z_BLOCK between Z_NO_FLUSH and Z_PARTIAL_FLUSH 
#define RANK(f) (((f) * 2) - ((f) > 4 ? 9 : 0))
// 
// Update a hash value with the given input byte
// IN  assertion: all calls to UPDATE_HASH are made with consecutive input
//   characters, so that a running hash key can be computed from the previous
//   key instead of complete recalculation each time.
// 
#define UPDATE_HASH(s, h, c) (h = (((h)<<s->hash_shift) ^ (c)) & s->hash_mask)

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * If this file is compiled with -DFASTEST, the compression level is forced
 * to 1, and no hash chains are maintained.
 * IN  assertion: all calls to INSERT_STRING are made with consecutive input
 *  characters and the first MIN_MATCH bytes of str are valid (except for
 *  the last MIN_MATCH-1 bytes of the input file).
 */
#ifdef FASTEST
#define INSERT_STRING(s, str, match_head) \
	(UPDATE_HASH(s, s->ins_h, s->window[(str) + (MIN_MATCH-1)]), match_head = s->head[s->ins_h], s->head[s->ins_h] = (Pos)(str))
#else
#define INSERT_STRING(s, str, match_head) \
	(UPDATE_HASH(s, s->ins_h, s->window[(str) + (MIN_MATCH-1)]), match_head = s->prev[(str) & s->w_mask] = s->head[s->ins_h], s->head[s->ins_h] = (Pos)(str))
#endif
// 
// Initialize the hash table (avoiding 64K overflow for 16 bit systems).
// prev[] will be initialized on the fly.
// 
#define CLEAR_HASH(s) s->head[s->hash_size-1] = NIL; memzero(s->head, static_cast<uint>(s->hash_size-1)*sizeof(*s->head));
// 
// Slide the hash table when sliding the window down (could be avoided with 32
// bit values at the expense of memory usage). We slide even when level == 0 to
// keep the hash table consistent if we switch back to level > 0 later.
// 
static void FASTCALL slide_hash(deflate_state * s)
{
	uint m;
	uInt wsize = s->w_size;
	uint n = s->hash_size;
	Posf * p = &s->head[n];
	do {
		m = *--p;
		*p = static_cast<Pos>(m >= wsize ? m - wsize : NIL);
	} while(--n);
	n = wsize;
#ifndef FASTEST
	p = &s->prev[n];
	do {
		m = *--p;
		*p = static_cast<Pos>(m >= wsize ? m - wsize : NIL);
		// If n is not on any hash chain, prev[n] is garbage but its value will never be used.
	} while(--n);
#endif
}

int ZEXPORT deflateInit_(z_streamp strm, int level, const char * version, int stream_size)
{
	return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, version, stream_size);
	/* To do: ignore strm->next_in if we use it as window */
}

int ZEXPORT deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy, const char * version, int stream_size)
{
	deflate_state * s;
	int wrap = 1;
	static const char my_version[] = ZLIB_VERSION;
	ushort * overlay;
	// We overlay pending_buf and d_buf+l_buf. This works since the average
	// output size for (length,distance) codes is <= 24 bits.
	if(version == Z_NULL || version[0] != my_version[0] || stream_size != sizeof(z_stream)) {
		return Z_VERSION_ERROR;
	}
	if(strm == Z_NULL) 
		return Z_STREAM_ERROR;
	strm->msg = Z_NULL;
	if(strm->zalloc == (alloc_func)0) {
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zalloc = zcalloc;
		strm->opaque = (void *)0;
#endif
	}
	if(strm->zfree == (free_func)0)
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zfree = zcfree;
#endif
#ifdef FASTEST
	if(level != 0) 
		level = 1;
#else
	if(level == Z_DEFAULT_COMPRESSION) 
		level = 6;
#endif
	if(windowBits < 0) { /* suppress zlib wrapper */
		wrap = 0;
		windowBits = -windowBits;
	}
#ifdef GZIP
	else if(windowBits > 15) {
		wrap = 2; /* write gzip wrapper instead */
		windowBits -= 16;
	}
#endif
	if(memLevel < 1 || memLevel > MAX_MEM_LEVEL || method != Z_DEFLATED || windowBits < 8 || windowBits > 15 || level < 0 || level > 9 ||
	    strategy < 0 || strategy > Z_FIXED || (windowBits == 8 && wrap != 1)) {
		return Z_STREAM_ERROR;
	}
	if(windowBits == 8) 
		windowBits = 9;  /* until 256-byte window bug fixed */
	s = (deflate_state*)ZLIB_ALLOC(strm, 1, sizeof(deflate_state));
	if(s == Z_NULL) 
		return Z_MEM_ERROR;
	strm->state = (struct internal_state *)(s);
	s->strm = strm;
	s->status = INIT_STATE; /* to pass state test in deflateReset() */
	s->wrap = wrap;
	s->gzhead = Z_NULL;
	s->w_bits = (uInt)windowBits;
	s->w_size = 1 << s->w_bits;
	s->w_mask = s->w_size - 1;
	s->hash_bits = (uInt)memLevel + 7;
	s->hash_size = 1 << s->hash_bits;
	s->hash_mask = s->hash_size - 1;
	s->hash_shift =  ((s->hash_bits+MIN_MATCH-1)/MIN_MATCH);
	s->window = static_cast<Bytef *>(ZLIB_ALLOC(strm, s->w_size, 2*sizeof(Byte)));
	s->prev   = static_cast<Posf *>(ZLIB_ALLOC(strm, s->w_size, sizeof(Pos)));
	s->head   = static_cast<Posf *>(ZLIB_ALLOC(strm, s->hash_size, sizeof(Pos)));
	s->high_water = 0;  /* nothing written to s->window yet */
	s->lit_bufsize = 1 << (memLevel + 6); /* 16K elements by default */
	overlay = static_cast<ushort *>(ZLIB_ALLOC(strm, s->lit_bufsize, sizeof(ushort)+2));
	s->pending_buf = reinterpret_cast<uchar *>(overlay);
	s->pending_buf_size = (ulong)s->lit_bufsize * (sizeof(ushort)+2L);
	if(s->window == Z_NULL || s->prev == Z_NULL || s->head == Z_NULL ||
	    s->pending_buf == Z_NULL) {
		s->status = FINISH_STATE;
		strm->msg = ERR_MSG(Z_MEM_ERROR);
		deflateEnd(strm);
		return Z_MEM_ERROR;
	}
	s->d_buf = overlay + s->lit_bufsize/sizeof(ushort);
	s->l_buf = s->pending_buf + (1+sizeof(ushort))*s->lit_bufsize;
	s->level = level;
	s->strategy = strategy;
	s->method = (Byte)method;
	return deflateReset(strm);
}
// 
// Check for a valid deflate stream state. Return 0 if ok, 1 if not.
// 
static int FASTCALL deflateStateCheck(z_streamp strm)
{
	deflate_state * s;
	if(!strm || strm->zalloc == (alloc_func)0 || strm->zfree == (free_func)0)
		return 1;
	s = strm->state;
	if(!s || s->strm != strm || (
#ifdef GZIP
		    s->status != GZIP_STATE &&
#endif
		    !oneof7(s->status, INIT_STATE, EXTRA_STATE, NAME_STATE, COMMENT_STATE, HCRC_STATE, BUSY_STATE, FINISH_STATE)))
		return 1;
	return 0;
}

int ZEXPORT deflateSetDictionary(z_streamp strm, const Bytef * dictionary, uInt dictLength)
{
	deflate_state * s;
	uInt str, n;
	int wrap;
	uint avail;
	const uchar * next;
	if(deflateStateCheck(strm) || dictionary == Z_NULL)
		return Z_STREAM_ERROR;
	s = strm->state;
	wrap = s->wrap;
	if(wrap == 2 || (wrap == 1 && s->status != INIT_STATE) || s->lookahead)
		return Z_STREAM_ERROR;
	// when using zlib wrappers, compute Adler-32 for provided dictionary 
	if(wrap == 1)
		strm->adler = adler32(strm->adler, dictionary, dictLength);
	s->wrap = 0; // avoid computing Adler-32 in read_buf 
	// if dictionary would fill window, just replace the history 
	if(dictLength >= s->w_size) {
		if(wrap == 0) { // already empty otherwise 
			CLEAR_HASH(s);
			s->strstart = 0;
			s->block_start = 0L;
			s->insert = 0;
		}
		dictionary += dictLength - s->w_size; // use the tail 
		dictLength = s->w_size;
	}
	// insert dictionary into window and hash 
	avail = strm->avail_in;
	next = strm->next_in;
	strm->avail_in = dictLength;
	strm->next_in = dictionary;
	fill_window(s);
	while(s->lookahead >= MIN_MATCH) {
		str = s->strstart;
		n = s->lookahead - (MIN_MATCH-1);
		do {
			UPDATE_HASH(s, s->ins_h, s->window[str + MIN_MATCH-1]);
#ifndef FASTEST
			s->prev[str & s->w_mask] = s->head[s->ins_h];
#endif
			s->head[s->ins_h] = (Pos)str;
			str++;
		} while(--n);
		s->strstart = str;
		s->lookahead = MIN_MATCH-1;
		fill_window(s);
	}
	s->strstart += s->lookahead;
	s->block_start = (long)s->strstart;
	s->insert = s->lookahead;
	s->lookahead = 0;
	s->match_length = s->prev_length = MIN_MATCH-1;
	s->match_available = 0;
	strm->next_in = next;
	strm->avail_in = avail;
	s->wrap = wrap;
	return Z_OK;
}

int ZEXPORT deflateGetDictionary(z_streamp strm, Bytef * dictionary, uInt  * dictLength)
{
	if(deflateStateCheck(strm))
		return Z_STREAM_ERROR;
	else {
		deflate_state * s = strm->state;
		uInt len = s->strstart + s->lookahead;
		SETMIN(len, s->w_size);
		if(dictionary && len)
			memcpy(dictionary, s->window + s->strstart + s->lookahead - len, len);
		ASSIGN_PTR(dictLength, len);
		return Z_OK;
	}
}

int ZEXPORT deflateResetKeep(z_streamp strm)
{
	deflate_state * s;
	if(deflateStateCheck(strm)) {
		return Z_STREAM_ERROR;
	}
	strm->total_in = strm->total_out = 0;
	strm->msg = Z_NULL; /* use zfree if we ever allocate msg dynamically */
	strm->data_type = Z_UNKNOWN;
	s = (deflate_state*)strm->state;
	s->pending = 0;
	s->pending_out = s->pending_buf;
	if(s->wrap < 0) {
		s->wrap = -s->wrap; /* was made negative by deflate(..., Z_FINISH); */
	}
	s->status =
#ifdef GZIP
	    s->wrap == 2 ? GZIP_STATE :
#endif
	    s->wrap ? INIT_STATE : BUSY_STATE;
	strm->adler =
#ifdef GZIP
	    s->wrap == 2 ? crc32(0L, Z_NULL, 0) :
#endif
	    adler32(0L, Z_NULL, 0);
	s->last_flush = Z_NO_FLUSH;
	_tr_init(s);
	return Z_OK;
}
// 
// Initialize the "longest match" routines for a new zlib stream
// 
static void lm_init(deflate_state * s)
{
	s->window_size = (ulong)2L*s->w_size;
	CLEAR_HASH(s);
	//
	// Set the default configuration parameters:
	//
	s->max_lazy_match   = configuration_table[s->level].max_lazy;
	s->good_match       = configuration_table[s->level].good_length;
	s->nice_match       = configuration_table[s->level].nice_length;
	s->max_chain_length = configuration_table[s->level].max_chain;
	s->strstart = 0;
	s->block_start = 0L;
	s->lookahead = 0;
	s->insert = 0;
	s->match_length = s->prev_length = MIN_MATCH-1;
	s->match_available = 0;
	s->ins_h = 0;
#ifndef FASTEST
#ifdef ASMV
	match_init(); /* initialize the asm code */
#endif
#endif
}

int ZEXPORT deflateReset(z_streamp strm)
{
	int ret = deflateResetKeep(strm);
	if(ret == Z_OK)
		lm_init(strm->state);
	return ret;
}

int ZEXPORT deflateSetHeader(z_streamp strm, gz_headerp head)
{
	if(deflateStateCheck(strm) || strm->state->wrap != 2)
		return Z_STREAM_ERROR;
	else {
		strm->state->gzhead = head;
		return Z_OK;
	}
}

int ZEXPORT deflatePending(z_streamp strm, uint * pending, int * bits)
{
	if(deflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		ASSIGN_PTR(pending, strm->state->pending);
		ASSIGN_PTR(bits, strm->state->bi_valid);
		return Z_OK;
	}
}

int ZEXPORT deflatePrime(z_streamp strm, int bits, int value)
{
	if(deflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		deflate_state * s = strm->state;
		if((Bytef *)(s->d_buf) < s->pending_out + ((Buf_size + 7) >> 3))
			return Z_BUF_ERROR;
		else {
			do {
				int put = Buf_size - s->bi_valid;
				SETMIN(put, bits);
				s->bi_buf |= (ushort)((value & ((1 << put) - 1)) << s->bi_valid);
				s->bi_valid += put;
				_tr_flush_bits(s);
				value >>= put;
				bits -= put;
			} while(bits);
			return Z_OK;
		}
	}
}

int ZEXPORT deflateParams(z_streamp strm, int level, int strategy)
{
	deflate_state * s;
	compress_func func;
	if(deflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	s = strm->state;
#ifdef FASTEST
	if(level != 0) level = 1;
#else
	if(level == Z_DEFAULT_COMPRESSION) level = 6;
#endif
	if(level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
		return Z_STREAM_ERROR;
	}
	func = configuration_table[s->level].func;
	if((strategy != s->strategy || func != configuration_table[level].func) && s->high_water) {
		// Flush the last buffer: 
		int err = deflate(strm, Z_BLOCK);
		if(err == Z_STREAM_ERROR)
			return err;
		else if(strm->avail_out == 0)
			return Z_BUF_ERROR;
	}
	if(s->level != level) {
		if(s->level == 0 && s->matches != 0) {
			if(s->matches == 1)
				slide_hash(s);
			else
				CLEAR_HASH(s);
			s->matches = 0;
		}
		s->level = level;
		s->max_lazy_match   = configuration_table[level].max_lazy;
		s->good_match       = configuration_table[level].good_length;
		s->nice_match       = configuration_table[level].nice_length;
		s->max_chain_length = configuration_table[level].max_chain;
	}
	s->strategy = strategy;
	return Z_OK;
}

int ZEXPORT deflateTune(z_streamp strm, int good_length, int max_lazy, int nice_length, int max_chain)
{
	if(deflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		deflate_state * s = strm->state;
		s->good_match = (uInt)good_length;
		s->max_lazy_match = (uInt)max_lazy;
		s->nice_match = nice_length;
		s->max_chain_length = (uInt)max_chain;
		return Z_OK;
	}
}
// 
// For the default windowBits of 15 and memLevel of 8, this function returns
// a close to exact, as well as small, upper bound on the compressed size.
// They are coded as constants here for a reason--if the #define's are
// changed, then this function needs to be changed as well.  The return
// value for 15 and 8 only works for those exact settings.
// 
// For any setting other than those defaults for windowBits and memLevel,
// the value returned is a conservative worst case for the maximum expansion
// resulting from using fixed blocks instead of stored blocks, which deflate
// can emit on compressed data for some combinations of the parameters.
// 
// This function could be more sophisticated to provide closer upper bounds for
// every combination of windowBits and memLevel.  But even the conservative
// upper bound of about 14% expansion does not seem onerous for output buffer allocation.
// 
uLong ZEXPORT deflateBound(z_streamp strm, uLong sourceLen)
{
	deflate_state * s;
	uLong wraplen;
	// conservative upper bound for compressed data 
	uLong complen = sourceLen + ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5;
	// if can't get parameters, return conservative bound plus zlib wrapper 
	if(deflateStateCheck(strm))
		return complen + 6;
	// compute wrapper length 
	s = strm->state;
	switch(s->wrap) {
		case 0: // raw deflate 
		    wraplen = 0;
		    break;
		case 1: // zlib wrapper 
		    wraplen = 6 + (s->strstart ? 4 : 0);
		    break;
#ifdef GZIP
		case 2: // gzip wrapper 
		    wraplen = 18;
		    if(s->gzhead) { // user-supplied gzip header 
			    Bytef * str;
			    if(s->gzhead->extra)
				    wraplen += 2 + s->gzhead->extra_len;
			    str = s->gzhead->name;
			    if(str) do {
					wraplen++;
				} while(*str++);
			    str = s->gzhead->comment;
			    if(str) do {
					wraplen++;
				} while(*str++);
			    if(s->gzhead->hcrc)
				    wraplen += 2;
		    }
		    break;
#endif
		default:                    /* for compiler happiness */
		    wraplen = 6;
	}
	/* if not default parameters, return conservative bound */
	if(s->w_bits != 15 || s->hash_bits != 8 + 7)
		return complen + wraplen;
	/* default settings: return tight bound for that case */
	return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) + 13 - 6 + wraplen;
}
// 
// Put a short in the pending buffer. The 16-bit value is put in MSB order.
// IN assertion: the stream state is correct and there is enough room in pending_buf.
// 
static void FASTCALL putShortMSB(deflate_state * s, uInt b)
{
	put_byte(s, (Byte)(b >> 8));
	put_byte(s, (Byte)(b & 0xff));
}
// 
// Flush as much pending output as possible. All deflate() output, except for
// some deflate_stored() output, goes through this function so some
// applications may wish to modify it to avoid allocating a large
// strm->next_out buffer and copying into it. (See also read_buf()).
// 
static void FASTCALL flush_pending(z_streamp strm)
{
	uint len;
	deflate_state * s = strm->state;
	_tr_flush_bits(s);
	len = s->pending;
	if(len > strm->avail_out) 
		len = strm->avail_out;
	if(len) {
		memcpy(strm->next_out, s->pending_out, len);
		strm->next_out  += len;
		s->pending_out  += len;
		strm->total_out += len;
		strm->avail_out -= len;
		s->pending      -= len;
		if(s->pending == 0) {
			s->pending_out = s->pending_buf;
		}
	}
}
// 
// Update the header CRC with the bytes s->pending_buf[beg..s->pending - 1].
// 
#define HCRC_UPDATE(beg) \
	do { \
		if(s->gzhead->hcrc && s->pending > (beg)) \
			strm->adler = crc32(strm->adler, s->pending_buf + (beg), s->pending - (beg)); \
	} while(0)

int ZEXPORT deflate(z_streamp strm, int flush)
{
	int old_flush; // value of flush param for previous deflate call 
	deflate_state * s;
	if(deflateStateCheck(strm) || flush > Z_BLOCK || flush < 0) {
		return Z_STREAM_ERROR;
	}
	s = strm->state;
	if(strm->next_out == Z_NULL || (strm->avail_in != 0 && strm->next_in == Z_NULL) || (s->status == FINISH_STATE && flush != Z_FINISH)) {
		ERR_RETURN(strm, Z_STREAM_ERROR);
	}
	if(strm->avail_out == 0) 
		ERR_RETURN(strm, Z_BUF_ERROR);
	old_flush = s->last_flush;
	s->last_flush = flush;
	// Flush as much pending output as possible 
	if(s->pending != 0) {
		flush_pending(strm);
		if(strm->avail_out == 0) {
			// Since avail_out is 0, deflate will be called again with
			// more output space, but possibly with both pending and
			// avail_in equal to zero. There won't be anything to do,
			// but this is not an error situation so make sure we
			// return OK instead of BUF_ERROR at next call of deflate:
			s->last_flush = -1;
			return Z_OK;
		}
		// Make sure there is something to do and avoid duplicate consecutive
		// flushes. For repeated and useless calls with Z_FINISH, we keep
		// returning Z_STREAM_END instead of Z_BUF_ERROR.
	}
	else if(strm->avail_in == 0 && RANK(flush) <= RANK(old_flush) && flush != Z_FINISH) {
		ERR_RETURN(strm, Z_BUF_ERROR);
	}
	// User must not provide more input after the first FINISH: 
	if(s->status == FINISH_STATE && strm->avail_in != 0) {
		ERR_RETURN(strm, Z_BUF_ERROR);
	}
	// Write the header 
	if(s->status == INIT_STATE) {
		// zlib header 
		uInt header = (Z_DEFLATED + ((s->w_bits-8)<<4)) << 8;
		uInt level_flags;
		if(s->strategy >= Z_HUFFMAN_ONLY || s->level < 2)
			level_flags = 0;
		else if(s->level < 6)
			level_flags = 1;
		else if(s->level == 6)
			level_flags = 2;
		else
			level_flags = 3;
		header |= (level_flags << 6);
		if(s->strstart != 0) 
			header |= PRESET_DICT;
		header += 31 - (header % 31);
		putShortMSB(s, header);
		// Save the adler32 of the preset dictionary: 
		if(s->strstart != 0) {
			putShortMSB(s, (uInt)(strm->adler >> 16));
			putShortMSB(s, (uInt)(strm->adler & 0xffff));
		}
		strm->adler = adler32(0L, 0, 0);
		s->status = BUSY_STATE;
		// Compression must start with an empty pending buffer 
		flush_pending(strm);
		if(s->pending != 0) {
			s->last_flush = -1;
			return Z_OK;
		}
	}
#ifdef GZIP
	if(s->status == GZIP_STATE) {
		// gzip header 
		strm->adler = crc32(0L, 0, 0);
		put_byte(s, 31);
		put_byte(s, 139);
		put_byte(s, 8);
		if(s->gzhead == 0) {
			put_byte(s, 0);
			put_byte(s, 0);
			put_byte(s, 0);
			put_byte(s, 0);
			put_byte(s, 0);
			put_byte(s, s->level == 9 ? 2 : (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
			put_byte(s, OS_CODE);
			s->status = BUSY_STATE;
			// Compression must start with an empty pending buffer 
			flush_pending(strm);
			if(s->pending != 0) {
				s->last_flush = -1;
				return Z_OK;
			}
		}
		else {
			put_byte(s, (s->gzhead->text ? 1 : 0) +
			    (s->gzhead->hcrc ? 2 : 0) + (s->gzhead->extra == Z_NULL ? 0 : 4) +
			    (s->gzhead->name == Z_NULL ? 0 : 8) + (s->gzhead->comment == Z_NULL ? 0 : 16));
			put_byte(s, (Byte)(s->gzhead->time & 0xff));
			put_byte(s, (Byte)((s->gzhead->time >> 8) & 0xff));
			put_byte(s, (Byte)((s->gzhead->time >> 16) & 0xff));
			put_byte(s, (Byte)((s->gzhead->time >> 24) & 0xff));
			put_byte(s, s->level == 9 ? 2 : (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
			put_byte(s, s->gzhead->os & 0xff);
			if(s->gzhead->extra != Z_NULL) {
				put_byte(s, s->gzhead->extra_len & 0xff);
				put_byte(s, (s->gzhead->extra_len >> 8) & 0xff);
			}
			if(s->gzhead->hcrc)
				strm->adler = crc32(strm->adler, s->pending_buf, s->pending);
			s->gzindex = 0;
			s->status = EXTRA_STATE;
		}
	}
	if(s->status == EXTRA_STATE) {
		if(s->gzhead->extra != Z_NULL) {
			ulong beg = s->pending; /* start of bytes to update crc */
			uInt left = (s->gzhead->extra_len & 0xffff) - s->gzindex;
			while(s->pending + left > s->pending_buf_size) {
				uInt copy = s->pending_buf_size - s->pending;
				memcpy(s->pending_buf + s->pending, s->gzhead->extra + s->gzindex, copy);
				s->pending = s->pending_buf_size;
				HCRC_UPDATE(beg);
				s->gzindex += copy;
				flush_pending(strm);
				if(s->pending != 0) {
					s->last_flush = -1;
					return Z_OK;
				}
				beg = 0;
				left -= copy;
			}
			memcpy(s->pending_buf + s->pending, s->gzhead->extra + s->gzindex, left);
			s->pending += left;
			HCRC_UPDATE(beg);
			s->gzindex = 0;
		}
		s->status = NAME_STATE;
	}
	if(s->status == NAME_STATE) {
		if(s->gzhead->name != Z_NULL) {
			ulong beg = s->pending; /* start of bytes to update crc */
			int val;
			do {
				if(s->pending == s->pending_buf_size) {
					HCRC_UPDATE(beg);
					flush_pending(strm);
					if(s->pending != 0) {
						s->last_flush = -1;
						return Z_OK;
					}
					beg = 0;
				}
				val = s->gzhead->name[s->gzindex++];
				put_byte(s, val);
			} while(val != 0);
			HCRC_UPDATE(beg);
			s->gzindex = 0;
		}
		s->status = COMMENT_STATE;
	}
	if(s->status == COMMENT_STATE) {
		if(s->gzhead->comment != Z_NULL) {
			ulong beg = s->pending; /* start of bytes to update crc */
			int val;
			do {
				if(s->pending == s->pending_buf_size) {
					HCRC_UPDATE(beg);
					flush_pending(strm);
					if(s->pending != 0) {
						s->last_flush = -1;
						return Z_OK;
					}
					beg = 0;
				}
				val = s->gzhead->comment[s->gzindex++];
				put_byte(s, val);
			} while(val != 0);
			HCRC_UPDATE(beg);
		}
		s->status = HCRC_STATE;
	}
	if(s->status == HCRC_STATE) {
		if(s->gzhead->hcrc) {
			if(s->pending + 2 > s->pending_buf_size) {
				flush_pending(strm);
				if(s->pending != 0) {
					s->last_flush = -1;
					return Z_OK;
				}
			}
			put_byte(s, (Byte)(strm->adler & 0xff));
			put_byte(s, (Byte)((strm->adler >> 8) & 0xff));
			strm->adler = crc32(0L, Z_NULL, 0);
		}
		s->status = BUSY_STATE;
		// Compression must start with an empty pending buffer 
		flush_pending(strm);
		if(s->pending != 0) {
			s->last_flush = -1;
			return Z_OK;
		}
	}
#endif
	// Start a new block or continue the current one.
	if(strm->avail_in != 0 || s->lookahead != 0 || (flush != Z_NO_FLUSH && s->status != FINISH_STATE)) {
		block_state bstate = s->level == 0 ? deflate_stored(s, flush) :
		    s->strategy == Z_HUFFMAN_ONLY ? deflate_huff(s, flush) :
		    s->strategy == Z_RLE ? deflate_rle(s, flush) : (*(configuration_table[s->level].func))(s, flush);
		if(bstate == finish_started || bstate == finish_done) {
			s->status = FINISH_STATE;
		}
		if(bstate == need_more || bstate == finish_started) {
			if(strm->avail_out == 0) {
				s->last_flush = -1; // avoid BUF_ERROR next call, see above 
			}
			return Z_OK;
			/* If flush != Z_NO_FLUSH && avail_out == 0, the next call
			 * of deflate should use the same flush parameter to make sure
			 * that the flush is complete. So we don't have to output an
			 * empty block here, this will be done at next call. This also
			 * ensures that for a very small output buffer, we emit at most
			 * one empty block.
			 */
		}
		if(bstate == block_done) {
			if(flush == Z_PARTIAL_FLUSH) {
				_tr_align(s);
			}
			else if(flush != Z_BLOCK) { /* FULL_FLUSH or SYNC_FLUSH */
				_tr_stored_block(s, (char *)0, 0L, 0);
				// For a full flush, this empty block will be recognized as a special marker by inflate_sync().
				if(flush == Z_FULL_FLUSH) {
					CLEAR_HASH(s); /* forget history */
					if(s->lookahead == 0) {
						s->strstart = 0;
						s->block_start = 0L;
						s->insert = 0;
					}
				}
			}
			flush_pending(strm);
			if(strm->avail_out == 0) {
				s->last_flush = -1; /* avoid BUF_ERROR at next call, see above */
				return Z_OK;
			}
		}
	}

	if(flush != Z_FINISH) return Z_OK;
	if(s->wrap <= 0) return Z_STREAM_END;

	/* Write the trailer */
#ifdef GZIP
	if(s->wrap == 2) {
		put_byte(s, (Byte)(strm->adler & 0xff));
		put_byte(s, (Byte)((strm->adler >> 8) & 0xff));
		put_byte(s, (Byte)((strm->adler >> 16) & 0xff));
		put_byte(s, (Byte)((strm->adler >> 24) & 0xff));
		put_byte(s, (Byte)(strm->total_in & 0xff));
		put_byte(s, (Byte)((strm->total_in >> 8) & 0xff));
		put_byte(s, (Byte)((strm->total_in >> 16) & 0xff));
		put_byte(s, (Byte)((strm->total_in >> 24) & 0xff));
	}
	else
#endif
	{
		putShortMSB(s, (uInt)(strm->adler >> 16));
		putShortMSB(s, (uInt)(strm->adler & 0xffff));
	}
	flush_pending(strm);
	//
	// If avail_out is zero, the application will call deflate again to flush the rest.
	//
	if(s->wrap > 0) 
		s->wrap = -s->wrap; // write the trailer only once! 
	return s->pending != 0 ? Z_OK : Z_STREAM_END;
}

int ZEXPORT deflateEnd(z_streamp strm)
{
	if(deflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		int status = strm->state->status;
		/* Deallocate in reverse order of allocations: */
		TRY_FREE(strm, strm->state->pending_buf);
		TRY_FREE(strm, strm->state->head);
		TRY_FREE(strm, strm->state->prev);
		TRY_FREE(strm, strm->state->window);
		ZLIB_FREE(strm, strm->state);
		strm->state = Z_NULL;
		return (status == BUSY_STATE) ? Z_DATA_ERROR : Z_OK;
	}
}
//
// Copy the source state to the destination state.
// To simplify the source, this is not supported for 16-bit MSDOS (which
// doesn't have enough memory anyway to duplicate compression states).
//
int ZEXPORT deflateCopy(z_streamp dest, z_streamp source)
{
#ifdef MAXSEG_64K
	return Z_STREAM_ERROR;
#else
	deflate_state * ds;
	deflate_state * ss;
	ushort * overlay;
	if(deflateStateCheck(source) || dest == Z_NULL) {
		return Z_STREAM_ERROR;
	}
	ss = source->state;
	memcpy((void *)dest, (void *)source, sizeof(z_stream));
	ds = (deflate_state*)ZLIB_ALLOC(dest, 1, sizeof(deflate_state));
	if(ds == Z_NULL) return Z_MEM_ERROR;
	dest->state = (struct internal_state *)ds;
	memcpy((void *)ds, (void *)ss, sizeof(deflate_state));
	ds->strm = dest;

	ds->window = static_cast<Bytef *>(ZLIB_ALLOC(dest, ds->w_size, 2*sizeof(Byte)));
	ds->prev   = (Posf*)ZLIB_ALLOC(dest, ds->w_size, sizeof(Pos));
	ds->head   = (Posf*)ZLIB_ALLOC(dest, ds->hash_size, sizeof(Pos));
	overlay = (ushort *)ZLIB_ALLOC(dest, ds->lit_bufsize, sizeof(ushort)+2);
	ds->pending_buf = (uchar *)overlay;
	if(ds->window == Z_NULL || ds->prev == Z_NULL || ds->head == Z_NULL ||
	    ds->pending_buf == Z_NULL) {
		deflateEnd(dest);
		return Z_MEM_ERROR;
	}
	/* following memcpy do not work for 16-bit MSDOS */
	memcpy(ds->window, ss->window, ds->w_size * 2 * sizeof(Byte));
	memcpy((void *)ds->prev, (void *)ss->prev, ds->w_size * sizeof(Pos));
	memcpy((void *)ds->head, (void *)ss->head, ds->hash_size * sizeof(Pos));
	memcpy(ds->pending_buf, ss->pending_buf, (uInt)ds->pending_buf_size);

	ds->pending_out = ds->pending_buf + (ss->pending_out - ss->pending_buf);
	ds->d_buf = overlay + ds->lit_bufsize/sizeof(ushort);
	ds->l_buf = ds->pending_buf + (1+sizeof(ushort))*ds->lit_bufsize;

	ds->l_desc.dyn_tree = ds->dyn_ltree;
	ds->d_desc.dyn_tree = ds->dyn_dtree;
	ds->bl_desc.dyn_tree = ds->bl_tree;

	return Z_OK;
#endif /* MAXSEG_64K */
}
// 
// Read a new buffer from the current input stream, update the adler32
// and total number of bytes read.  All deflate() input goes through
// this function so some applications may wish to modify it to avoid
// allocating a large strm->next_in buffer and copying from it.
// (See also flush_pending()).
// 
static uint read_buf(z_streamp strm, Bytef * buf, uint size)
{
	uint len = strm->avail_in;
	SETMIN(len, size);
	if(len) {
		strm->avail_in  -= len;
		memcpy(buf, strm->next_in, len);
		if(strm->state->wrap == 1) {
			strm->adler = adler32(strm->adler, buf, len);
		}
#ifdef GZIP
		else if(strm->state->wrap == 2) {
			strm->adler = crc32(strm->adler, buf, len);
		}
#endif
		strm->next_in  += len;
		strm->total_in += len;
	}
	return len;
}

#ifndef FASTEST
// 
// Set match_start to the longest match starting at the given string and
// return its length. Matches shorter or equal to prev_length are discarded,
// in which case the result is equal to prev_length and match_start is garbage.
// IN assertions: cur_match is the head of the hash chain for the current
//   string (strstart) and its distance is <= MAX_DIST, and prev_length >= 1
// OUT assertion: the match length is not greater than s->lookahead.
// 
#ifndef ASMV
// For 80x86 and 680x0, an optimized version will be provided in match.asm or
// match.S. The code will be functionally equivalent.
// 
static uInt FASTCALL longest_match(deflate_state * s, IPos cur_match)
{
	uint chain_length = s->max_chain_length; /* max hash chain length */
	Bytef * scan = s->window + s->strstart; /* current string */
	Bytef * match;                 /* matched string */
	int len;                       /* length of current match */
	int best_len = (int)s->prev_length;     /* best match length so far */
	int nice_match = s->nice_match;         /* stop if match long enough */
	IPos limit = s->strstart > (IPos)MAX_DIST(s) ? s->strstart - (IPos)MAX_DIST(s) : NIL;
	// Stop when cur_match becomes <= limit. To simplify the code,
	// we prevent matches with the string of window index 0.
	Posf * prev = s->prev;
	uInt wmask = s->w_mask;
#ifdef UNALIGNED_OK
	// Compare two bytes at a time. Note: this is not always beneficial.
	// Try with and without -DUNALIGNED_OK to check.
	Bytef * strend = s->window + s->strstart + MAX_MATCH - 1;
	ushort scan_start = *(ushort *)scan;
	ushort scan_end   = *(ushort *)(scan+best_len-1);
#else
	Bytef * strend = s->window + s->strstart + MAX_MATCH;
	Byte scan_end1  = scan[best_len-1];
	Byte scan_end   = scan[best_len];
#endif
	// The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple of 16.
	// It is easy to get rid of this optimization if necessary.
	Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");
	// Do not waste too much time if we already have a good match: */
	if(s->prev_length >= s->good_match) {
		chain_length >>= 2;
	}
	// Do not look for matches beyond the end of the input. This is necessary
	// to make deflate deterministic.
	if((uInt)nice_match > s->lookahead) 
		nice_match = (int)s->lookahead;
	Assert((ulong)s->strstart <= s->window_size-MIN_LOOKAHEAD, "need lookahead");
	do {
		Assert(cur_match < s->strstart, "no future");
		match = s->window + cur_match;
		// Skip to next match if the match length cannot increase
		// or if the match length is less than 2.  Note that the checks below
		// for insufficient lookahead only occur occasionally for performance
		// reasons.  Therefore uninitialized memory will be accessed, and
		// conditional jumps will be made that depend on those values.
		// However the length of the match is limited to the lookahead, so
		// the output of deflate is not affected by the uninitialized values.
#if (defined(UNALIGNED_OK) && MAX_MATCH == 258)
		/* This code assumes sizeof(ushort) == 2. Do not use
		 * UNALIGNED_OK if your compiler uses a different size.
		 */
		if(*(ushort *)(match+best_len-1) != scan_end || *(ushort *)match != scan_start) 
			continue;
		/* It is not necessary to compare scan[2] and match[2] since they are
		 * always equal when the other bytes match, given that the hash keys
		 * are equal and that HASH_BITS >= 8. Compare 2 bytes at a time at
		 * strstart+3, +5, ... up to strstart+257. We check for insufficient
		 * lookahead only every 4th comparison; the 128th check will be made
		 * at strstart+257. If MAX_MATCH-2 is not a multiple of 8, it is
		 * necessary to put more guard bytes at the end of the window, or
		 * to check more often for insufficient lookahead.
		 */
		Assert(scan[2] == match[2], "scan[2]?");
		scan++, match++;
		do {
		} while(*(ushort *)(scan += 2) == *(ushort *)(match += 2) && *(ushort *)(scan += 2) == *(ushort *)(match += 2) &&
		    *(ushort *)(scan += 2) == *(ushort *)(match += 2) && *(ushort *)(scan += 2) == *(ushort *)(match += 2) && scan < strend);
		/* The funny "do {}" generates better code on most compilers */

		/* Here, scan <= window+strstart+257 */
		Assert(scan <= s->window+(uint)(s->window_size-1), "wild scan");
		if(*scan == *match) 
			scan++;
		len = (MAX_MATCH - 1) - (int)(strend-scan);
		scan = strend - (MAX_MATCH-1);
#else /* UNALIGNED_OK */
		if(match[best_len] != scan_end  || match[best_len-1] != scan_end1 || *match != *scan || *++match != scan[1]) 
			continue;
		// The check at best_len-1 can be removed because it will be made
		// again later. (This heuristic is not always a win.)
		// It is not necessary to compare scan[2] and match[2] since they
		// are always equal when the other bytes match, given that
		// the hash keys are equal and that HASH_BITS >= 8.
		scan += 2, match++;
		Assert(*scan == *match, "match[2]?");
		// We check for insufficient lookahead only every 8th comparison;
		// the 256th check will be made at strstart+258.
		do {
		} while(*++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match &&
		    *++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match && scan < strend);
		Assert(scan <= s->window+(uint)(s->window_size-1), "wild scan");
		len = MAX_MATCH - (int)(strend - scan);
		scan = strend - MAX_MATCH;
#endif /* UNALIGNED_OK */
		if(len > best_len) {
			s->match_start = cur_match;
			best_len = len;
			if(len >= nice_match) 
				break;
#ifdef UNALIGNED_OK
			scan_end = *(ushort *)(scan+best_len-1);
#else
			scan_end1  = scan[best_len-1];
			scan_end   = scan[best_len];
#endif
		}
	} while((cur_match = prev[cur_match & wmask]) > limit && --chain_length != 0);
	if((uInt)best_len <= s->lookahead) 
		return (uInt)best_len;
	return s->lookahead;
}

#endif /* ASMV */
#else /* FASTEST */
// 
// Optimized version for FASTEST only
// 
static uInt FASTCALL longest_match(deflate_state * s, IPos cur_match)
{
	Bytef * scan = s->window + s->strstart; /* current string */
	Bytef * match;                  /* matched string */
	int len;                       /* length of current match */
	Bytef * strend = s->window + s->strstart + MAX_MATCH;
	/* The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple of 16.
	 * It is easy to get rid of this optimization if necessary.
	 */
	Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");
	Assert((ulong)s->strstart <= s->window_size-MIN_LOOKAHEAD, "need lookahead");
	Assert(cur_match < s->strstart, "no future");
	match = s->window + cur_match;
	/* Return failure if the match length is less than 2:
	 */
	if(match[0] != scan[0] || match[1] != scan[1]) return MIN_MATCH-1;
	/* The check at best_len-1 can be removed because it will be made
	 * again later. (This heuristic is not always a win.)
	 * It is not necessary to compare scan[2] and match[2] since they
	 * are always equal when the other bytes match, given that
	 * the hash keys are equal and that HASH_BITS >= 8.
	 */
	scan += 2, match += 2;
	Assert(*scan == *match, "match[2]?");

	/* We check for insufficient lookahead only every 8th comparison;
	 * the 256th check will be made at strstart+258.
	 */
	do {
	} while(*++scan == *++match && *++scan == *++match &&
	    *++scan == *++match && *++scan == *++match &&
	    *++scan == *++match && *++scan == *++match &&
	    *++scan == *++match && *++scan == *++match &&
	    scan < strend);
	Assert(scan <= s->window+(uint)(s->window_size-1), "wild scan");
	len = MAX_MATCH - (int)(strend - scan);
	if(len < MIN_MATCH) 
		return MIN_MATCH - 1;
	s->match_start = cur_match;
	return (uInt)len <= s->lookahead ? (uInt)len : s->lookahead;
}

#endif /* FASTEST */

#ifdef ZLIB_DEBUG

#define EQUAL 0
/* result of memcmp for equal strings */
// 
// Check that the match at match_start is indeed a match.
// 
static void check_match(deflate_state * s, IPos start, IPos match, int length)
{
	/* check that the match is indeed a match */
	if(zmemcmp(s->window + match, s->window + start, length) != EQUAL) {
		fprintf(stderr, " start %u, match %u, length %d\n", start, match, length);
		do {
			fprintf(stderr, "%c%c", s->window[match++], s->window[start++]);
		} while(--length != 0);
		z_error("invalid match");
	}
	if(z_verbose > 1) {
		fprintf(stderr, "\\[%d,%d]", start-match, length);
		do { putc(s->window[start++], stderr); } while(--length != 0);
	}
}
#else
#define check_match(s, start, match, length)
#endif /* ZLIB_DEBUG */
// 
// Fill the window when the lookahead becomes insufficient.
// Updates strstart and lookahead.
// 
// IN assertion: lookahead < MIN_LOOKAHEAD
// OUT assertions: strstart <= window_size-MIN_LOOKAHEAD
//   At least one byte has been read, or avail_in == 0; reads are
//   performed for at least two bytes (required for the zip translate_eol
//   option -- not supported here).
// 
static void FASTCALL fill_window(deflate_state * s)
{
	uint n;
	uint more; /* Amount of free space at the end of the window. */
	uInt wsize = s->w_size;
	Assert(s->lookahead < MIN_LOOKAHEAD, "already enough lookahead");
	do {
		more = (uint)(s->window_size -(ulong)s->lookahead -(ulong)s->strstart);
		/* Deal with !@#$% 64K limit: */
		if(sizeof(int) <= 2) {
			if(more == 0 && s->strstart == 0 && s->lookahead == 0) {
				more = wsize;
			}
			else if(more == (uint)(-1)) {
				/* Very unlikely, but possible on 16 bit machine if
				 * strstart == 0 && lookahead == 1 (input done a byte at time)
				 */
				more--;
			}
		}
		/* If the window is almost full and there is insufficient lookahead,
		 * move the upper half to the lower one to make room in the upper half.
		 */
		if(s->strstart >= wsize+MAX_DIST(s)) {
			memcpy(s->window, s->window+wsize, (uint)wsize - more);
			s->match_start -= wsize;
			s->strstart    -= wsize; /* we now have strstart >= MAX_DIST */
			s->block_start -= (long)wsize;
			slide_hash(s);
			more += wsize;
		}
		if(s->strm->avail_in == 0) break;

		/* If there was no sliding:
		 *  strstart <= WSIZE+MAX_DIST-1 && lookahead <= MIN_LOOKAHEAD - 1 &&
		 *  more == window_size - lookahead - strstart
		 * => more >= window_size - (MIN_LOOKAHEAD-1 + WSIZE + MAX_DIST-1)
		 * => more >= window_size - 2*WSIZE + 2
		 * In the BIG_MEM or MMAP case (not yet supported),
		 * window_size == input_size + MIN_LOOKAHEAD  &&
		 * strstart + s->lookahead <= input_size => more >= MIN_LOOKAHEAD.
		 * Otherwise, window_size == 2*WSIZE so more >= 2.
		 * If there was sliding, more >= WSIZE. So in all cases, more >= 2.
		 */
		Assert(more >= 2, "more < 2");
		n = read_buf(s->strm, s->window + s->strstart + s->lookahead, more);
		s->lookahead += n;
		/* Initialize the hash value now that we have some input: */
		if(s->lookahead + s->insert >= MIN_MATCH) {
			uInt str = s->strstart - s->insert;
			s->ins_h = s->window[str];
			UPDATE_HASH(s, s->ins_h, s->window[str + 1]);
#if MIN_MATCH != 3
			Call UPDATE_HASH() MIN_MATCH-3 more times
#endif
			while(s->insert) {
				UPDATE_HASH(s, s->ins_h, s->window[str + MIN_MATCH-1]);
#ifndef FASTEST
				s->prev[str & s->w_mask] = s->head[s->ins_h];
#endif
				s->head[s->ins_h] = (Pos)str;
				str++;
				s->insert--;
				if(s->lookahead + s->insert < MIN_MATCH)
					break;
			}
		}
		/* If the whole input has less than MIN_MATCH bytes, ins_h is garbage,
		 * but this is not important since only literal bytes will be emitted.
		 */
	} while(s->lookahead < MIN_LOOKAHEAD && s->strm->avail_in != 0);

	/* If the WIN_INIT bytes after the end of the current data have never been
	 * written, then zero those bytes in order to avoid memory check reports of
	 * the use of uninitialized (or uninitialised as Julian writes) bytes by
	 * the longest match routines.  Update the high water mark for the next
	 * time through here.  WIN_INIT is set to MAX_MATCH since the longest match
	 * routines allow scanning to strstart + MAX_MATCH, ignoring lookahead.
	 */
	if(s->high_water < s->window_size) {
		ulong curr = s->strstart + (ulong)(s->lookahead);
		ulong init;
		if(s->high_water < curr) {
			/* Previous high water mark below current data -- zero WIN_INIT
			 * bytes or up to end of window, whichever is less.
			 */
			init = s->window_size - curr;
			SETMIN(init, WIN_INIT);
			memzero(s->window + curr, (uint)init);
			s->high_water = curr + init;
		}
		else if(s->high_water < (ulong)curr + WIN_INIT) {
			/* High water mark at or above current data, but below current data
			 * plus WIN_INIT -- zero out to current data plus WIN_INIT, or up
			 * to end of window, whichever is less.
			 */
			init = (ulong)curr + WIN_INIT - s->high_water;
			SETMIN(init, s->window_size - s->high_water);
			memzero(s->window + s->high_water, (uint)init);
			s->high_water += init;
		}
	}
	Assert((ulong)s->strstart <= s->window_size - MIN_LOOKAHEAD, "not enough room for search");
}

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, last) { \
	_tr_flush_block(s, (s->block_start >= 0L ? (charf*)&s->window[(uint)s->block_start] : (charf*)0), \
	    (ulong)((long)s->strstart - s->block_start), (last)); \
	s->block_start = s->strstart; \
	flush_pending(s->strm);	\
	Tracev((stderr, "[FLUSH]")); \
}

// Same but force premature exit if necessary
#define FLUSH_BLOCK(s, last) { FLUSH_BLOCK_ONLY(s, last); if(s->strm->avail_out == 0) return (last) ? finish_started : need_more; }
#define MAX_STORED 65535 // Maximum stored block length in deflate format (not including header)

/* Minimum of a and b. */
// @sobolev #define MIN(a, b) ((a) > (b) ? (b) : (a))

/* ===========================================================================
 * Copy without compression as much as possible from the input stream, return
 * the current block state.
 *
 * In case deflateParams() is used to later switch to a non-zero compression
 * level, s->matches (otherwise unused when storing) keeps track of the number
 * of hash table slides to perform. If s->matches is 1, then one hash table
 * slide will be done when switching. If s->matches is 2, the maximum value
 * allowed here, then the hash table will be cleared, since two or more slides
 * is the same as a clear.
 *
 * deflate_stored() is written to minimize the number of times an input byte is
 * copied. It is most efficient with large input and output buffers, which
 * maximizes the opportunites to have a single copy from next_in to next_out.
 */
static block_state deflate_stored(deflate_state * s, int flush)
{
	/* Smallest worthy block size when not flushing or finishing. By default
	 * this is 32K. This can be as small as 507 bytes for memLevel == 1. For
	 * large input and output buffers, the stored block size will be larger.
	 */
	uint min_block = MIN(s->pending_buf_size - 5, s->w_size);
	/* Copy as many min_block or larger stored blocks directly to next_out as
	 * possible. If flushing, copy the remaining available input to next_out as
	 * stored blocks, if there is enough space.
	 */
	uint len;
	uint left;
	uint have;
	uint last = 0;
	uint used = s->strm->avail_in;
	do {
		/* Set len to the maximum size block that we can copy directly with the
		 * available input data and output space. Set left to how much of that
		 * would be copied from what's left in the window.
		 */
		len = MAX_STORED; /* maximum deflate stored block length */
		have = (s->bi_valid + 42) >> 3; /* number of header bytes */
		if(s->strm->avail_out < have)   /* need room for header */
			break;
		/* maximum stored block length that will fit in avail_out: */
		have = s->strm->avail_out - have;
		left = s->strstart - s->block_start; /* bytes left in window */
		if(len > (ulong)left + s->strm->avail_in)
			len = left + s->strm->avail_in;  /* limit len to the input */
		SETMIN(len, have); // limit len to the output
		/* If the stored block would be less than min_block in length, or if
		 * unable to copy all of the available input when flushing, then try
		 * copying to the window and the pending buffer instead. Also don't
		 * write an empty block when flushing -- deflate() does that.
		 */
		if(len < min_block && ((len == 0 && flush != Z_FINISH) || flush == Z_NO_FLUSH || len != left + s->strm->avail_in))
			break;

		/* Make a dummy stored block in pending to get the header bytes,
		 * including any pending bits. This also updates the debugging counts.
		 */
		last = flush == Z_FINISH && len == left + s->strm->avail_in ? 1 : 0;
		_tr_stored_block(s, (char *)0, 0L, last);
		/* Replace the lengths in the dummy stored block with len. */
		s->pending_buf[s->pending - 4] = len;
		s->pending_buf[s->pending - 3] = len >> 8;
		s->pending_buf[s->pending - 2] = ~len;
		s->pending_buf[s->pending - 1] = ~len >> 8;
		/* Write the stored block header bytes. */
		flush_pending(s->strm);
#ifdef ZLIB_DEBUG
		/* Update debugging counts for the data about to be copied. */
		s->compressed_len += len << 3;
		s->bits_sent += len << 3;
#endif
		// Copy uncompressed bytes from the window to next_out
		if(left) {
			SETMIN(left, len);
			memcpy(s->strm->next_out, s->window + s->block_start, left);
			s->strm->next_out += left;
			s->strm->avail_out -= left;
			s->strm->total_out += left;
			s->block_start += left;
			len -= left;
		}
		// Copy uncompressed bytes directly from next_in to next_out, updating the check value.
		if(len) {
			read_buf(s->strm, s->strm->next_out, len);
			s->strm->next_out += len;
			s->strm->avail_out -= len;
			s->strm->total_out += len;
		}
	} while(last == 0);

	/* Update the sliding window with the last s->w_size bytes of the copied
	 * data, or append all of the copied data to the existing window if less
	 * than s->w_size bytes were copied. Also update the number of bytes to
	 * insert in the hash tables, in the event that deflateParams() switches to
	 * a non-zero compression level.
	 */
	used -= s->strm->avail_in;  /* number of input bytes directly copied */
	if(used) {
		/* If any input was used, then no unused input remains in the window,
		 * therefore s->block_start == s->strstart.
		 */
		if(used >= s->w_size) { /* supplant the previous history */
			s->matches = 2; /* clear hash */
			memcpy(s->window, s->strm->next_in - s->w_size, s->w_size);
			s->strstart = s->w_size;
		}
		else {
			if(s->window_size - s->strstart <= used) {
				// Slide the window down. 
				s->strstart -= s->w_size;
				memcpy(s->window, s->window + s->w_size, s->strstart);
				if(s->matches < 2)
					s->matches++; // add a pending slide_hash() 
			}
			memcpy(s->window + s->strstart, s->strm->next_in - used, used);
			s->strstart += used;
		}
		s->block_start = s->strstart;
		s->insert += MIN(used, s->w_size - s->insert);
	}
	SETMAX(s->high_water, s->strstart);
	// If the last block was written to next_out, then done
	if(last)
		return finish_done;
	// If flushing and all input has been consumed, then done. 
	if(flush != Z_NO_FLUSH && flush != Z_FINISH && s->strm->avail_in == 0 && (long)s->strstart == s->block_start)
		return block_done;
	// Fill the window with any remaining input
	have = s->window_size - s->strstart - 1;
	if(s->strm->avail_in > have && s->block_start >= (long)s->w_size) {
		// Slide the window down
		s->block_start -= s->w_size;
		s->strstart -= s->w_size;
		memcpy(s->window, s->window + s->w_size, s->strstart);
		if(s->matches < 2)
			s->matches++;  // add a pending slide_hash() 
		have += s->w_size; // more space now 
	}
	SETMIN(have, s->strm->avail_in);
	if(have) {
		read_buf(s->strm, s->window + s->strstart, have);
		s->strstart += have;
	}
	SETMAX(s->high_water, s->strstart);
	/* There was not enough avail_out to write a complete worthy or flushed
	 * stored block to next_out. Write a stored block to pending instead, if we
	 * have enough input for a worthy block, or if flushing and there is enough
	 * room for the remaining input as a stored block in the pending buffer.
	 */
	have = (s->bi_valid + 42) >> 3;     /* number of header bytes */
	// maximum stored block length that will fit in pending: 
	have = MIN(s->pending_buf_size - have, MAX_STORED);
	min_block = MIN(have, s->w_size);
	left = s->strstart - s->block_start;
	if(left >= min_block || ((left || flush == Z_FINISH) && flush != Z_NO_FLUSH && s->strm->avail_in == 0 && left <= have)) {
		len = MIN(left, have);
		last = (flush == Z_FINISH && s->strm->avail_in == 0 && len == left) ? 1 : 0;
		_tr_stored_block(s, (charf*)s->window + s->block_start, len, last);
		s->block_start += len;
		flush_pending(s->strm);
	}
	// We've done all we can with the available input and output
	return last ? finish_started : need_more;
}
// 
// Compress as much as possible from the input stream, return the current block state.
// This function does not perform lazy evaluation of matches and inserts
// new strings in the dictionary only for unmatched strings or for short
// matches. It is used only for the fast compression options.
// 
static block_state deflate_fast(deflate_state * s, int flush)
{
	IPos hash_head;   /* head of the hash chain */
	int bflush;       /* set if current block must be flushed */
	for(;; ) {
		/* Make sure that we always have enough lookahead, except
		 * at the end of the input file. We need MAX_MATCH bytes
		 * for the next match, plus MIN_MATCH bytes to insert the
		 * string following the next match.
		 */
		if(s->lookahead < MIN_LOOKAHEAD) {
			fill_window(s);
			if(s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
				return need_more;
			}
			if(s->lookahead == 0) // flush the current block 
				break; 
		}
		// Insert the string window[strstart .. strstart+2] in the
		// dictionary, and set hash_head to the head of the hash chain:
		hash_head = NIL;
		if(s->lookahead >= MIN_MATCH) {
			INSERT_STRING(s, s->strstart, hash_head);
		}
		// Find the longest match, discarding those <= prev_length.
		// At this point we have always match_length < MIN_MATCH
		if(hash_head != NIL && s->strstart - hash_head <= MAX_DIST(s)) {
			// To simplify the code, we prevent matches with the string
			// of window index 0 (in particular we have to avoid a match
			// of the string with itself at the start of the input file).
			s->match_length = longest_match(s, hash_head);
			// longest_match() sets match_start 
		}
		if(s->match_length >= MIN_MATCH) {
			check_match(s, s->strstart, s->match_start, s->match_length);
			_tr_tally_dist(s, s->strstart - s->match_start, s->match_length - MIN_MATCH, bflush);
			s->lookahead -= s->match_length;
			// Insert new strings in the hash table only if the match length
			// is not too large. This saves time but degrades compression.
#ifndef FASTEST
			if(s->match_length <= s->max_insert_length && s->lookahead >= MIN_MATCH) {
				s->match_length--; /* string at strstart already in table */
				do {
					s->strstart++;
					INSERT_STRING(s, s->strstart, hash_head);
					// strstart never exceeds WSIZE-MAX_MATCH, so there are always MIN_MATCH bytes ahead.
				} while(--s->match_length != 0);
				s->strstart++;
			}
			else
#endif
			{
				s->strstart += s->match_length;
				s->match_length = 0;
				s->ins_h = s->window[s->strstart];
				UPDATE_HASH(s, s->ins_h, s->window[s->strstart+1]);
#if MIN_MATCH != 3
				Call UPDATE_HASH() MIN_MATCH-3 more times
#endif
				// If lookahead < MIN_MATCH, ins_h is garbage, but it does not
				// matter since it will be recomputed at next deflate call.
			}
		}
		else {
			// No match, output a literal byte 
			Tracevv((stderr, "%c", s->window[s->strstart]));
			_tr_tally_lit(s, s->window[s->strstart], bflush);
			s->lookahead--;
			s->strstart++;
		}
		if(bflush) 
			FLUSH_BLOCK(s, 0);
	}
	s->insert = s->strstart < MIN_MATCH-1 ? s->strstart : MIN_MATCH-1;
	if(flush == Z_FINISH) {
		FLUSH_BLOCK(s, 1);
		return finish_done;
	}
	else {
		if(s->last_lit)
			FLUSH_BLOCK(s, 0);
		return block_done;
	}
}

#ifndef FASTEST
// 
// Same as above, but achieves better compression. We use a lazy
// evaluation for matches: a match is finally adopted only if there is
// no better match at the next window position.
// 
static block_state deflate_slow(deflate_state * s, int flush)
{
	IPos hash_head;      /* head of hash chain */
	int bflush;          /* set if current block must be flushed */
	/* Process the input block. */
	for(;; ) {
		/* Make sure that we always have enough lookahead, except
		 * at the end of the input file. We need MAX_MATCH bytes
		 * for the next match, plus MIN_MATCH bytes to insert the
		 * string following the next match.
		 */
		if(s->lookahead < MIN_LOOKAHEAD) {
			fill_window(s);
			if(s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
				return need_more;
			}
			if(s->lookahead == 0) break;  /* flush the current block */
		}

		/* Insert the string window[strstart .. strstart+2] in the
		 * dictionary, and set hash_head to the head of the hash chain:
		 */
		hash_head = NIL;
		if(s->lookahead >= MIN_MATCH) {
			INSERT_STRING(s, s->strstart, hash_head);
		}
		// Find the longest match, discarding those <= prev_length.
		s->prev_length = s->match_length, s->prev_match = s->match_start;
		s->match_length = MIN_MATCH-1;
		if(hash_head != NIL && s->prev_length < s->max_lazy_match && s->strstart - hash_head <= MAX_DIST(s)) {
			/* To simplify the code, we prevent matches with the string
			 * of window index 0 (in particular we have to avoid a match
			 * of the string with itself at the start of the input file).
			 */
			s->match_length = longest_match(s, hash_head);
			/* longest_match() sets match_start */

			if(s->match_length <= 5 && (s->strategy == Z_FILTERED
#if TOO_FAR <= 32767
				    || (s->match_length == MIN_MATCH && s->strstart - s->match_start > TOO_FAR)
#endif
				    )) {
				/* If prev_match is also MIN_MATCH, match_start is garbage
				 * but we will ignore the current match anyway.
				 */
				s->match_length = MIN_MATCH-1;
			}
		}
		/* If there was a match at the previous step and the current
		 * match is not better, output the previous match:
		 */
		if(s->prev_length >= MIN_MATCH && s->match_length <= s->prev_length) {
			uInt max_insert = s->strstart + s->lookahead - MIN_MATCH;
			/* Do not insert strings in hash table beyond this. */
			check_match(s, s->strstart-1, s->prev_match, s->prev_length);
			_tr_tally_dist(s, s->strstart -1 - s->prev_match, s->prev_length - MIN_MATCH, bflush);
			/* Insert in hash table all strings up to the end of the match.
			 * strstart-1 and strstart are already inserted. If there is not
			 * enough lookahead, the last two strings are not inserted in
			 * the hash table.
			 */
			s->lookahead -= s->prev_length-1;
			s->prev_length -= 2;
			do {
				if(++s->strstart <= max_insert) {
					INSERT_STRING(s, s->strstart, hash_head);
				}
			} while(--s->prev_length != 0);
			s->match_available = 0;
			s->match_length = MIN_MATCH-1;
			s->strstart++;
			if(bflush) 
				FLUSH_BLOCK(s, 0);
		}
		else if(s->match_available) {
			/* If there was no match at the previous position, output a
			 * single literal. If there was a match but the current match
			 * is longer, truncate the previous match to a single literal.
			 */
			Tracevv((stderr, "%c", s->window[s->strstart-1]));
			_tr_tally_lit(s, s->window[s->strstart-1], bflush);
			if(bflush) {
				FLUSH_BLOCK_ONLY(s, 0);
			}
			s->strstart++;
			s->lookahead--;
			if(s->strm->avail_out == 0) return need_more;
		}
		else {
			/* There is no previous match to compare with, wait for
			 * the next step to decide.
			 */
			s->match_available = 1;
			s->strstart++;
			s->lookahead--;
		}
	}
	Assert(flush != Z_NO_FLUSH, "no flush?");
	if(s->match_available) {
		Tracevv((stderr, "%c", s->window[s->strstart-1]));
		_tr_tally_lit(s, s->window[s->strstart-1], bflush);
		s->match_available = 0;
	}
	s->insert = s->strstart < MIN_MATCH-1 ? s->strstart : MIN_MATCH-1;
	if(flush == Z_FINISH) {
		FLUSH_BLOCK(s, 1);
		return finish_done;
	}
	if(s->last_lit)
		FLUSH_BLOCK(s, 0);
	return block_done;
}

#endif /* FASTEST */
// 
// For Z_RLE, simply look for runs of bytes, generate matches only of distance
// one.  Do not maintain a hash table.  (It will be regenerated if this run of
// deflate switches away from Z_RLE.)
// 
static block_state FASTCALL deflate_rle(deflate_state * s, int flush)
{
	int bflush;         /* set if current block must be flushed */
	uInt prev;          /* byte at distance one to match */
	Bytef * scan, * strend; /* scan goes up to strend for length of run */
	for(;; ) {
		/* Make sure that we always have enough lookahead, except
		 * at the end of the input file. We need MAX_MATCH bytes
		 * for the longest run, plus one for the unrolled loop.
		 */
		if(s->lookahead <= MAX_MATCH) {
			fill_window(s);
			if(s->lookahead <= MAX_MATCH && flush == Z_NO_FLUSH) {
				return need_more;
			}
			if(s->lookahead == 0) 
				break; // flush the current block 
		}
		// See how many times the previous byte repeats 
		s->match_length = 0;
		if(s->lookahead >= MIN_MATCH && s->strstart > 0) {
			scan = s->window + s->strstart - 1;
			prev = *scan;
			if(prev == *++scan && prev == *++scan && prev == *++scan) {
				strend = s->window + s->strstart + MAX_MATCH;
				do {
				} while(prev == *++scan && prev == *++scan && prev == *++scan && prev == *++scan &&
				    prev == *++scan && prev == *++scan && prev == *++scan && prev == *++scan && scan < strend);
				s->match_length = MAX_MATCH - (uInt)(strend - scan);
				if(s->match_length > s->lookahead)
					s->match_length = s->lookahead;
			}
			Assert(scan <= s->window+(uInt)(s->window_size-1), "wild scan");
		}
		// Emit match if have run of MIN_MATCH or longer, else emit literal 
		if(s->match_length >= MIN_MATCH) {
			check_match(s, s->strstart, s->strstart - 1, s->match_length);
			_tr_tally_dist(s, 1, s->match_length - MIN_MATCH, bflush);
			s->lookahead -= s->match_length;
			s->strstart += s->match_length;
			s->match_length = 0;
		}
		else {
			// No match, output a literal byte 
			Tracevv((stderr, "%c", s->window[s->strstart]));
			_tr_tally_lit(s, s->window[s->strstart], bflush);
			s->lookahead--;
			s->strstart++;
		}
		if(bflush) 
			FLUSH_BLOCK(s, 0);
	}
	s->insert = 0;
	if(flush == Z_FINISH) {
		FLUSH_BLOCK(s, 1);
		return finish_done;
	}
	if(s->last_lit)
		FLUSH_BLOCK(s, 0);
	return block_done;
}
// 
// For Z_HUFFMAN_ONLY, do not look for matches.  Do not maintain a hash table.
// (It will be regenerated if this run of deflate switches away from Huffman.)
// 
static block_state FASTCALL deflate_huff(deflate_state * s, int flush)
{
	int bflush; // set if current block must be flushed 
	for(;; ) {
		// Make sure that we have a literal to write
		if(s->lookahead == 0) {
			fill_window(s);
			if(s->lookahead == 0) {
				if(flush == Z_NO_FLUSH)
					return need_more;
				break; // flush the current block 
			}
		}
		// Output a literal byte 
		s->match_length = 0;
		Tracevv((stderr, "%c", s->window[s->strstart]));
		_tr_tally_lit(s, s->window[s->strstart], bflush);
		s->lookahead--;
		s->strstart++;
		if(bflush) 
			FLUSH_BLOCK(s, 0);
	}
	s->insert = 0;
	if(flush == Z_FINISH) {
		FLUSH_BLOCK(s, 1);
		return finish_done;
	}
	else {
		if(s->last_lit)
			FLUSH_BLOCK(s, 0);
		return block_done;
	}
}
//
// TREES -- output deflated data using Huffman coding
// Copyright (C) 1995-2017 Jean-loup Gailly
// detect_data_type() function provided freely by Cosmin Truta, 2006
// 
/*
 *  ALGORITHM
 *
 * The "deflation" process uses several Huffman trees. The more
 * common source values are represented by shorter bit sequences.
 *
 * Each code tree is stored in a compressed form which is itself
 * a Huffman encoding of the lengths of all the code strings (in
 * ascending order by source values).  The actual code strings are
 * reconstructed from the lengths in the inflate process, as described
 * in the deflate specification.
 *
 *  REFERENCES
 *
 * Deutsch, L.P.,"'Deflate' Compressed Data Format Specification".
 * Available in ftp.uu.net:/pub/archiving/zip/doc/deflate-1.1.doc
 *
 * Storer, James A.
 *     Data Compression:  Methods and Theory, pp. 49-50.
 *     Computer Science Press, 1988.  ISBN 0-7167-8156-5.
 *
 * Sedgewick, R.
 *     Algorithms, p290.
 *     Addison-Wesley, 1983. ISBN 0-201-06672-6.
 */
/* #define GEN_TREES_H */
// 
// Constants
// 
#define MAX_BL_BITS   7 // Bit length codes must not exceed MAX_BL_BITS bits 
#define END_BLOCK   256 // end of block literal code 
#define REP_3_6      16 // repeat previous bit length 3-6 times (2 bits of repeat count) 
#define REPZ_3_10    17 // repeat a zero length 3-10 times  (3 bits of repeat count) 
#define REPZ_11_138  18 // repeat a zero length 11-138 times  (7 bits of repeat count) 

// extra bits for each length code 
static const int extra_lbits[LENGTH_CODES] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
// extra bits for each distance code 
static const int extra_dbits[D_CODES] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// extra bits for each bit length code 
static const int extra_blbits[BL_CODES] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};
static const uchar bl_order[BL_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
// The lengths of the bit length codes are sent in order of decreasing
// probability, to avoid transmitting the lengths for unused bit length codes.
//
// Local data. These are initialized only once.
//
#define DIST_CODE_LEN  512 /* see definition of array dist_code below */

#if defined(GEN_TREES_H) || !defined(STDC) // non ANSI compilers may not accept trees.h 
	static ct_data static_ltree[L_CODES+2];
	/* The static literal tree. Since the bit lengths are imposed, there is no
	 * need for the L_CODES extra codes used during heap construction. However
	 * The codes 286 and 287 are needed to build a canonical tree (see _tr_init below).
	 */
	static ct_data static_dtree[D_CODES]; // The static distance tree. (Actually a trivial tree since all codes use 5 bits.)
	uchar _dist_code[DIST_CODE_LEN];
	/* Distance codes. The first 256 values correspond to the distances
	 * 3 .. 258, the last 256 values correspond to the top 8 bits of the 15 bit distances.
	 */
	uchar _length_code[MAX_MATCH-MIN_MATCH+1]; /* length code for each normalized match length (0 == MIN_MATCH) */
	static int base_length[LENGTH_CODES]; /* First normalized length for each code (0 = MIN_MATCH) */
	static int base_dist[D_CODES]; /* First normalized distance for each code (0 = distance of 1) */
#else
	//#include "trees.h"
	//
	// header created automatically with -DGEN_TREES_H 
	//
	static const ct_data static_ltree[L_CODES+2] = {
		{{ 12}, {  8}}, {{140}, {  8}}, {{ 76}, {  8}}, {{204}, {  8}}, {{ 44}, {  8}},
		{{172}, {  8}}, {{108}, {  8}}, {{236}, {  8}}, {{ 28}, {  8}}, {{156}, {  8}},
		{{ 92}, {  8}}, {{220}, {  8}}, {{ 60}, {  8}}, {{188}, {  8}}, {{124}, {  8}},
		{{252}, {  8}}, {{  2}, {  8}}, {{130}, {  8}}, {{ 66}, {  8}}, {{194}, {  8}},
		{{ 34}, {  8}}, {{162}, {  8}}, {{ 98}, {  8}}, {{226}, {  8}}, {{ 18}, {  8}},
		{{146}, {  8}}, {{ 82}, {  8}}, {{210}, {  8}}, {{ 50}, {  8}}, {{178}, {  8}},
		{{114}, {  8}}, {{242}, {  8}}, {{ 10}, {  8}}, {{138}, {  8}}, {{ 74}, {  8}},
		{{202}, {  8}}, {{ 42}, {  8}}, {{170}, {  8}}, {{106}, {  8}}, {{234}, {  8}},
		{{ 26}, {  8}}, {{154}, {  8}}, {{ 90}, {  8}}, {{218}, {  8}}, {{ 58}, {  8}},
		{{186}, {  8}}, {{122}, {  8}}, {{250}, {  8}}, {{  6}, {  8}}, {{134}, {  8}},
		{{ 70}, {  8}}, {{198}, {  8}}, {{ 38}, {  8}}, {{166}, {  8}}, {{102}, {  8}},
		{{230}, {  8}}, {{ 22}, {  8}}, {{150}, {  8}}, {{ 86}, {  8}}, {{214}, {  8}},
		{{ 54}, {  8}}, {{182}, {  8}}, {{118}, {  8}}, {{246}, {  8}}, {{ 14}, {  8}},
		{{142}, {  8}}, {{ 78}, {  8}}, {{206}, {  8}}, {{ 46}, {  8}}, {{174}, {  8}},
		{{110}, {  8}}, {{238}, {  8}}, {{ 30}, {  8}}, {{158}, {  8}}, {{ 94}, {  8}},
		{{222}, {  8}}, {{ 62}, {  8}}, {{190}, {  8}}, {{126}, {  8}}, {{254}, {  8}},
		{{  1}, {  8}}, {{129}, {  8}}, {{ 65}, {  8}}, {{193}, {  8}}, {{ 33}, {  8}},
		{{161}, {  8}}, {{ 97}, {  8}}, {{225}, {  8}}, {{ 17}, {  8}}, {{145}, {  8}},
		{{ 81}, {  8}}, {{209}, {  8}}, {{ 49}, {  8}}, {{177}, {  8}}, {{113}, {  8}},
		{{241}, {  8}}, {{  9}, {  8}}, {{137}, {  8}}, {{ 73}, {  8}}, {{201}, {  8}},
		{{ 41}, {  8}}, {{169}, {  8}}, {{105}, {  8}}, {{233}, {  8}}, {{ 25}, {  8}},
		{{153}, {  8}}, {{ 89}, {  8}}, {{217}, {  8}}, {{ 57}, {  8}}, {{185}, {  8}},
		{{121}, {  8}}, {{249}, {  8}}, {{  5}, {  8}}, {{133}, {  8}}, {{ 69}, {  8}},
		{{197}, {  8}}, {{ 37}, {  8}}, {{165}, {  8}}, {{101}, {  8}}, {{229}, {  8}},
		{{ 21}, {  8}}, {{149}, {  8}}, {{ 85}, {  8}}, {{213}, {  8}}, {{ 53}, {  8}},
		{{181}, {  8}}, {{117}, {  8}}, {{245}, {  8}}, {{ 13}, {  8}}, {{141}, {  8}},
		{{ 77}, {  8}}, {{205}, {  8}}, {{ 45}, {  8}}, {{173}, {  8}}, {{109}, {  8}},
		{{237}, {  8}}, {{ 29}, {  8}}, {{157}, {  8}}, {{ 93}, {  8}}, {{221}, {  8}},
		{{ 61}, {  8}}, {{189}, {  8}}, {{125}, {  8}}, {{253}, {  8}}, {{ 19}, {  9}},
		{{275}, {  9}}, {{147}, {  9}}, {{403}, {  9}}, {{ 83}, {  9}}, {{339}, {  9}},
		{{211}, {  9}}, {{467}, {  9}}, {{ 51}, {  9}}, {{307}, {  9}}, {{179}, {  9}},
		{{435}, {  9}}, {{115}, {  9}}, {{371}, {  9}}, {{243}, {  9}}, {{499}, {  9}},
		{{ 11}, {  9}}, {{267}, {  9}}, {{139}, {  9}}, {{395}, {  9}}, {{ 75}, {  9}},
		{{331}, {  9}}, {{203}, {  9}}, {{459}, {  9}}, {{ 43}, {  9}}, {{299}, {  9}},
		{{171}, {  9}}, {{427}, {  9}}, {{107}, {  9}}, {{363}, {  9}}, {{235}, {  9}},
		{{491}, {  9}}, {{ 27}, {  9}}, {{283}, {  9}}, {{155}, {  9}}, {{411}, {  9}},
		{{ 91}, {  9}}, {{347}, {  9}}, {{219}, {  9}}, {{475}, {  9}}, {{ 59}, {  9}},
		{{315}, {  9}}, {{187}, {  9}}, {{443}, {  9}}, {{123}, {  9}}, {{379}, {  9}},
		{{251}, {  9}}, {{507}, {  9}}, {{  7}, {  9}}, {{263}, {  9}}, {{135}, {  9}},
		{{391}, {  9}}, {{ 71}, {  9}}, {{327}, {  9}}, {{199}, {  9}}, {{455}, {  9}},
		{{ 39}, {  9}}, {{295}, {  9}}, {{167}, {  9}}, {{423}, {  9}}, {{103}, {  9}},
		{{359}, {  9}}, {{231}, {  9}}, {{487}, {  9}}, {{ 23}, {  9}}, {{279}, {  9}},
		{{151}, {  9}}, {{407}, {  9}}, {{ 87}, {  9}}, {{343}, {  9}}, {{215}, {  9}},
		{{471}, {  9}}, {{ 55}, {  9}}, {{311}, {  9}}, {{183}, {  9}}, {{439}, {  9}},
		{{119}, {  9}}, {{375}, {  9}}, {{247}, {  9}}, {{503}, {  9}}, {{ 15}, {  9}},
		{{271}, {  9}}, {{143}, {  9}}, {{399}, {  9}}, {{ 79}, {  9}}, {{335}, {  9}},
		{{207}, {  9}}, {{463}, {  9}}, {{ 47}, {  9}}, {{303}, {  9}}, {{175}, {  9}},
		{{431}, {  9}}, {{111}, {  9}}, {{367}, {  9}}, {{239}, {  9}}, {{495}, {  9}},
		{{ 31}, {  9}}, {{287}, {  9}}, {{159}, {  9}}, {{415}, {  9}}, {{ 95}, {  9}},
		{{351}, {  9}}, {{223}, {  9}}, {{479}, {  9}}, {{ 63}, {  9}}, {{319}, {  9}},
		{{191}, {  9}}, {{447}, {  9}}, {{127}, {  9}}, {{383}, {  9}}, {{255}, {  9}},
		{{511}, {  9}}, {{  0}, {  7}}, {{ 64}, {  7}}, {{ 32}, {  7}}, {{ 96}, {  7}},
		{{ 16}, {  7}}, {{ 80}, {  7}}, {{ 48}, {  7}}, {{112}, {  7}}, {{  8}, {  7}},
		{{ 72}, {  7}}, {{ 40}, {  7}}, {{104}, {  7}}, {{ 24}, {  7}}, {{ 88}, {  7}},
		{{ 56}, {  7}}, {{120}, {  7}}, {{  4}, {  7}}, {{ 68}, {  7}}, {{ 36}, {  7}},
		{{100}, {  7}}, {{ 20}, {  7}}, {{ 84}, {  7}}, {{ 52}, {  7}}, {{116}, {  7}},
		{{  3}, {  8}}, {{131}, {  8}}, {{ 67}, {  8}}, {{195}, {  8}}, {{ 35}, {  8}},
		{{163}, {  8}}, {{ 99}, {  8}}, {{227}, {  8}}
	};
	static const ct_data static_dtree[D_CODES] = {
		{{ 0}, { 5}}, {{16}, { 5}}, {{ 8}, { 5}}, {{24}, { 5}}, {{ 4}, { 5}},
		{{20}, { 5}}, {{12}, { 5}}, {{28}, { 5}}, {{ 2}, { 5}}, {{18}, { 5}},
		{{10}, { 5}}, {{26}, { 5}}, {{ 6}, { 5}}, {{22}, { 5}}, {{14}, { 5}},
		{{30}, { 5}}, {{ 1}, { 5}}, {{17}, { 5}}, {{ 9}, { 5}}, {{25}, { 5}},
		{{ 5}, { 5}}, {{21}, { 5}}, {{13}, { 5}}, {{29}, { 5}}, {{ 3}, { 5}},
		{{19}, { 5}}, {{11}, { 5}}, {{27}, { 5}}, {{ 7}, { 5}}, {{23}, { 5}}
	};
	const uchar ZLIB_INTERNAL _dist_code[DIST_CODE_LEN] = {
		0,  1,  2,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  8,
		8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10, 10, 10, 10,
		10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
		11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
		12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13,
		13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
		13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
		14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
		14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
		14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  0,  0, 16, 17,
		18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22,
		23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
		24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
		26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
		26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27,
		27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
		27, 27, 27, 27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
		28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
		28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
		28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
		29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
		29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
		29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29
	};
	const uchar ZLIB_INTERNAL _length_code[MAX_MATCH-MIN_MATCH+1] = {
		0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  9,  9, 10, 10, 11, 11, 12, 12, 12, 12,
		13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16, 16,
		17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19,
		19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
		21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22,
		22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23,
		23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
		24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
		25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
		25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26,
		26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
		26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
		27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28
	};
	static const int base_length[LENGTH_CODES] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 0
	};
	static const int base_dist[D_CODES] = {
		0,     1,     2,     3,     4,     6,     8,    12,    16,    24,
		32,    48,    64,    96,   128,   192,   256,   384,   512,   768,
		1024,  1536,  2048,  3072,  4096,  6144,  8192, 12288, 16384, 24576
	};
#endif /* GEN_TREES_H */

struct static_tree_desc_s {
	const ct_data * static_tree; /* static tree or NULL */
	const intf * extra_bits; /* extra bits for each code or NULL */
	int extra_base;          /* base index for extra_bits */
	int elems;               /* max number of elements in the tree */
	int max_length;          /* max bit length for the codes */
};

static const static_tree_desc static_l_desc = {static_ltree, extra_lbits, LITERALS+1, L_CODES, MAX_BITS};
static const static_tree_desc static_d_desc = {static_dtree, extra_dbits, 0,          D_CODES, MAX_BITS};
static const static_tree_desc static_bl_desc = {(const ct_data*)0, extra_blbits, 0,   BL_CODES, MAX_BL_BITS};
//
// Flush the bits in the bit buffer to pending output (leaves at most 7 bits)
//
void ZLIB_INTERNAL _tr_flush_bits(deflate_state * s)
{
	bi_flush(s);
}

#ifdef GEN_TREES_H
	static void gen_trees_header OF((void));
#endif
#ifndef ZLIB_DEBUG
	#define send_code(s, c, tree) send_bits(s, tree[c].Code, tree[c].Len) // Send a code of the given tree. c and tree must not have side effects 
#else /* !ZLIB_DEBUG */
	#define send_code(s, c, tree) { if(z_verbose>2) fprintf(stderr, "\ncd %3d ", (c)); send_bits(s, tree[c].Code, tree[c].Len); }
#endif
// 
// Output a short LSB first on the stream.
// IN assertion: there is enough room in pendingBuf.
// 
#define put_short(s, w) { put_byte(s, (uchar)((w) & 0xff)); put_byte(s, (uchar)((ushort)(w) >> 8)); }
// 
// Send a value on a given number of bits.
// IN assertion: length <= 16 and value fits in length bits.
// 
#ifdef ZLIB_DEBUG
	static void send_bits(deflate_state * s, int value, int length)
	{
		Tracevv((stderr, " l %2d v %4x ", length, value));
		Assert(length > 0 && length <= 15, "invalid length");
		s->bits_sent += (ulong)length;
		// If not enough room in bi_buf, use (valid) bits from bi_buf and
		// (16 - bi_valid) bits from value, leaving (width - (16-bi_valid)) unused bits in value.
		if(s->bi_valid > (int)Buf_size - length) {
			s->bi_buf |= (ushort)value << s->bi_valid;
			put_short(s, s->bi_buf);
			s->bi_buf = (ushort)value >> (Buf_size - s->bi_valid);
			s->bi_valid += length - Buf_size;
		}
		else {
			s->bi_buf |= (ushort)value << s->bi_valid;
			s->bi_valid += length;
		}
	}
#else /* !ZLIB_DEBUG */
	#define send_bits(s, value, length) \
		{ int len = length; \
		  if(s->bi_valid > (int)Buf_size - len) { \
			  int val = (int)value;	\
			  s->bi_buf |= (ushort)val << s->bi_valid;	\
			  put_short(s, s->bi_buf); \
			  s->bi_buf = (ushort)val >> (Buf_size - s->bi_valid); \
			  s->bi_valid += len - Buf_size; \
		  } else { \
			  s->bi_buf |= (ushort)(value) << s->bi_valid; \
			  s->bi_valid += len; \
		  } \
		}
#endif /* ZLIB_DEBUG */
// 
// the arguments must not have side effects 
// 
// Initialize the various 'constant' tables.
// 
static void tr_static_init()
{
#if defined(GEN_TREES_H) || !defined(STDC)
	static int static_init_done = 0;
	int n;    /* iterates over tree elements */
	int bits; /* bit counter */
	int length; /* length value */
	int code; /* code value */
	int dist; /* distance index */
	ushort bl_count[MAX_BITS+1];
	// number of codes at each bit length for an optimal tree 
	if(static_init_done) 
		return;
	// For some embedded targets, global variables are not initialized: 
#ifdef NO_INIT_GLOBAL_POINTERS
	static_l_desc.static_tree = static_ltree;
	static_l_desc.extra_bits = extra_lbits;
	static_d_desc.static_tree = static_dtree;
	static_d_desc.extra_bits = extra_dbits;
	static_bl_desc.extra_bits = extra_blbits;
#endif
	// Initialize the mapping length (0..255) -> length code (0..28) 
	length = 0;
	for(code = 0; code < LENGTH_CODES-1; code++) {
		base_length[code] = length;
		for(n = 0; n < (1<<extra_lbits[code]); n++) {
			_length_code[length++] = (uchar)code;
		}
	}
	Assert(length == 256, "tr_static_init: length != 256");
	// Note that the length 255 (match length 258) can be represented
	// in two different ways: code 284 + 5 bits or code 285, so we
	// overwrite length_code[255] to use the best encoding:
	_length_code[length-1] = (uchar)code;
	// Initialize the mapping dist (0..32K) -> dist code (0..29) 
	dist = 0;
	for(code = 0; code < 16; code++) {
		base_dist[code] = dist;
		for(n = 0; n < (1<<extra_dbits[code]); n++) {
			_dist_code[dist++] = (uchar)code;
		}
	}
	Assert(dist == 256, "tr_static_init: dist != 256");
	dist >>= 7; /* from now on, all distances are divided by 128 */
	for(; code < D_CODES; code++) {
		base_dist[code] = dist << 7;
		for(n = 0; n < (1<<(extra_dbits[code]-7)); n++) {
			_dist_code[256 + dist++] = (uchar)code;
		}
	}
	Assert(dist == 256, "tr_static_init: 256+dist != 512");
	// Construct the codes of the static literal tree 
	for(bits = 0; bits <= MAX_BITS; bits++) 
		bl_count[bits] = 0;
	n = 0;
	while(n <= 143) static_ltree[n++].Len = 8, bl_count[8]++;
	while(n <= 255) static_ltree[n++].Len = 9, bl_count[9]++;
	while(n <= 279) static_ltree[n++].Len = 7, bl_count[7]++;
	while(n <= 287) static_ltree[n++].Len = 8, bl_count[8]++;
	// Codes 286 and 287 do not exist, but we must include them in the
	// tree construction to get a canonical Huffman tree (longest code all ones)
	gen_codes((ct_data*)static_ltree, L_CODES+1, bl_count);
	// The static distance tree is trivial: 
	for(n = 0; n < D_CODES; n++) {
		static_dtree[n].Len = 5;
		static_dtree[n].Code = bi_reverse((uint)n, 5);
	}
	static_init_done = 1;
#ifdef GEN_TREES_H
	gen_trees_header();
#endif
#endif /* defined(GEN_TREES_H) || !defined(STDC) */
}
// 
// Genererate the file trees.h describing the static trees.
// 
#ifdef GEN_TREES_H
	#define SEPARATOR(i, last, width) ((i) == (last) ? "\n};\n\n" : ((i) % (width) == (width)-1 ? ",\n" : ", "))

void gen_trees_header()
{
	FILE * header = fopen("trees.h", "w");
	int i;
	Assert(header != NULL, "Can't open trees.h");
	fprintf(header, "/* header created automatically with -DGEN_TREES_H */\n\n");
	fprintf(header, "local const ct_data static_ltree[L_CODES+2] = {\n");
	for(i = 0; i < L_CODES+2; i++) {
		fprintf(header, "{{%3u},{%3u}}%s", static_ltree[i].Code, static_ltree[i].Len, SEPARATOR(i, L_CODES+1, 5));
	}
	fprintf(header, "local const ct_data static_dtree[D_CODES] = {\n");
	for(i = 0; i < D_CODES; i++) {
		fprintf(header, "{{%2u},{%2u}}%s", static_dtree[i].Code, static_dtree[i].Len, SEPARATOR(i, D_CODES-1, 5));
	}
	fprintf(header, "const uchar ZLIB_INTERNAL _dist_code[DIST_CODE_LEN] = {\n");
	for(i = 0; i < DIST_CODE_LEN; i++) {
		fprintf(header, "%2u%s", _dist_code[i], SEPARATOR(i, DIST_CODE_LEN-1, 20));
	}
	fprintf(header, "const uchar ZLIB_INTERNAL _length_code[MAX_MATCH-MIN_MATCH+1]= {\n");
	for(i = 0; i < MAX_MATCH-MIN_MATCH+1; i++) {
		fprintf(header, "%2u%s", _length_code[i], SEPARATOR(i, MAX_MATCH-MIN_MATCH, 20));
	}
	fprintf(header, "local const int base_length[LENGTH_CODES] = {\n");
	for(i = 0; i < LENGTH_CODES; i++) {
		fprintf(header, "%1u%s", base_length[i], SEPARATOR(i, LENGTH_CODES-1, 20));
	}
	fprintf(header, "local const int base_dist[D_CODES] = {\n");
	for(i = 0; i < D_CODES; i++) {
		fprintf(header, "%5u%s", base_dist[i], SEPARATOR(i, D_CODES-1, 10));
	}
	fclose(header);
}

#endif /* GEN_TREES_H */
// 
// Initialize the tree data structures for a new zlib stream.
// 
void ZLIB_INTERNAL _tr_init(deflate_state * s)
{
	tr_static_init();
	s->l_desc.dyn_tree = s->dyn_ltree;
	s->l_desc.stat_desc = &static_l_desc;

	s->d_desc.dyn_tree = s->dyn_dtree;
	s->d_desc.stat_desc = &static_d_desc;

	s->bl_desc.dyn_tree = s->bl_tree;
	s->bl_desc.stat_desc = &static_bl_desc;

	s->bi_buf = 0;
	s->bi_valid = 0;
#ifdef ZLIB_DEBUG
	s->compressed_len = 0L;
	s->bits_sent = 0L;
#endif
	init_block(s); // Initialize the first block of the first file
}
// 
// Initialize a new block.
// 
static void init_block(deflate_state * s)
{
	uint n; /* iterates over tree elements */
	/* Initialize the trees. */
	for(n = 0; n < L_CODES; n++) 
		s->dyn_ltree[n].Freq = 0;
	for(n = 0; n < D_CODES; n++) 
		s->dyn_dtree[n].Freq = 0;
	for(n = 0; n < BL_CODES; n++) 
		s->bl_tree[n].Freq = 0;
	s->dyn_ltree[END_BLOCK].Freq = 1;
	s->opt_len = s->static_len = 0L;
	s->last_lit = s->matches = 0;
}

#define SMALLEST 1 // Index within the heap array of least frequent node in the Huffman tree 
// 
// Remove the smallest element from the heap and recreate the heap with
// one less element. Updates heap and heap_len.
// 
#define pqremove(s, tree, top) { top = s->heap[SMALLEST]; s->heap[SMALLEST] = s->heap[s->heap_len--]; pqdownheap(s, tree, SMALLEST); }
// 
// Compares to subtrees, using the tree depth as tie breaker when
// the subtrees have equal frequency. This minimizes the worst case length.
// 
#define smaller(tree, n, m, depth) (tree[n].Freq < tree[m].Freq || (tree[n].Freq == tree[m].Freq && depth[n] <= depth[m]))
// 
// Restore the heap property by moving down the tree starting at node k,
// exchanging a node with the smallest of its two sons if necessary, stopping
// when the heap property is re-established (each father smaller than its two sons).
// 
static void pqdownheap(deflate_state * s, ct_data * tree, int k)
{
	int v = s->heap[k];
	int j = k << 1; /* left son of k */
	while(j <= s->heap_len) {
		// Set j to the smallest of the two sons: 
		if(j < s->heap_len && smaller(tree, s->heap[j+1], s->heap[j], s->depth)) {
			j++;
		}
		// Exit if v is smaller than both sons 
		if(smaller(tree, v, s->heap[j], s->depth)) 
			break;
		// Exchange v with the smallest son 
		s->heap[k] = s->heap[j];  
		k = j;
		// And continue down the tree, setting j to the left son of k 
		j <<= 1;
	}
	s->heap[k] = v;
}
// 
// Compute the optimal bit lengths for a tree and update the total bit length for the current block.
// IN assertion: the fields freq and dad are set, heap[heap_max] and
//   above are the tree nodes sorted by increasing frequency.
// OUT assertions: the field len is set to the optimal bit length, the
//   array bl_count contains the frequencies for each bit length.
//   The length opt_len is updated; static_len is also updated if stree is not null.
// 
static void gen_bitlen(deflate_state * s, tree_desc * desc)
{
	ct_data * tree        = desc->dyn_tree;
	int max_code         = desc->max_code;
	const ct_data * stree = desc->stat_desc->static_tree;
	const intf * extra    = desc->stat_desc->extra_bits;
	int base             = desc->stat_desc->extra_base;
	int max_length       = desc->stat_desc->max_length;
	int h;          /* heap index */
	int n, m;       /* iterate over the tree elements */
	int bits;       /* bit length */
	int xbits;      /* extra bits */
	ushort f;          /* frequency */
	int overflow = 0; /* number of elements with bit length too large */
	for(bits = 0; bits <= MAX_BITS; bits++) 
		s->bl_count[bits] = 0;
	/* In a first pass, compute the optimal bit lengths (which may
	 * overflow in the case of the bit length tree).
	 */
	tree[s->heap[s->heap_max]].Len = 0; /* root of the heap */
	for(h = s->heap_max+1; h < HEAP_SIZE; h++) {
		n = s->heap[h];
		bits = tree[tree[n].Dad].Len + 1;
		if(bits > max_length) 
			bits = max_length, overflow++;
		tree[n].Len = (ushort)bits;
		/* We overwrite tree[n].Dad which is no longer needed */
		if(n > max_code) 
			continue;  /* not a leaf node */
		s->bl_count[bits]++;
		xbits = 0;
		if(n >= base) 
			xbits = extra[n-base];
		f = tree[n].Freq;
		s->opt_len += (ulong)f * (uint)(bits + xbits);
		if(stree) 
			s->static_len += (ulong)f * (uint)(stree[n].Len + xbits);
	}
	if(overflow == 0) 
		return;
	Tracev((stderr, "\nbit length overflow\n"));
	/* This happens for example on obj2 and pic of the Calgary corpus */

	/* Find the first bit length which could increase: */
	do {
		bits = max_length-1;
		while(s->bl_count[bits] == 0) 
			bits--;
		s->bl_count[bits]--; /* move one leaf down the tree */
		s->bl_count[bits+1] += 2; /* move one overflow item as its brother */
		s->bl_count[max_length]--;
		/* The brother of the overflow item also moves one step up,
		 * but this does not affect bl_count[max_length]
		 */
		overflow -= 2;
	} while(overflow > 0);

	/* Now recompute all bit lengths, scanning in increasing frequency.
	 * h is still equal to HEAP_SIZE. (It is simpler to reconstruct all
	 * lengths instead of fixing only the wrong ones. This idea is taken
	 * from 'ar' written by Haruhiko Okumura.)
	 */
	for(bits = max_length; bits != 0; bits--) {
		n = s->bl_count[bits];
		while(n != 0) {
			m = s->heap[--h];
			if(m > max_code) 
				continue;
			if((uint)tree[m].Len != (uint)bits) {
				Tracev((stderr, "code %d bits %d->%d\n", m, tree[m].Len, bits));
				s->opt_len += ((ulong)bits - tree[m].Len) * tree[m].Freq;
				tree[m].Len = (ushort)bits;
			}
			n--;
		}
	}
}
// 
// Generate the codes for a given tree and bit counts (which need not be optimal).
// IN assertion: the array bl_count contains the bit length statistics for
// the given tree and the field len is set for all tree elements.
// OUT assertion: the field code is set for all tree elements of non zero code length.
// 
static void gen_codes(ct_data * tree, int max_code, ushort * bl_count)
{
	ushort next_code[MAX_BITS+1]; /* next code value for each bit length */
	uint code = 0;     /* running code value */
	int bits;              /* bit index */
	int n;                 /* code index */
	// The distribution counts are first used to generate the code values without bit reversal.
	for(bits = 1; bits <= MAX_BITS; bits++) {
		code = (code + bl_count[bits-1]) << 1;
		next_code[bits] = (ushort)code;
	}
	// Check that the bit counts in bl_count are consistent. The last code must be all ones.
	Assert(code + bl_count[MAX_BITS]-1 == (1<<MAX_BITS)-1, "inconsistent bit counts");
	Tracev((stderr, "\ngen_codes: max_code %d ", max_code));
	for(n = 0; n <= max_code; n++) {
		int len = tree[n].Len;
		if(len == 0) 
			continue;
		/* Now reverse the bits */
		tree[n].Code = (ushort)bi_reverse(next_code[len]++, len);
		Tracecv(tree != static_ltree, (stderr, "\nn %3d %c l %2d c %4x (%x) ", n, (isgraph(n) ? n : ' '), len, tree[n].Code, next_code[len]-1));
	}
}
// 
// Construct one Huffman tree and assigns the code bit strings and lengths.
// Update the total bit length for the current block.
// IN assertion: the field freq is set for all tree elements.
// OUT assertions: the fields len and code are set to the optimal bit length
//     and corresponding code. The length opt_len is updated; static_len is
//     also updated if stree is not null. The field max_code is set.
// 
static void build_tree(deflate_state * s, tree_desc * desc)
{
	ct_data * tree         = desc->dyn_tree;
	const ct_data * stree  = desc->stat_desc->static_tree;
	int elems             = desc->stat_desc->elems;
	int n, m;      /* iterate over heap elements */
	int max_code = -1; /* largest code with non zero frequency */
	int node;      /* new node being created */
	/* Construct the initial heap, with least frequent element in
	 * heap[SMALLEST]. The sons of heap[n] are heap[2*n] and heap[2*n+1].
	 * heap[0] is not used.
	 */
	s->heap_len = 0, s->heap_max = HEAP_SIZE;
	for(n = 0; n < elems; n++) {
		if(tree[n].Freq != 0) {
			s->heap[++(s->heap_len)] = max_code = n;
			s->depth[n] = 0;
		}
		else {
			tree[n].Len = 0;
		}
	}
	/* The pkzip format requires that at least one distance code exists,
	 * and that at least one bit should be sent even if there is only one
	 * possible code. So to avoid special checks later on we force at least
	 * two codes of non zero frequency.
	 */
	while(s->heap_len < 2) {
		node = s->heap[++(s->heap_len)] = (max_code < 2 ? ++max_code : 0);
		tree[node].Freq = 1;
		s->depth[node] = 0;
		s->opt_len--; 
		if(stree) 
			s->static_len -= stree[node].Len;
		// node is 0 or 1 so it does not have extra bits
	}
	desc->max_code = max_code;
	// The elements heap[heap_len/2+1 .. heap_len] are leaves of the tree,
	// establish sub-heaps of increasing lengths:
	for(n = s->heap_len/2; n >= 1; n--) 
		pqdownheap(s, tree, n);
	// Construct the Huffman tree by repeatedly combining the least two frequent nodes.
	node = elems;          /* next internal node of the tree */
	do {
		pqremove(s, tree, n); /* n = node of least frequency */
		m = s->heap[SMALLEST]; /* m = node of next least frequency */
		s->heap[--(s->heap_max)] = n; /* keep the nodes sorted by frequency */
		s->heap[--(s->heap_max)] = m;
		/* Create a new node father of n and m */
		tree[node].Freq = tree[n].Freq + tree[m].Freq;
		s->depth[node] = (uchar)((s->depth[n] >= s->depth[m] ? s->depth[n] : s->depth[m]) + 1);
		tree[n].Dad = tree[m].Dad = (ushort)node;
#ifdef DUMP_BL_TREE
		if(tree == s->bl_tree) {
			fprintf(stderr, "\nnode %d(%d), sons %d(%d) %d(%d)", node, tree[node].Freq, n, tree[n].Freq, m, tree[m].Freq);
		}
#endif
		/* and insert the new node in the heap */
		s->heap[SMALLEST] = node++;
		pqdownheap(s, tree, SMALLEST);
	} while(s->heap_len >= 2);
	s->heap[--(s->heap_max)] = s->heap[SMALLEST];
	// At this point, the fields freq and dad are set. We can now generate the bit lengths.
	gen_bitlen(s, (tree_desc*)desc);
	/* The field len is now set, we can generate the bit codes */
	gen_codes((ct_data*)tree, max_code, s->bl_count);
}
//
// Scan a literal or distance tree to determine the frequencies of the codes in the bit length tree.
//
static void scan_tree(deflate_state * s, ct_data * tree, int max_code)
{
	int n;                 /* iterates over all tree elements */
	int prevlen = -1;      /* last emitted length */
	int curlen;            /* length of current code */
	int nextlen = tree[0].Len; /* length of next code */
	int count = 0;         /* repeat count of the current code */
	int max_count = 7;     /* max repeat count */
	int min_count = 4;     /* min repeat count */
	if(nextlen == 0) 
		max_count = 138, min_count = 3;
	tree[max_code+1].Len = (ushort)0xffff; /* guard */
	for(n = 0; n <= max_code; n++) {
		curlen = nextlen; nextlen = tree[n+1].Len;
		if(++count < max_count && curlen == nextlen) {
			continue;
		}
		else if(count < min_count) {
			s->bl_tree[curlen].Freq += count;
		}
		else if(curlen != 0) {
			if(curlen != prevlen) 
				s->bl_tree[curlen].Freq++;
			s->bl_tree[REP_3_6].Freq++;
		}
		else if(count <= 10) {
			s->bl_tree[REPZ_3_10].Freq++;
		}
		else {
			s->bl_tree[REPZ_11_138].Freq++;
		}
		count = 0; prevlen = curlen;
		if(nextlen == 0) {
			max_count = 138, min_count = 3;
		}
		else if(curlen == nextlen) {
			max_count = 6, min_count = 3;
		}
		else {
			max_count = 7, min_count = 4;
		}
	}
}
//
// Send a literal or distance tree in compressed form, using the codes in bl_tree.
//
static void send_tree(deflate_state * s, ct_data * tree, int max_code)
{
	int n;                 /* iterates over all tree elements */
	int prevlen = -1;      /* last emitted length */
	int curlen;            /* length of current code */
	int nextlen = tree[0].Len; /* length of next code */
	int count = 0;         /* repeat count of the current code */
	int max_count = 7;     /* max repeat count */
	int min_count = 4;     /* min repeat count */
	/* tree[max_code+1].Len = -1; */  /* guard already set */
	if(nextlen == 0) 
		max_count = 138, min_count = 3;
	for(n = 0; n <= max_code; n++) {
		curlen = nextlen; nextlen = tree[n+1].Len;
		if(++count < max_count && curlen == nextlen) {
			continue;
		}
		else if(count < min_count) {
			do { 
				send_code(s, curlen, s->bl_tree); 
			} while(--count != 0);
		}
		else if(curlen != 0) {
			if(curlen != prevlen) {
				send_code(s, curlen, s->bl_tree); count--;
			}
			Assert(count >= 3 && count <= 6, " 3_6?");
			send_code(s, REP_3_6, s->bl_tree); send_bits(s, count-3, 2);
		}
		else if(count <= 10) {
			send_code(s, REPZ_3_10, s->bl_tree); send_bits(s, count-3, 3);
		}
		else {
			send_code(s, REPZ_11_138, s->bl_tree); send_bits(s, count-11, 7);
		}
		count = 0; prevlen = curlen;
		if(nextlen == 0) {
			max_count = 138, min_count = 3;
		}
		else if(curlen == nextlen) {
			max_count = 6, min_count = 3;
		}
		else {
			max_count = 7, min_count = 4;
		}
	}
}
//
// Construct the Huffman tree for the bit lengths and return the index in bl_order of the last bit length code to send.
//
static int build_bl_tree(deflate_state * s)
{
	int max_blindex; // index of last bit length code of non zero freq 
	// Determine the bit length frequencies for literal and distance trees 
	scan_tree(s, (ct_data*)s->dyn_ltree, s->l_desc.max_code);
	scan_tree(s, (ct_data*)s->dyn_dtree, s->d_desc.max_code);
	// Build the bit length tree: 
	build_tree(s, (tree_desc*)(&(s->bl_desc)));
	// opt_len now includes the length of the tree representations, except
	// the lengths of the bit lengths codes and the 5+5+4 bits for the counts.
	// 
	// Determine the number of bit length codes to send. The pkzip format
	// requires that at least 4 bit length codes be sent. (appnote.txt says
	// 3 but the actual value used is 4.)
	// 
	for(max_blindex = BL_CODES-1; max_blindex >= 3; max_blindex--) {
		if(s->bl_tree[bl_order[max_blindex]].Len != 0) 
			break;
	}
	// Update opt_len to include the bit length tree and counts 
	s->opt_len += 3*((ulong)max_blindex+1) + 5+5+4;
	Tracev((stderr, "\ndyn trees: dyn %ld, stat %ld", s->opt_len, s->static_len));
	return max_blindex;
}
// 
// Send the header for a block using dynamic Huffman trees: the counts, the
// lengths of the bit length codes, the literal tree and the distance tree.
// IN assertion: lcodes >= 257, dcodes >= 1, blcodes >= 4.
// 
static void send_all_trees(deflate_state * s, int lcodes, int dcodes, int blcodes)
{
	int rank;                /* index in bl_order */
	Assert(lcodes >= 257 && dcodes >= 1 && blcodes >= 4, "not enough codes");
	Assert(lcodes <= L_CODES && dcodes <= D_CODES && blcodes <= BL_CODES, "too many codes");
	Tracev((stderr, "\nbl counts: "));
	send_bits(s, lcodes-257, 5); /* not +255 as stated in appnote.txt */
	send_bits(s, dcodes-1,   5);
	send_bits(s, blcodes-4,  4); /* not -3 as stated in appnote.txt */
	for(rank = 0; rank < blcodes; rank++) {
		Tracev((stderr, "\nbl code %2d ", bl_order[rank]));
		send_bits(s, s->bl_tree[bl_order[rank]].Len, 3);
	}
	Tracev((stderr, "\nbl tree: sent %ld", s->bits_sent));
	send_tree(s, (ct_data*)s->dyn_ltree, lcodes-1); /* literal tree */
	Tracev((stderr, "\nlit tree: sent %ld", s->bits_sent));
	send_tree(s, (ct_data*)s->dyn_dtree, dcodes-1); /* distance tree */
	Tracev((stderr, "\ndist tree: sent %ld", s->bits_sent));
}
//
// Send a stored block
//
void ZLIB_INTERNAL _tr_stored_block(deflate_state * s, charf * buf, ulong stored_len, int last)
{
	send_bits(s, (STORED_BLOCK<<1)+last, 3); /* send block type */
	bi_windup(s);    /* align on byte boundary */
	put_short(s, (ushort)stored_len);
	put_short(s, (ushort) ~stored_len);
	memcpy(s->pending_buf + s->pending, (Bytef *)buf, stored_len);
	s->pending += stored_len;
#ifdef ZLIB_DEBUG
	s->compressed_len = (s->compressed_len + 3 + 7) & (ulong) ~7L;
	s->compressed_len += (stored_len + 4) << 3;
	s->bits_sent += 2*16;
	s->bits_sent += stored_len<<3;
#endif
}
//
// Send one empty static block to give enough lookahead for inflate.
// This takes 10 bits, of which 7 may remain in the bit buffer.
//
void ZLIB_INTERNAL _tr_align(deflate_state * s)
{
	send_bits(s, STATIC_TREES<<1, 3);
	send_code(s, END_BLOCK, static_ltree);
#ifdef ZLIB_DEBUG
	s->compressed_len += 10L; /* 3 for block type, 7 for EOB */
#endif
	bi_flush(s);
}
// 
// Determine the best encoding for the current block: dynamic trees, static
// trees or store, and write out the encoded block.
// 
void ZLIB_INTERNAL _tr_flush_block(deflate_state * s, charf * buf, ulong stored_len, int last)
{
	ulong opt_lenb, static_lenb; // opt_len and static_len in bytes 
	int max_blindex = 0; // index of last bit length code of non zero freq 
	// Build the Huffman trees unless a stored block is forced 
	if(s->level > 0) {
		// Check if the file is binary or text 
		if(s->strm->data_type == Z_UNKNOWN)
			s->strm->data_type = detect_data_type(s);
		// Construct the literal and distance trees 
		build_tree(s, (tree_desc*)(&(s->l_desc)));
		Tracev((stderr, "\nlit data: dyn %ld, stat %ld", s->opt_len, s->static_len));
		build_tree(s, (tree_desc*)(&(s->d_desc)));
		Tracev((stderr, "\ndist data: dyn %ld, stat %ld", s->opt_len, s->static_len));
		// At this point, opt_len and static_len are the total bit lengths of
		// the compressed block data, excluding the tree representations.
		// 
		// Build the bit length tree for the above two trees, and get the index
		// in bl_order of the last bit length code to send.
		// 
		max_blindex = build_bl_tree(s);
		// Determine the best encoding. Compute the block lengths in bytes. 
		opt_lenb = (s->opt_len+3+7)>>3;
		static_lenb = (s->static_len+3+7)>>3;
		Tracev((stderr, "\nopt %lu(%lu) stat %lu(%lu) stored %lu lit %u ", opt_lenb, s->opt_len, static_lenb, s->static_len, stored_len, s->last_lit));
		if(static_lenb <= opt_lenb) 
			opt_lenb = static_lenb;
	}
	else {
		Assert(buf != (char *)0, "lost buf");
		opt_lenb = static_lenb = stored_len + 5; /* force a stored block */
	}
#ifdef FORCE_STORED
	if(buf != (char *)0) { /* force stored block */
#else
	if(stored_len+4 <= opt_lenb && buf != (char *)0) {
		// 4: two words for the lengths 
#endif
		// The test buf != NULL is only necessary if LIT_BUFSIZE > WSIZE.
		// Otherwise we can't have processed more than WSIZE input bytes since
		// the last block flush, because compression would have been
		// successful. If LIT_BUFSIZE <= WSIZE, it is never too late to
		// transform a block into a stored block.
		_tr_stored_block(s, buf, stored_len, last);
#ifdef FORCE_STATIC
	}
	else if(static_lenb >= 0) { /* force static trees */
#else
	}
	else if(s->strategy == Z_FIXED || static_lenb == opt_lenb) {
#endif
		send_bits(s, (STATIC_TREES<<1)+last, 3);
		compress_block(s, (const ct_data*)static_ltree, (const ct_data*)static_dtree);
#ifdef ZLIB_DEBUG
		s->compressed_len += 3 + s->static_len;
#endif
	}
	else {
		send_bits(s, (DYN_TREES<<1)+last, 3);
		send_all_trees(s, s->l_desc.max_code+1, s->d_desc.max_code+1, max_blindex+1);
		compress_block(s, (const ct_data*)s->dyn_ltree, (const ct_data*)s->dyn_dtree);
#ifdef ZLIB_DEBUG
		s->compressed_len += 3 + s->opt_len;
#endif
	}
	Assert(s->compressed_len == s->bits_sent, "bad compressed size");
	// The above check is made mod 2^32, for files larger than 512 MB and uLong implemented on 32 bits.
	init_block(s);
	if(last) {
		bi_windup(s);
#ifdef ZLIB_DEBUG
		s->compressed_len += 7; /* align on byte boundary */
#endif
	}
	Tracev((stderr, "\ncomprlen %lu(%lu) ", s->compressed_len>>3, s->compressed_len-7*last));
}
//
// Save the match info and tally the frequency counts. Return true if the current block must be flushed.
//
int ZLIB_INTERNAL _tr_tally(deflate_state * s, uint dist, uint lc)
{
	s->d_buf[s->last_lit] = (ushort)dist;
	s->l_buf[s->last_lit++] = (uchar)lc;
	if(dist == 0) {
		s->dyn_ltree[lc].Freq++; // lc is the unmatched char 
	}
	else {
		s->matches++;
		// Here, lc is the match length - MIN_MATCH 
		dist--; // dist = match distance - 1 
		Assert((ushort)dist < (ushort)MAX_DIST(s) && (ushort)lc <= (ushort)(MAX_MATCH-MIN_MATCH) && (ushort)d_code(dist) < (ushort)D_CODES,  "_tr_tally: bad match");
		s->dyn_ltree[_length_code[lc]+LITERALS+1].Freq++;
		s->dyn_dtree[d_code(dist)].Freq++;
	}
#ifdef TRUNCATE_BLOCK
	/* Try to guess if it is profitable to stop the current block here */
	if((s->last_lit & 0x1fff) == 0 && s->level > 2) {
		/* Compute an upper bound for the compressed length */
		ulong out_length = (ulong)s->last_lit*8L;
		ulong in_length = (ulong)((long)s->strstart - s->block_start);
		int dcode;
		for(dcode = 0; dcode < D_CODES; dcode++) {
			out_length += (ulong)s->dyn_dtree[dcode].Freq * (5L+extra_dbits[dcode]);
		}
		out_length >>= 3;
		Tracev((stderr, "\nlast_lit %u, in %ld, out ~%ld(%ld%%) ", s->last_lit, in_length, out_length, 100L - out_length*100L/in_length));
		if(s->matches < s->last_lit/2 && out_length < in_length/2) return 1;
	}
#endif
	return (s->last_lit == s->lit_bufsize-1);
	// We avoid equality with lit_bufsize because of wraparound at 64K
	// on 16 bit machines and because stored blocks are restricted to 64K-1 bytes.
}
//
// Send the block data compressed using the given Huffman trees
//
static void compress_block(deflate_state * s, const ct_data * ltree, const ct_data * dtree)
{
	uint   dist;   // distance of matched string 
	int    lc;     // match length or unmatched char (if dist == 0) 
	uint   lx = 0; // running index in l_buf 
	uint   code;   // the code to send 
	int    extra;  // number of extra bits to send 
	if(s->last_lit != 0) 
		do {
			dist = s->d_buf[lx];
			lc = s->l_buf[lx++];
			if(dist == 0) {
				send_code(s, lc, ltree); /* send a literal byte */
				Tracecv(isgraph(lc), (stderr, " '%c' ", lc));
			}
			else {
				// Here, lc is the match length - MIN_MATCH 
				code = _length_code[lc];
				send_code(s, code+LITERALS+1, ltree); // send the length code 
				extra = extra_lbits[code];
				if(extra != 0) {
					lc -= base_length[code];
					send_bits(s, lc, extra); // send the extra length bits 
				}
				dist--; // dist is now the match distance - 1 
				code = d_code(dist);
				Assert(code < D_CODES, "bad d_code");
				send_code(s, code, dtree); // send the distance code 
				extra = extra_dbits[code];
				if(extra != 0) {
					dist -= (uint)base_dist[code];
					send_bits(s, dist, extra); // send the extra distance bits 
				}
			} // literal or match pair ? 
			// Check that the overlay between pending_buf and d_buf+l_buf is ok: 
			Assert((uInt)(s->pending) < s->lit_bufsize + 2*lx, "pendingBuf overflow");
		} while(lx < s->last_lit);
	send_code(s, END_BLOCK, ltree);
}
// 
// Check if the data type is TEXT or BINARY, using the following algorithm:
//   - TEXT if the two conditions below are satisfied:
//     a) There are no non-portable control characters belonging to the "black list" (0..6, 14..25, 28..31).
//     b) There is at least one printable character belonging to the "white list" (9 {TAB}, 10 {LF}, 13 {CR}, 32..255).
//   - BINARY otherwise.
//   - The following partially-portable control characters form a "gray list" that is ignored in this detection algorithm:
//     (7 {BEL}, 8 {BS}, 11 {VT}, 12 {FF}, 26 {SUB}, 27 {ESC}).
// IN assertion: the fields Freq of dyn_ltree are set.
// 
static int detect_data_type(deflate_state * s)
{
	// black_mask is the bit mask of black-listed bytes
	// set bits 0..6, 14..25, and 28..31
	// 0xf3ffc07f = binary 11110011111111111100000001111111
	ulong black_mask = 0xf3ffc07fUL;
	int n;
	// Check for non-textual ("black-listed") bytes. */
	for(n = 0; n <= 31; n++, black_mask >>= 1)
		if((black_mask & 1) && (s->dyn_ltree[n].Freq != 0))
			return Z_BINARY;
	// Check for textual ("white-listed") bytes. 
	if(s->dyn_ltree[9].Freq != 0 || s->dyn_ltree[10].Freq != 0 || s->dyn_ltree[13].Freq != 0)
		return Z_TEXT;
	for(n = 32; n < LITERALS; n++)
		if(s->dyn_ltree[n].Freq != 0)
			return Z_TEXT;
	// There are no "black-listed" or "white-listed" bytes:
	// this stream either is empty or has tolerated ("gray-listed") bytes only.
	return Z_BINARY;
}
//
// Reverse the first len bits of a code, using straightforward code (a faster method would use a table)
// IN assertion: 1 <= len <= 15
//
static uint FASTCALL bi_reverse(uint code, int len)
{
	uint   res = 0;
	do {
		res |= code & 1;
		code >>= 1, res <<= 1;
	} while(--len > 0);
	return res >> 1;
}
//
// Flush the bit buffer, keeping at most 7 bits in it.
//
static void FASTCALL bi_flush(deflate_state * s)
{
	if(s->bi_valid == 16) {
		put_short(s, s->bi_buf);
		s->bi_buf = 0;
		s->bi_valid = 0;
	}
	else if(s->bi_valid >= 8) {
		put_byte(s, (Byte)s->bi_buf);
		s->bi_buf >>= 8;
		s->bi_valid -= 8;
	}
}
//
// Flush the bit buffer and align the output on a byte boundary
//
static void FASTCALL bi_windup(deflate_state * s)
{
	if(s->bi_valid > 8) {
		put_short(s, s->bi_buf);
	}
	else if(s->bi_valid > 0) {
		put_byte(s, (Byte)s->bi_buf);
	}
	s->bi_buf = 0;
	s->bi_valid = 0;
#ifdef ZLIB_DEBUG
	s->bits_sent = (s->bits_sent+7) & ~7;
#endif
}
//
// INFFAST
//
#ifdef ASMINF
	#pragma message("Assembler code may have bugs -- use at your own risk")
#else
/*
   Decode literal, length, and distance codes and write out the resulting
   literal and match bytes until either not enough input or output is
   available, an end-of-block is encountered, or a data error is encountered.
   When large enough input and output buffers are supplied to inflate(), for
   example, a 16K input buffer and a 64K output buffer, more than 95% of the
   inflate execution time is spent in this routine.

   Entry assumptions:
        state->mode == LEN
        strm->avail_in >= 6
        strm->avail_out >= 258
        start >= strm->avail_out
        state->bits < 8

   On return, state->mode is one of:
        LEN -- ran out of enough output space or enough available input
        TYPE -- reached end of block code, inflate() to interpret next block
        BAD -- error in block data

   Notes:
    - The maximum input bits used by a length/distance pair is 15 bits for the
      length code, 5 bits for the length extra, 15 bits for the distance code,
      and 13 bits for the distance extra.  This totals 48 bits, or six bytes.
      Therefore if strm->avail_in >= 6, then there is enough input to avoid
      checking for available input while decoding.

    - The maximum bytes that a single length/distance pair can output is 258
      bytes, which is the maximum length that can be coded.  inflate_fast()
      requires strm->avail_out >= 258 for each loop to avoid checking for
      output space.
 */
void ZLIB_INTERNAL inflate_fast(z_streamp strm, uint start)
{
	uint   wsize;         /* window size or zero if not using window */
	uint   whave;         /* valid bytes in the window */
	uint   wnext;         /* window write index */
	uchar  * window; /* allocated sliding window, if wsize != 0 */
	ulong  hold;     /* local strm->hold */
	uint   bits;          /* local strm->bits */
	ZInfTreesCode const  * lcode; /* local strm->lencode */
	ZInfTreesCode const  * dcode; /* local strm->distcode */
	uint   lmask;         /* mask for first level of length codes */
	uint   dmask;         /* mask for first level of distance codes */
	ZInfTreesCode here;              /* retrieved table entry */
	uint   op;            /* code bits, operation, extra bits, or  window position, window bytes to copy */
	uint   len;           /* match length, unused bytes */
	uint   dist;          /* match distance */
	uchar  * from; /* where to copy match from */
	// copy state to local variables 
	struct inflate_state * state = (struct inflate_state *)strm->state;
	const uchar  * in = strm->next_in; // local strm->next_in 
	const uchar  * last = in + (strm->avail_in - 5); // have enough input while in < last 
	uchar  * out = strm->next_out; // local strm->next_out 
	uchar  * beg = out - (start - strm->avail_out); // inflate()'s initial strm->next_out 
	uchar  * end = out + (strm->avail_out - 257);   // while out < end, enough space available 
#ifdef INFLATE_STRICT
	uint dmax = state->dmax; // maximum distance from zlib header 
#endif
	wsize = state->wsize;
	whave = state->whave;
	wnext = state->wnext;
	window = state->window;
	hold = state->hold;
	bits = state->bits;
	lcode = state->lencode;
	dcode = state->distcode;
	lmask = (1U << state->lenbits) - 1;
	dmask = (1U << state->distbits) - 1;
	// decode literals and length/distances until end-of-block or not enough
	// input data or output space 
	do {
		if(bits < 15) {
			hold += (ulong)(*in++) << bits;
			bits += 8;
			hold += (ulong)(*in++) << bits;
			bits += 8;
		}
		here = lcode[hold & lmask];
dolen:
		op = (uint)(here.bits);
		hold >>= op;
		bits -= op;
		op = (uint)(here.op);
		if(op == 0) {                   /* literal */
			Tracevv((stderr, here.val >= 0x20 && here.val < 0x7f ? "inflate:         literal '%c'\n" : "inflate:         literal 0x%02x\n", here.val));
			*out++ = (uchar)(here.val);
		}
		else if(op & 16) {              /* length base */
			len = (uint)(here.val);
			op &= 15;               /* number of extra bits */
			if(op) {
				if(bits < op) {
					hold += (ulong)(*in++) << bits;
					bits += 8;
				}
				len += (uint)hold & ((1U << op) - 1);
				hold >>= op;
				bits -= op;
			}
			Tracevv((stderr, "inflate:         length %u\n", len));
			if(bits < 15) {
				hold += (ulong)(*in++) << bits;
				bits += 8;
				hold += (ulong)(*in++) << bits;
				bits += 8;
			}
			here = dcode[hold & dmask];
dodist:
			op = (uint)(here.bits);
			hold >>= op;
			bits -= op;
			op = (uint)(here.op);
			if(op & 16) {           /* distance base */
				dist = (uint)(here.val);
				op &= 15;       /* number of extra bits */
				if(bits < op) {
					hold += (ulong)(*in++) << bits;
					bits += 8;
					if(bits < op) {
						hold += (ulong)(*in++) << bits;
						bits += 8;
					}
				}
				dist += (uint)hold & ((1U << op) - 1);
#ifdef INFLATE_STRICT
				if(dist > dmax) {
					strm->msg = "invalid distance too far back";
					state->mode = BAD;
					break;
				}
#endif
				hold >>= op;
				bits -= op;
				Tracevv((stderr, "inflate:         distance %u\n", dist));
				op = (uint)(out - beg); /* max distance in output */
				if(dist > op) { /* see if copy from window */
					op = dist - op; /* distance back in window */
					if(op > whave) {
						if(state->sane) {
							strm->msg = "invalid distance too far back";
							state->mode = BAD;
							break;
						}
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
						if(len <= op - whave) {
							do {
								*out++ = 0;
							} while(--len);
							continue;
						}
						len -= op - whave;
						do {
							*out++ = 0;
						} while(--op > whave);
						if(op == 0) {
							from = out - dist;
							do {
								*out++ = *from++;
							} while(--len);
							continue;
						}
#endif
					}
					from = window;
					if(wnext == 0) { /* very common case */
						from += wsize - op;
						if(op < len) { /* some from window */
							len -= op;
							do {
								*out++ = *from++;
							} while(--op);
							from = out - dist; /* rest from output */
						}
					}
					else if(wnext < op) { /* wrap around window */
						from += wsize + wnext - op;
						op -= wnext;
						if(op < len) { /* some from end of window */
							len -= op;
							do {
								*out++ = *from++;
							} while(--op);
							from = window;
							if(wnext < len) { /* some from start of window */
								op = wnext;
								len -= op;
								do {
									*out++ = *from++;
								} while(--op);
								from = out - dist; /* rest from output */
							}
						}
					}
					else {  /* contiguous in window */
						from += wnext - op;
						if(op < len) { /* some from window */
							len -= op;
							do {
								*out++ = *from++;
							} while(--op);
							from = out - dist; /* rest from output */
						}
					}
					while(len > 2) {
						*out++ = *from++;
						*out++ = *from++;
						*out++ = *from++;
						len -= 3;
					}
					if(len) {
						*out++ = *from++;
						if(len > 1)
							*out++ = *from++;
					}
				}
				else {
					from = out - dist; /* copy direct from output */
					do {    /* minimum length is three */
						*out++ = *from++;
						*out++ = *from++;
						*out++ = *from++;
						len -= 3;
					} while(len > 2);
					if(len) {
						*out++ = *from++;
						if(len > 1)
							*out++ = *from++;
					}
				}
			}
			else if((op & 64) == 0) { /* 2nd level distance code */
				here = dcode[here.val + (hold & ((1U << op) - 1))];
				goto dodist;
			}
			else {
				strm->msg = "invalid distance code";
				state->mode = BAD;
				break;
			}
		}
		else if((op & 64) == 0) {       /* 2nd level length code */
			here = lcode[here.val + (hold & ((1U << op) - 1))];
			goto dolen;
		}
		else if(op & 32) {              /* end-of-block */
			Tracevv((stderr, "inflate:         end of block\n"));
			state->mode = TYPE;
			break;
		}
		else {
			strm->msg = "invalid literal/length code";
			state->mode = BAD;
			break;
		}
	} while(in < last && out < end);
	// return unused bytes (on entry, bits < 8, so in won't go too far back)
	len = bits >> 3;
	in -= len;
	bits -= len << 3;
	hold &= (1U << bits) - 1;
	// update state and return 
	strm->next_in = in;
	strm->next_out = out;
	strm->avail_in = (uint)(in < last ? 5 + (last - in) : 5 - (in - last));
	strm->avail_out = (uint)(out < end ? 257 + (end - out) : 257 - (out - end));
	state->hold = hold;
	state->bits = bits;
	return;
}
// 
// inflate_fast() speedups that turned out slower (on a PowerPC G3 750CXe):
//   - Using bit fields for code structure
//   - Different op definition to avoid & for extra bits (do & for table bits)
//   - Three separate decoding do-loops for direct, window, and wnext == 0
//   - Special case for distance > 1 copies to do overlapped load and store copy
//   - Explicit branch predictions (based on measured branch probabilities)
//   - Deferring match copy and interspersed it with decoding subsequent codes
//   - Swapping literal/length else
//   - Swapping window/direct else
//   - Larger unrolled copy loops (three is about right)
//   - Moving len -= 3 statement into middle of loop
//   
#endif /* !ASMINF */
//
// INFLATE
//
/*
 * Change history:
 *
 * 1.2.beta0    24 Nov 2002
 * - First version -- complete rewrite of inflate to simplify code, avoid
 * creation of window when not needed, minimize use of window when it is
 * needed, make inffast.c even faster, implement gzip decoding, and to
 * improve code readability and style over the previous zlib inflate code
 *
 * 1.2.beta1    25 Nov 2002
 * - Use pointers for available input and output checking in inffast.c
 * - Remove input and output counters in inffast.c
 * - Change inffast.c entry and loop from avail_in >= 7 to >= 6
 * - Remove unnecessary second byte pull from length extra in inffast.c
 * - Unroll direct copy to three copies per loop in inffast.c
 *
 * 1.2.beta2    4 Dec 2002
 * - Change external routine names to reduce potential conflicts
 * - Correct filename to inffixed.h for fixed tables in inflate.c
 * - Make hbuf[] uchar to match parameter type in inflate.c
 * - Change strm->next_out[-state->offset] to *(strm->next_out - state->offset)
 * to avoid negation problem on Alphas (64 bit) in inflate.c
 *
 * 1.2.beta3    22 Dec 2002
 * - Add comments on state->bits assertion in inffast.c
 * - Add comments on op field in inftrees.h
 * - Fix bug in reuse of allocated window after inflateReset()
 * - Remove bit fields--back to byte structure for speed
 * - Remove distance extra == 0 check in inflate_fast()--only helps for lengths
 * - Change post-increments to pre-increments in inflate_fast(), PPC biased?
 * - Add compile time option, POSTINC, to use post-increments instead (Intel?)
 * - Make MATCH copy in inflate() much faster for when inflate_fast() not used
 * - Use local copies of stream next and avail values, as well as local bit
 * buffer and bit count in inflate()--for speed when inflate_fast() not used
 *
 * 1.2.beta4    1 Jan 2003
 * - Split ptr - 257 statements in inflate_table() to avoid compiler warnings
 * - Move a comment on output buffer sizes from inffast.c to inflate.c
 * - Add comments in inffast.c to introduce the inflate_fast() routine
 * - Rearrange window copies in inflate_fast() for speed and simplification
 * - Unroll last copy for window match in inflate_fast()
 * - Use local copies of window variables in inflate_fast() for speed
 * - Pull out common wnext == 0 case for speed in inflate_fast()
 * - Make op and len in inflate_fast() unsigned for consistency
 * - Add FAR to lcode and dcode declarations in inflate_fast()
 * - Simplified bad distance check in inflate_fast()
 * - Added inflateBackInit(), inflateBack(), and inflateBackEnd() in new
 * source file infback.c to provide a call-back interface to inflate for
 * programs like gzip and unzip -- uses window as output buffer to avoid
 * window copying
 *
 * 1.2.beta5    1 Jan 2003
 * - Improved inflateBack() interface to allow the caller to provide initial
 * input in strm.
 * - Fixed stored blocks bug in inflateBack()
 *
 * 1.2.beta6    4 Jan 2003
 * - Added comments in inffast.c on effectiveness of POSTINC
 * - Typecasting all around to reduce compiler warnings
 * - Changed loops from while (1) or do {} while (1) to for (;;), again to
 * make compilers happy
 * - Changed type of window in inflateBackInit() to uchar *
 *
 * 1.2.beta7    27 Jan 2003
 * - Changed many types to unsigned or ushort to avoid warnings
 * - Added inflateCopy() function
 *
 * 1.2.0        9 Mar 2003
 * - Changed inflateBack() interface to provide separate opaque descriptors
 * for the in() and out() functions
 * - Changed inflateBack() argument and in_func typedef to swap the length
 * and buffer address return values for the input function
 * - Check next_in and next_out for Z_NULL on entry to inflate()
 *
 * The history for versions after 1.2.0 are in ChangeLog in zlib distribution.
 */
static int FASTCALL inflateStateCheck(const z_streamp strm)
{
	if(!strm || strm->zalloc == (alloc_func)0 || strm->zfree == (free_func)0)
		return 1;
	else {
		const struct inflate_state * state = (struct inflate_state *)strm->state;
		return (!state || state->strm != strm || state->mode < HEAD || state->mode > SYNC) ? 1 : 0;
	}
}

int ZEXPORT inflateResetKeep(z_streamp strm)
{
	struct inflate_state  * state;
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	state = (struct inflate_state *)strm->state;
	strm->total_in = strm->total_out = state->total = 0;
	strm->msg = Z_NULL;
	if(state->wrap)     /* to support ill-conceived Java test suite */
		strm->adler = state->wrap & 1;
	state->mode = HEAD;
	state->last = 0;
	state->havedict = 0;
	state->dmax = 32768U;
	state->head = Z_NULL;
	state->hold = 0;
	state->bits = 0;
	state->lencode = state->distcode = state->next = state->codes;
	state->sane = 1;
	state->back = -1;
	Tracev((stderr, "inflate: reset\n"));
	return Z_OK;
}

int ZEXPORT inflateReset(z_streamp strm)
{
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		state->wsize = 0;
		state->whave = 0;
		state->wnext = 0;
		return inflateResetKeep(strm);
	}
}

int ZEXPORT inflateReset2(z_streamp strm, int windowBits)
{
	// get the state 
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		int wrap;
		struct inflate_state * state = (struct inflate_state *)strm->state;
		// extract wrap request from windowBits parameter 
		if(windowBits < 0) {
			wrap = 0;
			windowBits = -windowBits;
		}
		else {
			wrap = (windowBits >> 4) + 5;
#ifdef GUNZIP
			if(windowBits < 48)
				windowBits &= 15;
#endif
		}
		// set number of window bits, free window if different 
		if(windowBits && (windowBits < 8 || windowBits > 15))
			return Z_STREAM_ERROR;
		else {
			if(state->window && state->wbits != (uint)windowBits) {
				ZLIB_FREE(strm, state->window);
				state->window = Z_NULL;
			}
			// update state and reset the rest of it 
			state->wrap = wrap;
			state->wbits = (uint)windowBits;
			return inflateReset(strm);
		}
	}
}

int ZEXPORT inflateInit2_(z_streamp strm, int windowBits, const char * version, int stream_size)
{
	int ret;
	struct inflate_state  * state;
	if(version == Z_NULL || version[0] != ZLIB_VERSION[0] || stream_size != (int)(sizeof(z_stream)))
		return Z_VERSION_ERROR;
	if(strm == Z_NULL) return Z_STREAM_ERROR;
	strm->msg = Z_NULL;             /* in case we return an error */
	if(strm->zalloc == (alloc_func)0) {
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zalloc = zcalloc;
		strm->opaque = (void *)0;
#endif
	}
	if(strm->zfree == (free_func)0)
#ifdef Z_SOLO
		return Z_STREAM_ERROR;
#else
		strm->zfree = zcfree;
#endif
	state = static_cast<struct inflate_state *>(ZLIB_ALLOC(strm, 1, sizeof(struct inflate_state)));
	if(state == Z_NULL) 
		return Z_MEM_ERROR;
	Tracev((stderr, "inflate: allocated\n"));
	strm->state = (struct internal_state *)state;
	state->strm = strm;
	state->window = Z_NULL;
	state->mode = HEAD; /* to pass state test in inflateReset2() */
	ret = inflateReset2(strm, windowBits);
	if(ret != Z_OK) {
		ZLIB_FREE(strm, state);
		strm->state = Z_NULL;
	}
	return ret;
}

int ZEXPORT inflateInit_(z_streamp strm, const char * version, int stream_size)
{
	return inflateInit2_(strm, DEF_WBITS, version, stream_size);
}

int ZEXPORT inflatePrime(z_streamp strm, int bits, int value)
{
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = reinterpret_cast<struct inflate_state *>(strm->state);
		if(bits < 0) {
			state->hold = 0;
			state->bits = 0;
			return Z_OK;
		}
		else if(bits > 16 || state->bits + (uInt)bits > 32) 
			return Z_STREAM_ERROR;
		else {
			value &= (1L << bits) - 1;
			state->hold += (uint)value << state->bits;
			state->bits += (uInt)bits;
			return Z_OK;
		}
	}
}
#if 0 // @sobolev (������� ������������� � 2-� �������. ����� ������� ������� �������� ���� ����������) {
/*
   Return state with length and distance decoding tables and index sizes set to
   fixed code decoding.  Normally this returns fixed tables from inffixed.h.
   If BUILDFIXED is defined, then instead this routine builds the tables the
   first time it's called, and returns those tables the first time and
   thereafter.  This reduces the size of the code by about 2K bytes, in
   exchange for a little execution time.  However, BUILDFIXED should not be
   used for threaded applications, since the rewriting of the tables and virgin
   may not be thread-safe.
 */
static void FASTCALL fixedtables(struct inflate_state * state)
{
#ifdef BUILDFIXED
	static int virgin = 1;
	static code * lenfix, * distfix;
	static code fixed[544];
	// build fixed huffman tables if first call (may not be thread safe) 
	if(virgin) {
		uint   bits;
		static code * next;
		// literal/length table 
		uint   sym = 0;
		while(sym < 144) state->lens[sym++] = 8;
		while(sym < 256) state->lens[sym++] = 9;
		while(sym < 280) state->lens[sym++] = 7;
		while(sym < 288) state->lens[sym++] = 8;
		next = fixed;
		lenfix = next;
		bits = 9;
		inflate_table(LENS, state->lens, 288, &(next), &(bits), state->work);
		// distance table 
		sym = 0;
		while(sym < 32) 
			state->lens[sym++] = 5;
		distfix = next;
		bits = 5;
		inflate_table(DISTS, state->lens, 32, &(next), &(bits), state->work);
		// do this just once 
		virgin = 0;
	}
#else /* !BUILDFIXED */
	#include "inffixed.h"
#endif /* BUILDFIXED */
	state->lencode = lenfix;
	state->lenbits = 9;
	state->distcode = distfix;
	state->distbits = 5;
}
#endif // } 0 @sobolev

#ifdef MAKEFIXED
/*
   Write out the inffixed.h that is #include'd above.  Defining MAKEFIXED also
   defines BUILDFIXED, so the tables are built on the fly.  makefixed() writes
   those tables to stdout, which would be piped to inffixed.h.  A small program
   can simply call makefixed to do this:

    void makefixed(void);

    int main(void)
    {
        makefixed();
        return 0;
    }

   Then that can be linked with zlib built with MAKEFIXED defined and run:

    a.out > inffixed.h
 */
void makefixed()
{
	uint low, size;
	struct inflate_state state;
	fixedtables(&state);
	puts("    /* inffixed.h -- table for decoding fixed codes");
	puts(" * Generated automatically by makefixed().");
	puts("     */");
	puts("");
	puts("    /* WARNING: this file should *not* be used by applications.");
	puts("       It is part of the implementation of this library and is");
	puts("       subject to change. Applications should only use zlib.h.");
	puts("     */");
	puts("");
	size = 1U << 9;
	printf("    static const code lenfix[%u] = {", size);
	low = 0;
	for(;; ) {
		if((low % 7) == 0) printf("\n        ");
		printf("{%u,%u,%d}", (low & 127) == 99 ? 64 : state.lencode[low].op, state.lencode[low].bits, state.lencode[low].val);
		if(++low == size) break;
		putchar(',');
	}
	puts("\n    };");
	size = 1U << 5;
	printf("\n    static const code distfix[%u] = {", size);
	low = 0;
	for(;; ) {
		if((low % 6) == 0) printf("\n        ");
		printf("{%u,%u,%d}", state.distcode[low].op, state.distcode[low].bits,
		    state.distcode[low].val);
		if(++low == size) break;
		putchar(',');
	}
	puts("\n    };");
}

#endif /* MAKEFIXED */
// 
// Update the window with the last wsize (normally 32K) bytes written before
// returning.  If window does not exist yet, create it.  This is only called
// when a window is already in use, or when output has been written during this
// inflate call, but the end of the deflate stream has not been reached yet.
// It is also called to create a window for dictionary data when a dictionary is loaded.
//
// Providing output buffers larger than 32K to inflate() should provide a speed
// advantage, since only the last 32K of output is copied to the sliding window
// upon return from inflate(), and since all distances after the first 32K of
// output will fall in the output data, making match copies simpler and faster.
// The advantage may be dependent on the size of the processor's data caches.
// 
static int updatewindow(z_streamp strm, const uchar * end, uint copy)
{
	uint dist;
	struct inflate_state  * state = (struct inflate_state *)strm->state;
	// if it hasn't been done already, allocate space for the window 
	if(state->window == Z_NULL) {
		state->window = (uchar *)ZLIB_ALLOC(strm, 1U << state->wbits, sizeof(uchar));
		if(state->window == Z_NULL) 
			return 1;
	}
	// if window not in use yet, initialize 
	if(state->wsize == 0) {
		state->wsize = 1U << state->wbits;
		state->wnext = 0;
		state->whave = 0;
	}
	// copy state->wsize or less output bytes into the circular window 
	if(copy >= state->wsize) {
		memcpy(state->window, end - state->wsize, state->wsize);
		state->wnext = 0;
		state->whave = state->wsize;
	}
	else {
		dist = state->wsize - state->wnext;
		SETMIN(dist, copy);
		memcpy(state->window + state->wnext, end - copy, dist);
		copy -= dist;
		if(copy) {
			memcpy(state->window, end - copy, copy);
			state->wnext = copy;
			state->whave = state->wsize;
		}
		else {
			state->wnext += dist;
			if(state->wnext == state->wsize) 
				state->wnext = 0;
			if(state->whave < state->wsize) 
				state->whave += dist;
		}
	}
	return 0;
}

/* Macros for inflate(): */

/* check function to use adler32() for zlib or crc32() for gzip */
#ifdef GUNZIP
	#define UPDATE(check, buf, len) (state->flags ? crc32(check, buf, len) : adler32(check, buf, len))
#else
	#define UPDATE(check, buf, len) adler32(check, buf, len)
#endif
//
// check macros for header crc 
//
#ifdef GUNZIP
#define CRC2(check, word) do { hbuf[0] = (uchar)(word); hbuf[1] = (uchar)((word) >> 8); check = crc32(check, hbuf, 2); } while(0)
#define CRC4(check, word) do { hbuf[0] = (uchar)(word); hbuf[1] = (uchar)((word) >> 8);	hbuf[2] = (uchar)((word) >> 16); hbuf[3] = (uchar)((word) >> 24); check = crc32(check, hbuf, 4); } while(0)
#endif
//
// Get a byte of input into the bit accumulator, or return from inflate() if there is no input available. 
//
#define PULLBYTE_INFL() \
	do { \
		if(have == 0) goto inf_leave; \
		have--;	\
		hold += (ulong)(*next++) << bits; \
		bits += 8; \
	} while(0)

// Assure that there are at least n bits in the bit accumulator.  If there is not enough available input to do that, then return from inflate(). 
#define NEEDBITS_INFL(n) do { while(bits < (uint)(n)) PULLBYTE_INFL(); } while(0)
/*
   inflate() uses a state machine to process as much input data and generate as
   much output data as possible before returning.  The state machine is
   structured roughly as follows:

    for (;;) switch (state) {
    ...
    case STATEn:
        if (not enough input data or output space to make progress)
            return;
        ... make progress ...
        state = STATEm;
        break;
    ...
    }

   so when inflate() is called again, the same case is attempted again, and
   if the appropriate resources are provided, the machine proceeds to the
   next state.  The NEEDBITS_INFL() macro is usually the way the state evaluates
   whether it can proceed or should return.  NEEDBITS_INFL() does the return if
   the requested bits are not available.  The typical use of the BITS macros
   is:

        NEEDBITS_INFL(n);
        ... do something with BITS(n) ...
        DROPBITS(n);

   where NEEDBITS_INFL(n) either returns from inflate() if there isn't enough
   input left to load n bits into the accumulator, or it continues.  BITS(n)
   gives the low n bits in the accumulator.  When done, DROPBITS(n) drops
   the low n bits off the accumulator.  INITBITS() clears the accumulator
   and sets the number of available bits to zero.  BYTEBITS() discards just
   enough bits to put the accumulator on a byte boundary.  After BYTEBITS()
   and a NEEDBITS_INFL(8), then BITS(8) would return the next byte in the stream.

   NEEDBITS_INFL(n) uses PULLBYTE_INFL() to get an available byte of input, or to return
   if there is no input available.  The decoding of variable length codes uses
   PULLBYTE_INFL() directly in order to pull just enough bytes to decode the next
   code, and no more.

   Some states loop until they get enough input, making sure that enough
   state information is maintained to continue the loop where it left off
   if NEEDBITS_INFL() returns in the loop.  For example, want, need, and keep
   would all have to actually be part of the saved state in case NEEDBITS_INFL()
   returns:

    case STATEw:
        while (want < need) {
            NEEDBITS_INFL(n);
            keep[want++] = BITS(n);
            DROPBITS(n);
        }
        state = STATEx;
    case STATEx:

   As shown above, if the next state is also the next case, then the break
   is omitted.

   A state may also return if there is not enough output space available to
   complete that state.  Those states are copying stored data, writing a
   literal byte, and copying a matching string.

   When returning, a "goto inf_leave" is used to update the total counters,
   update the check value, and determine whether any progress has been made
   during that inflate() call in order to return the proper return code.
   Progress is defined as a change in either strm->avail_in or strm->avail_out.
   When there is a window, goto inf_leave will update the window with the last
   output written.  If a goto inf_leave occurs in the middle of decompression
   and there is no window currently, goto inf_leave will create one and copy
   output to the window for the next call of inflate().

   In this implementation, the flush parameter of inflate() only affects the
   return code (per zlib.h).  inflate() always writes as much as possible to
   strm->next_out, given the space available and the provided input--the effect
   documented in zlib.h of Z_SYNC_FLUSH.  Furthermore, inflate() always defers
   the allocation of and copying into a sliding window until necessary, which
   provides the effect documented in zlib.h for Z_FINISH when the entire input
   stream available.  So the only thing the flush parameter actually does is:
   when flush is set to Z_FINISH, inflate() cannot return Z_OK.  Instead it
   will return Z_BUF_ERROR if it has not reached the end of the stream.
 */
int ZEXPORT inflate(z_streamp strm, int flush)
{
	struct inflate_state * state;
	const uchar * next; // next input 
	uchar  * put; // next output 
	uint   have, left; // available input and output 
	ulong  hold; // bit buffer 
	uint   bits;          /* bits in bit buffer */
	uint   in, out;       /* save starting available input and output */
	uint   copy;          /* number of stored or match bytes to copy */
	uchar  * from; /* where to copy match bytes from */
	ZInfTreesCode here; // current decoding table entry 
	ZInfTreesCode last; // parent table entry 
	uint   len; // length to copy for repeats, bits to drop 
	int    ret; // return code 
#ifdef GUNZIP
	uchar  hbuf[4]; // buffer for gzip header crc calculation 
#endif
	// permutation of code lengths 
	static const ushort order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	if(inflateStateCheck(strm) || strm->next_out == Z_NULL || (strm->next_in == Z_NULL && strm->avail_in != 0))
		return Z_STREAM_ERROR;
	state = (struct inflate_state *)strm->state;
	if(state->mode == TYPE) 
		state->mode = TYPEDO;   /* skip check */
	LOAD();
	in = have;
	out = left;
	ret = Z_OK;
	for(;; )
		switch(state->mode) {
			case HEAD:
			    if(state->wrap == 0) {
				    state->mode = TYPEDO;
				    break;
			    }
			    NEEDBITS_INFL(16);
#ifdef GUNZIP
			    if((state->wrap & 2) && hold == 0x8b1f) { /* gzip header */
					SETIFZ(state->wbits, 15);
				    state->check = crc32(0L, Z_NULL, 0);
				    CRC2(state->check, hold);
				    INITBITS();
				    state->mode = FLAGS;
				    break;
			    }
			    state->flags = 0; /* expect zlib header */
			    if(state->head)
				    state->head->done = -1;
			    if(!(state->wrap & 1) || /* check if zlib header allowed */
#else
			    if(
#endif
			    ((BITS(8) << 8) + (hold >> 8)) % 31) {
				    strm->msg = "incorrect header check";
				    state->mode = BAD;
				    break;
			    }
			    if(BITS(4) != Z_DEFLATED) {
				    strm->msg = "unknown compression method";
				    state->mode = BAD;
				    break;
			    }
			    DROPBITS(4);
			    len = BITS(4) + 8;
				SETIFZ(state->wbits, len);
			    if(len > 15 || len > state->wbits) {
				    strm->msg = "invalid window size";
				    state->mode = BAD;
				    break;
			    }
			    state->dmax = 1U << len;
			    Tracev((stderr, "inflate:   zlib header ok\n"));
			    strm->adler = state->check = adler32(0L, Z_NULL, 0);
			    state->mode = hold & 0x200 ? DICTID : TYPE;
			    INITBITS();
			    break;
#ifdef GUNZIP
			case FLAGS:
			    NEEDBITS_INFL(16);
			    state->flags = (int)(hold);
			    if((state->flags & 0xff) != Z_DEFLATED) {
				    strm->msg = "unknown compression method";
				    state->mode = BAD;
				    break;
			    }
			    if(state->flags & 0xe000) {
				    strm->msg = "unknown header flags set";
				    state->mode = BAD;
				    break;
			    }
			    if(state->head)
				    state->head->text = (int)((hold >> 8) & 1);
			    if((state->flags & 0x0200) && (state->wrap & 4))
				    CRC2(state->check, hold);
			    INITBITS();
			    state->mode = TIME;
			case TIME:
			    NEEDBITS_INFL(32);
			    if(state->head)
				    state->head->time = hold;
			    if((state->flags & 0x0200) && (state->wrap & 4))
				    CRC4(state->check, hold);
			    INITBITS();
			    state->mode = OS;
			case OS:
			    NEEDBITS_INFL(16);
			    if(state->head) {
				    state->head->xflags = (int)(hold & 0xff);
				    state->head->os = (int)(hold >> 8);
			    }
			    if((state->flags & 0x0200) && (state->wrap & 4))
				    CRC2(state->check, hold);
			    INITBITS();
			    state->mode = EXLEN;
			case EXLEN:
			    if(state->flags & 0x0400) {
				    NEEDBITS_INFL(16);
				    state->length = (uint)(hold);
				    if(state->head)
					    state->head->extra_len = (uint)hold;
				    if((state->flags & 0x0200) && (state->wrap & 4))
					    CRC2(state->check, hold);
				    INITBITS();
			    }
			    else if(state->head)
				    state->head->extra = Z_NULL;
			    state->mode = EXTRA;
			case EXTRA:
			    if(state->flags & 0x0400) {
				    copy = state->length;
				    if(copy > have) copy = have;
				    if(copy) {
					    if(state->head && state->head->extra) {
						    len = state->head->extra_len - state->length;
						    memcpy(state->head->extra + len, next,
						    len + copy > state->head->extra_max ?
						    state->head->extra_max - len : copy);
					    }
					    if((state->flags & 0x0200) && (state->wrap & 4))
						    state->check = crc32(state->check, next, copy);
					    have -= copy;
					    next += copy;
					    state->length -= copy;
				    }
				    if(state->length) 
						goto inf_leave;
			    }
			    state->length = 0;
			    state->mode = NAME;
			case NAME:
			    if(state->flags & 0x0800) {
				    if(have == 0) 
						goto inf_leave;
				    copy = 0;
				    do {
					    len = (uint)(next[copy++]);
					    if(state->head && state->head->name && state->length < state->head->name_max)
						    state->head->name[state->length++] = (Bytef)len;
				    } while(len && copy < have);
				    if((state->flags & 0x0200) && (state->wrap & 4))
					    state->check = crc32(state->check, next, copy);
				    have -= copy;
				    next += copy;
				    if(len) 
						goto inf_leave;
			    }
			    else if(state->head)
				    state->head->name = Z_NULL;
			    state->length = 0;
			    state->mode = COMMENT;
			case COMMENT:
			    if(state->flags & 0x1000) {
				    if(have == 0) goto inf_leave;
				    copy = 0;
				    do {
					    len = (uint)(next[copy++]);
					    if(state->head && state->head->comment && state->length < state->head->comm_max)
						    state->head->comment[state->length++] = (Bytef)len;
				    } while(len && copy < have);
				    if((state->flags & 0x0200) && (state->wrap & 4))
					    state->check = crc32(state->check, next, copy);
				    have -= copy;
				    next += copy;
				    if(len) 
						goto inf_leave;
			    }
			    else if(state->head)
				    state->head->comment = Z_NULL;
			    state->mode = HCRC;
			case HCRC:
			    if(state->flags & 0x0200) {
				    NEEDBITS_INFL(16);
				    if((state->wrap & 4) && hold != (state->check & 0xffff)) {
					    strm->msg = "header crc mismatch";
					    state->mode = BAD;
					    break;
				    }
				    INITBITS();
			    }
			    if(state->head) {
				    state->head->hcrc = (int)((state->flags >> 9) & 1);
				    state->head->done = 1;
			    }
			    strm->adler = state->check = crc32(0L, Z_NULL, 0);
			    state->mode = TYPE;
			    break;
#endif
			case DICTID:
			    NEEDBITS_INFL(32);
			    strm->adler = state->check = ZSWAP32(hold);
			    INITBITS();
			    state->mode = DICT;
			case DICT:
			    if(state->havedict == 0) {
				    RESTORE();
				    return Z_NEED_DICT;
			    }
			    strm->adler = state->check = adler32(0L, Z_NULL, 0);
			    state->mode = TYPE;
			case TYPE:
			    if(oneof2(flush, Z_BLOCK, Z_TREES))
					goto inf_leave;
			case TYPEDO:
			    if(state->last) {
				    BYTEBITS();
				    state->mode = CHECK;
				    break;
			    }
			    NEEDBITS_INFL(3);
			    state->last = BITS(1);
			    DROPBITS(1);
			    switch(BITS(2)) {
				    case 0:     /* stored block */
						Tracev((stderr, "inflate:     stored block%s\n", state->last ? " (last)" : ""));
						state->mode = STORED;
						break;
				    case 1:     /* fixed block */
						fixedtables(state);
						Tracev((stderr, "inflate:     fixed codes block%s\n", state->last ? " (last)" : ""));
						state->mode = LEN_; /* decode codes */
						if(flush == Z_TREES) {
							DROPBITS(2);
							goto inf_leave;
						}
						break;
				    case 2:     /* dynamic block */
						Tracev((stderr, "inflate:     dynamic codes block%s\n", state->last ? " (last)" : ""));
						state->mode = TABLE;
						break;
				    case 3:
						strm->msg = "invalid block type";
						state->mode = BAD;
			    }
			    DROPBITS(2);
			    break;
			case STORED:
			    BYTEBITS();         /* go to byte boundary */
			    NEEDBITS_INFL(32);
			    if((hold & 0xffff) != ((hold >> 16) ^ 0xffff)) {
				    strm->msg = "invalid stored block lengths";
				    state->mode = BAD;
				    break;
			    }
			    state->length = (uint)hold & 0xffff;
			    Tracev((stderr, "inflate:       stored length %u\n", state->length));
			    INITBITS();
			    state->mode = COPY_;
			    if(flush == Z_TREES) 
					goto inf_leave;
			case COPY_:
			    state->mode = COPY;
			case COPY:
			    copy = state->length;
			    if(copy) {
					SETMIN(copy, have);
					SETMIN(copy, left);
				    if(copy == 0) 
						goto inf_leave;
				    memcpy(put, next, copy);
				    have -= copy;
				    next += copy;
				    left -= copy;
				    put += copy;
				    state->length -= copy;
				    break;
			    }
			    Tracev((stderr, "inflate:       stored end\n"));
			    state->mode = TYPE;
			    break;
			case TABLE:
			    NEEDBITS_INFL(14);
			    state->nlen = BITS(5) + 257;
			    DROPBITS(5);
			    state->ndist = BITS(5) + 1;
			    DROPBITS(5);
			    state->ncode = BITS(4) + 4;
			    DROPBITS(4);
#ifndef PKZIP_BUG_WORKAROUND
			    if(state->nlen > 286 || state->ndist > 30) {
				    strm->msg = "too many length or distance symbols";
				    state->mode = BAD;
				    break;
			    }
#endif
			    Tracev((stderr, "inflate:       table sizes ok\n"));
			    state->have = 0;
			    state->mode = LENLENS;
			case LENLENS:
			    while(state->have < state->ncode) {
				    NEEDBITS_INFL(3);
				    state->lens[order[state->have++]] = (ushort)BITS(3);
				    DROPBITS(3);
			    }
			    while(state->have < 19)
				    state->lens[order[state->have++]] = 0;
			    state->next = state->codes;
			    state->lencode = (const ZInfTreesCode *)(state->next);
			    state->lenbits = 7;
			    ret = inflate_table(CODES, state->lens, 19, &(state->next), &(state->lenbits), state->work);
			    if(ret) {
				    strm->msg = "invalid code lengths set";
				    state->mode = BAD;
				    break;
			    }
			    Tracev((stderr, "inflate:       code lengths ok\n"));
			    state->have = 0;
			    state->mode = CODELENS;
			case CODELENS:
			    while(state->have < state->nlen + state->ndist) {
				    for(;; ) {
					    here = state->lencode[BITS(state->lenbits)];
					    if((uint)(here.bits) <= bits) 
							break;
					    PULLBYTE_INFL();
				    }
				    if(here.val < 16) {
					    DROPBITS(here.bits);
					    state->lens[state->have++] = here.val;
				    }
				    else {
					    if(here.val == 16) {
						    NEEDBITS_INFL(here.bits + 2);
						    DROPBITS(here.bits);
						    if(state->have == 0) {
							    strm->msg = "invalid bit length repeat";
							    state->mode = BAD;
							    break;
						    }
						    len = state->lens[state->have - 1];
						    copy = 3 + BITS(2);
						    DROPBITS(2);
					    }
					    else if(here.val == 17) {
						    NEEDBITS_INFL(here.bits + 3);
						    DROPBITS(here.bits);
						    len = 0;
						    copy = 3 + BITS(3);
						    DROPBITS(3);
					    }
					    else {
						    NEEDBITS_INFL(here.bits + 7);
						    DROPBITS(here.bits);
						    len = 0;
						    copy = 11 + BITS(7);
						    DROPBITS(7);
					    }
					    if(state->have + copy > state->nlen + state->ndist) {
						    strm->msg = "invalid bit length repeat";
						    state->mode = BAD;
						    break;
					    }
					    while(copy--)
						    state->lens[state->have++] = (ushort)len;
				    }
			    }
			    if(state->mode == BAD)  // handle error breaks in while 
					break;
			    if(state->lens[256] == 0) { // check for end-of-block code (better have one) 
				    strm->msg = "invalid code -- missing end-of-block";
				    state->mode = BAD;
				    break;
			    }
			    // build code tables -- note: do not change the lenbits or distbits
			    // values here (9 and 6) without reading the comments in inftrees.h
			    // concerning the ENOUGH constants, which depend on those values 
			    state->next = state->codes;
			    state->lencode = (const ZInfTreesCode *)(state->next);
			    state->lenbits = 9;
			    ret = inflate_table(LENS, state->lens, state->nlen, &(state->next), &(state->lenbits), state->work);
			    if(ret) {
				    strm->msg = "invalid literal/lengths set";
				    state->mode = BAD;
				    break;
			    }
			    state->distcode = (const ZInfTreesCode *)(state->next);
			    state->distbits = 6;
			    ret = inflate_table(DISTS, state->lens + state->nlen, state->ndist, &(state->next), &(state->distbits), state->work);
			    if(ret) {
				    strm->msg = "invalid distances set";
				    state->mode = BAD;
				    break;
			    }
			    Tracev((stderr, "inflate:       codes ok\n"));
			    state->mode = LEN_;
			    if(flush == Z_TREES) 
					goto inf_leave;
			case LEN_:
			    state->mode = LEN;
			case LEN:
			    if(have >= 6 && left >= 258) {
				    RESTORE();
				    inflate_fast(strm, out);
				    LOAD();
				    if(state->mode == TYPE)
					    state->back = -1;
				    break;
			    }
			    state->back = 0;
			    for(;; ) {
				    here = state->lencode[BITS(state->lenbits)];
				    if((uint)(here.bits) <= bits) 
						break;
				    PULLBYTE_INFL();
			    }
			    if(here.op && (here.op & 0xf0) == 0) {
				    last = here;
				    for(;; ) {
					    here = state->lencode[last.val + (BITS(last.bits + last.op) >> last.bits)];
					    if((uint)(last.bits + here.bits) <= bits) 
							break;
					    PULLBYTE_INFL();
				    }
				    DROPBITS(last.bits);
				    state->back += last.bits;
			    }
			    DROPBITS(here.bits);
			    state->back += here.bits;
			    state->length = (uint)here.val;
			    if((int)(here.op) == 0) {
				    Tracevv((stderr, here.val >= 0x20 && here.val < 0x7f ? "inflate:         literal '%c'\n" : "inflate:         literal 0x%02x\n", here.val));
				    state->mode = LIT;
				    break;
			    }
			    if(here.op & 32) {
				    Tracevv((stderr, "inflate:         end of block\n"));
				    state->back = -1;
				    state->mode = TYPE;
				    break;
			    }
			    if(here.op & 64) {
				    strm->msg = "invalid literal/length code";
				    state->mode = BAD;
				    break;
			    }
			    state->extra = (uint)(here.op) & 15;
			    state->mode = LENEXT;
			case LENEXT:
			    if(state->extra) {
				    NEEDBITS_INFL(state->extra);
				    state->length += BITS(state->extra);
				    DROPBITS(state->extra);
				    state->back += state->extra;
			    }
			    Tracevv((stderr, "inflate:         length %u\n", state->length));
			    state->was = state->length;
			    state->mode = DIST;
			case DIST:
			    for(;; ) {
				    here = state->distcode[BITS(state->distbits)];
				    if((uint)(here.bits) <= bits) 
						break;
				    PULLBYTE_INFL();
			    }
			    if((here.op & 0xf0) == 0) {
				    last = here;
				    for(;; ) {
					    here = state->distcode[last.val + (BITS(last.bits + last.op) >> last.bits)];
					    if((uint)(last.bits + here.bits) <= bits) 
							break;
					    PULLBYTE_INFL();
				    }
				    DROPBITS(last.bits);
				    state->back += last.bits;
			    }
			    DROPBITS(here.bits);
			    state->back += here.bits;
			    if(here.op & 64) {
				    strm->msg = "invalid distance code";
				    state->mode = BAD;
				    break;
			    }
			    state->offset = (uint)here.val;
			    state->extra = (uint)(here.op) & 15;
			    state->mode = DISTEXT;
			case DISTEXT:
			    if(state->extra) {
				    NEEDBITS_INFL(state->extra);
				    state->offset += BITS(state->extra);
				    DROPBITS(state->extra);
				    state->back += state->extra;
			    }
#ifdef INFLATE_STRICT
			    if(state->offset > state->dmax) {
				    strm->msg = "invalid distance too far back";
				    state->mode = BAD;
				    break;
			    }
#endif
			    Tracevv((stderr, "inflate:         distance %u\n", state->offset));
			    state->mode = MATCH;
			case MATCH:
			    if(left == 0) goto inf_leave;
			    copy = out - left;
			    if(state->offset > copy) { /* copy from window */
				    copy = state->offset - copy;
				    if(copy > state->whave) {
					    if(state->sane) {
						    strm->msg = "invalid distance too far back";
						    state->mode = BAD;
						    break;
					    }
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
					    Trace((stderr, "inflate.c too far\n"));
					    copy -= state->whave;
					    if(copy > state->length) copy = state->length;
					    if(copy > left) copy = left;
					    left -= copy;
					    state->length -= copy;
					    do {
						    *put++ = 0;
					    } while(--copy);
					    if(state->length == 0) state->mode = LEN;
					    break;
#endif
				    }
				    if(copy > state->wnext) {
					    copy -= state->wnext;
					    from = state->window + (state->wsize - copy);
				    }
				    else
					    from = state->window + (state->wnext - copy);
				    if(copy > state->length) copy = state->length;
			    }
			    else { // copy from output 
				    from = put - state->offset;
				    copy = state->length;
			    }
			    if(copy > left) copy = left;
			    left -= copy;
			    state->length -= copy;
			    do {
				    *put++ = *from++;
			    } while(--copy);
			    if(state->length == 0) state->mode = LEN;
			    break;
			case LIT:
			    if(left == 0) 
					goto inf_leave;
			    *put++ = (uchar)(state->length);
			    left--;
			    state->mode = LEN;
			    break;
			case CHECK:
			    if(state->wrap) {
				    NEEDBITS_INFL(32);
				    out -= left;
				    strm->total_out += out;
				    state->total += out;
				    if((state->wrap & 4) && out)
					    strm->adler = state->check = UPDATE(state->check, put - out, out);
				    out = left;
				    if((state->wrap & 4) && (
#ifdef GUNZIP
					    state->flags ? hold :
#endif
					    ZSWAP32(hold)) != state->check) {
					    strm->msg = "incorrect data check"; 
						state->mode = BAD;
					    break;
				    }
				    INITBITS();
				    Tracev((stderr, "inflate:   check matches trailer\n"));
			    }
#ifdef GUNZIP
			    state->mode = LENGTH;
			case LENGTH:
			    if(state->wrap && state->flags) {
				    NEEDBITS_INFL(32);
				    if(hold != (state->total & 0xffffffffUL)) {
					    strm->msg = "incorrect length check";
					    state->mode = BAD;
					    break;
				    }
				    INITBITS();
				    Tracev((stderr, "inflate:   length matches trailer\n"));
			    }
#endif
			    state->mode = DONE;
			case DONE:
			    ret = Z_STREAM_END;
			    goto inf_leave;
			case BAD:
			    ret = Z_DATA_ERROR;
			    goto inf_leave;
			case MEM: return Z_MEM_ERROR;
			case SYNC:
			default: return Z_STREAM_ERROR;
		}

	/*
	   Return from inflate(), updating the total counts and the check value.
	   If there was no progress during the inflate() call, return a buffer
	   error.  Call updatewindow() to create and/or update the window state.
	   Note: a memory error from inflate() is non-recoverable.
	 */
inf_leave:
	RESTORE();
	if(state->wsize || (out != strm->avail_out && state->mode < BAD && (state->mode < CHECK || flush != Z_FINISH)))
		if(updatewindow(strm, strm->next_out, out - strm->avail_out)) {
			state->mode = MEM;
			return Z_MEM_ERROR;
		}
	in -= strm->avail_in;
	out -= strm->avail_out;
	strm->total_in += in;
	strm->total_out += out;
	state->total += out;
	if((state->wrap & 4) && out)
		strm->adler = state->check = UPDATE(state->check, strm->next_out - out, out);
	strm->data_type = (int)state->bits + (state->last ? 64 : 0) + (state->mode == TYPE ? 128 : 0) + (state->mode == LEN_ || state->mode == COPY_ ? 256 : 0);
	if(((in == 0 && out == 0) || flush == Z_FINISH) && ret == Z_OK)
		ret = Z_BUF_ERROR;
	return ret;
}

int ZEXPORT inflateEnd(z_streamp strm)
{
	if(inflateStateCheck(strm))
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		if(state->window) 
			ZLIB_FREE(strm, state->window);
		ZLIB_FREE(strm, strm->state);
		strm->state = Z_NULL;
		Tracev((stderr, "inflate: end\n"));
		return Z_OK;
	}
}

int ZEXPORT inflateGetDictionary(z_streamp strm, Bytef * dictionary, uInt * dictLength)
{
	if(inflateStateCheck(strm)) // check state 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		// copy dictionary 
		if(state->whave && dictionary != Z_NULL) {
			memcpy(dictionary, state->window + state->wnext, state->whave - state->wnext);
			memcpy(dictionary + state->whave - state->wnext, state->window, state->wnext);
		}
		ASSIGN_PTR(dictLength, state->whave);
		return Z_OK;
	}
}

int ZEXPORT inflateSetDictionary(z_streamp strm, const Bytef * dictionary, uInt dictLength)
{
	if(inflateStateCheck(strm))  // check state 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		if(state->wrap != 0 && state->mode != DICT)
			return Z_STREAM_ERROR;
		else {
			// check for correct dictionary identifier 
			if(state->mode == DICT) {
				ulong  dictid = adler32(0L, Z_NULL, 0);
				dictid = adler32(dictid, dictionary, dictLength);
				if(dictid != state->check)
					return Z_DATA_ERROR;
			}
			// copy dictionary to window using updatewindow(), which will amend the
			// existing dictionary if appropriate 
			int ret = updatewindow(strm, dictionary + dictLength, dictLength);
			if(ret) {
				state->mode = MEM;
				return Z_MEM_ERROR;
			}
			else {
				state->havedict = 1;
				Tracev((stderr, "inflate:   dictionary set\n"));
				return Z_OK;
			}
		}
	}
}

int ZEXPORT inflateGetHeader(z_streamp strm, gz_headerp head)
{
	if(inflateStateCheck(strm))  // check state 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		if((state->wrap & 2) == 0) 
			return Z_STREAM_ERROR;
		else {
			// save header structure 
			state->head = head;
			head->done = 0;
			return Z_OK;
		}
	}
}
// 
// Search buf[0..len-1] for the pattern: 0, 0, 0xff, 0xff.  Return when found
// or when out of input.  When called, *have is the number of pattern bytes
// found in order so far, in 0..3.  On return *have is updated to the new
// state.  If on return *have equals four, then the pattern was found and the
// return value is how many bytes were read including the last byte of the
// pattern.  If *have is less than four, then the pattern has not been found
// yet and the return value is len.  In the latter case, syncsearch() can be
// called again with more data and the *have state.  *have is initialized to
// zero for the first call.
// 
static uint syncsearch(uint * have, const uchar  * buf, uint len)
{
	uint got = *have;
	uint next = 0;
	while(next < len && got < 4) {
		if((int)(buf[next]) == (got < 2 ? 0 : 0xff))
			got++;
		else if(buf[next])
			got = 0;
		else
			got = 4 - got;
		next++;
	}
	*have = got;
	return next;
}

int ZEXPORT inflateSync(z_streamp strm)
{
	if(inflateStateCheck(strm)) // check parameters 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		if(strm->avail_in == 0 && state->bits < 8) 
			return Z_BUF_ERROR;
		else {
			uint len; // number of bytes to look at or looked at 
			// if first time, start search in bit buffer 
			if(state->mode != SYNC) {
				uchar  buf[4]; // to restore bit buffer to byte string 
				state->mode = SYNC;
				state->hold <<= state->bits & 7;
				state->bits -= state->bits & 7;
				len = 0;
				while(state->bits >= 8) {
					buf[len++] = (uchar)(state->hold);
					state->hold >>= 8;
					state->bits -= 8;
				}
				state->have = 0;
				syncsearch(&(state->have), buf, len);
			}
			// search available input 
			len = syncsearch(&(state->have), strm->next_in, strm->avail_in);
			strm->avail_in -= len;
			strm->next_in += len;
			strm->total_in += len;
			// return no joy or set up to restart inflate() on a new block 
			if(state->have != 4) 
				return Z_DATA_ERROR;
			else {
				const ulong in = strm->total_in;  // temporary to save total_in
				const ulong out = strm->total_out; // temporary to save total_out 
				inflateReset(strm);
				strm->total_in = in;  
				strm->total_out = out;
				state->mode = TYPE;
				return Z_OK;
			}
		}
	}
}
// 
// Returns true if inflate is currently at the end of a block generated by
// Z_SYNC_FLUSH or Z_FULL_FLUSH. This function is used by one PPP
// implementation to provide an additional safety check. PPP uses
// Z_SYNC_FLUSH but removes the length bytes of the resulting empty stored
// block. When decompressing, PPP checks that at the end of input packet,
// inflate is waiting for these length bytes.
// 
int ZEXPORT inflateSyncPoint(z_streamp strm)
{
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		return state->mode == STORED && state->bits == 0;
	}
}

int ZEXPORT inflateCopy(z_streamp dest, z_streamp source)
{
	struct inflate_state  * state;
	struct inflate_state  * copy;
	uchar * window;
	uint wsize;
	// check input 
	if(inflateStateCheck(source) || dest == Z_NULL)
		return Z_STREAM_ERROR;
	state = (struct inflate_state *)source->state;
	// allocate space 
	copy = (struct inflate_state *)ZLIB_ALLOC(source, 1, sizeof(struct inflate_state));
	if(copy == Z_NULL) 
		return Z_MEM_ERROR;
	window = Z_NULL;
	if(state->window != Z_NULL) {
		window = (uchar *)ZLIB_ALLOC(source, 1U << state->wbits, sizeof(uchar));
		if(!window) {
			ZLIB_FREE(source, copy);
			return Z_MEM_ERROR;
		}
	}
	// copy state 
	memcpy((void *)dest, (void *)source, sizeof(z_stream));
	memcpy((void *)copy, (void *)state, sizeof(struct inflate_state));
	copy->strm = dest;
	if(state->lencode >= state->codes && state->lencode <= state->codes + ENOUGH - 1) {
		copy->lencode = copy->codes + (state->lencode - state->codes);
		copy->distcode = copy->codes + (state->distcode - state->codes);
	}
	copy->next = copy->codes + (state->next - state->codes);
	if(window) {
		wsize = 1U << state->wbits;
		memcpy(window, state->window, wsize);
	}
	copy->window = window;
	dest->state = (struct internal_state *)copy;
	return Z_OK;
}

int ZEXPORT inflateUndermine(z_streamp strm, int subvert)
{
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
		state->sane = !subvert;
		return Z_OK;
#else
		(void)subvert;
		state->sane = 1;
		return Z_DATA_ERROR;
#endif
	}
}

int ZEXPORT inflateValidate(z_streamp strm, int check)
{
	if(inflateStateCheck(strm)) 
		return Z_STREAM_ERROR;
	else {
		struct inflate_state * state = (struct inflate_state *)strm->state;
		/* @sobolev
		if(check)
			state->wrap |= 4;
		else
			state->wrap &= ~4;
		*/
		SETFLAG(state->wrap, 4, check); // @sobolev
		return Z_OK;
	}
}

long ZEXPORT inflateMark(z_streamp strm)
{
	if(inflateStateCheck(strm))
		return -(1L << 16);
	else {
		struct inflate_state * state = reinterpret_cast<struct inflate_state *>(strm->state);
		return (long)(((ulong)((long)state->back)) << 16) + (state->mode == COPY ? state->length : (state->mode == MATCH ? state->was - state->length : 0));
	}
}

ulong ZEXPORT inflateCodesUsed(z_streamp strm)
{
	if(inflateStateCheck(strm)) 
		return (ulong)-1;
	else {
		struct inflate_state * state = reinterpret_cast<struct inflate_state *>(strm->state);
		return (ulong)(state->next - state->codes);
	}
}
