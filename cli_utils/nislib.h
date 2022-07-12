/* collection of funcs for working with Nissan ROMs
 * (c) fenugrec 2014-2022
 * GPLv3
 */

#ifndef NISLIB_H
#define NISLIB_H

#include <stdio.h>	//just for FILE
#include <stdint.h>
#include <stdbool.h>

#include "stypes.h"


#define MIN(_a_, _b_) (((_a_) < (_b_) ? (_a_) : (_b_)))
#define MAX(_a_, _b_) (((_a_) > (_b_) ? (_a_) : (_b_)))
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))

#define	MIN_ROMSIZE (128*1024UL)	//smallest known ROM is SH7050, 128kB
#define MAX_ROMSIZE (2048*1024UL)

/* this needs to be valid; debugging output is written to this.*/
extern FILE *dbg_stream;	//such as as stdout or stderr

/** get file length but restore position */
uint32_t flen(FILE *hf);

/** Read uint32 at *buf with SH endianness
*/
uint32_t reconst_32(const uint8_t *buf);

/** Read uint16 at *buf with SH endianness
*/
uint16_t reconst_16(const uint8_t *buf);

/** write uint32 at *buf with SH endianness
*/
void write_32b(uint32_t val, uint8_t *buf);

/** search a <buflen> u8 buffer for a <len>-byte long sequence.
 *
 * @param buflen size in bytes of *buf
 * @param needle pattern to search
 * @param nlen size of "needle" pattern
 * @return NULL if not found
 *
 * Painfully unoptimized, because it's easy to get it wrong
 */
const uint8_t *u8memstr(const uint8_t *buf, uint32_t buflen, const uint8_t *needle, unsigned nlen);

/** search for an aligned, big-endian u16 value. */
const uint8_t *u16memstr(const uint8_t *buf, uint32_t buflen, const uint16_t needle);

/** same as u16memstr but searches backwards starting at buf[start_pos] */
const uint8_t *u16memstr_rev(const uint8_t *buf, uint32_t start_pos, const uint16_t needle);

/** search a <buflen> u8 buffer for a 32-bit aligned uint32_t value, in SH endianness
 *
 */
const uint8_t *u32memstr(const uint8_t *buf, uint32_t buflen, const uint32_t needle);



/* "security" algorithms */

/** Encode uint32_t data, algo 1
 * @param data uint32_t value to encode
 * @param scode key to use
 */
uint32_t enc1(uint32_t data, uint32_t scode);

/** Decode uint32_t data, algo 1
 * @param data uint32_t value to decode
 * @param scode key to use
 */
uint32_t dec1(uint32_t data, uint32_t scode);

/* key stuff */

enum key_type {
	KEY_S27 = 0,	//SID27 key
	KEY_S36K1,	//SID36 kernel key
	KEY_S36K2,	//SID36 factory payload key (less useful)
	KEY_INVALID,
};

struct keyset_t {
	uint32_t s27k;
	uint32_t s36k1;
	uint32_t s36k2;
	};

extern const struct keyset_t known_keys[];


/** try to see if candidate matches one known keyset.
 *
 * @param candidate must not be 0
 *
 * return NULL if not found
 */
const struct keyset_t *find_knownkey(enum key_type ktype, u32 candidate);

/** Sum and xor all uint32_t values in *buf, read with SH endianness
 * @param [out] *xor
 * @param [out] *sum
 */
void sum32(const uint8_t *buf, uint32_t siz, uint32_t *sum, uint32_t *xor);

/** calculate checksums and find their location
 *
 * @param siz : size of *buf in bytes
 * @param p_cks (output) sum location (offset in buf)
 * @param p_ckx (output) xor location (offset in buf)
 * @return 0 if ok
 *
 * Uses "standard" algo
 */
int checksum_std(const uint8_t *buf, uint32_t siz, uint32_t *p_cks, uint32_t *p_ckx);

/** calculate alt2 checksum and find its location
 *
 * @param siz : size of *buf in bytes
 * @param p_ack_s : sum location (offset in buf)
 * @param p_ack_x : xor location (offset in buf)
 * @param p_skip1 : if != UINT32_MAX: first location to skip (offset in buf)
 * @param p_skip2 : if != UINT32_MAX: second location to skip (offset in buf)
 *
 * @return 0 if ok
 *
 * This is the same algo as "std checksum", but assumes a loop that
 * skips 4 locations (sum,xor, skip1, skip2).
 */
int checksum_alt2(const uint8_t *buf, uint32_t siz, uint32_t *p_ack_s, uint32_t *p_ack_x,
				uint32_t p_skip1, uint32_t p_skip2);

/** Calculate correction values to give a known checksum
 *
 * @param siz length of *buf, in bytes
 * @param p_cks location of original sum that is to be matched
 * @param p_ckx location of original xor that is to be matched
 * @param p_a location, in *buf, of first correction value
 * @param p_b location, in *buf, of second correction value
 * @param p_c location, in *buf, of third correction value
 *
 * This calculates and sets values a,b,c so that the checksums match the original sum and xor.
 */
void checksum_fix(uint8_t *buf, uint32_t siz, uint32_t p_cks, uint32_t p_ckx,
		uint32_t p_a, uint32_t p_b, uint32_t p_c);


/** Verify if a vector table (IVT) is sane.
 * @param ivt : start of vector table
 * @param siz : bytes in buf[]
 * @return true if it's an IVT.
 *
 * Uses very basic heuristics :
 * - check if the power-on and manual resets have the same values for PC and SP
 * - PC points in bottom 16MB, aligned on 2-byte boundary
 * - SP points in RAM (top 128kB), aligned on 4-byte boundary
 *
 * Example of a valid IVT : 0000 0104, ffff 7ffc, 0000 0104, ffff 7ffc
 */
bool check_ivt(const uint8_t *buf, unsigned siz);

#define IVT_MINSIZE 0x100	//absolute minimum for a trimmed IVT on 705x

/** find a likely vector table (IVT)
 * @param siz length of *buf, in bytes
 * @return offset of IVT if successful, -1 otherwise
 */
uint32_t find_ivt(const uint8_t *buf, uint32_t siz);


/** find EEPROM read_byte(addr, &dest) function address and IO port used
 * returns address of eepread() function, otherwise 0 if nothing found
 */
uint32_t find_eepread(const uint8_t *buf, uint32_t siz, uint32_t *real_portreg);


#endif // NISLIB_H
