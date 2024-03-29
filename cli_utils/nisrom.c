/* (c) fenugrec 2015-2022
 * Gather information about a ROM, from metadata and heuristics
 * GPLv3
 *
 * show usage:
 * nisrom -h
 */

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>	//for offsetof()
#include <stdint.h>
#include <stdio.h>
#include <string.h>	//memcmp
#include <stdlib.h>	//malloc etc

#include <getopt.h>

#include "md5/md5.h"	//we could use libmd or a wrapper around windows' CryptAcquireContext() but this is simpler.
#include "uthash/utstring.h"

#include "nissan_romdefs.h"
#include "nislib.h"
#include "nislib_shtools.h"
#include "nisrom_finders.h"
#include "nisrom_keyfinders.h"
#include "nis_romdb.h"

#include "stypes.h"


#define DBG_OUTFILE	"nisrom_dbg.log"	//default log file
#define KEYSET_CSV "../romdb/keysets.csv"	//default keyset db file

#if (CHAR_BIT != 8)
#error HAH ! a non-8bit char system. Some of this will not work
#endif

const char *progname="nisrom";

FILE *dbg_stream;

static bool force_parse = 0;	//force parsing a ROM, ignoring errors as much as possible. Can cause segfaults

// generic ROM struct. For the file offsets in here, UINT32_MAX signals invalid / inexistant target
struct romfile {
	FILE *hf;
	const char *filename;
	u32 siz;	//in bytes
	uint8_t *buf;	//copied here

	nis_romdb *romdb;

	//and some metadata
	rom_offset p_loader;	//struct loader
	enum loadvers_t loader_v;	//version (10, 50, 60 etc)

	rom_offset p_fid;	//location of struct fid_base
	enum fidtype_ic fid_ic;
	const struct fidtype_t *fidtype;

	u32 sfid_size;	//sizeof correct struct fid_base
	rom_offset p_ramf;	//location of struct ramf
	rel_offset	ramf_offset;	//RAMF struct wasn't found where expected (offset != 0)

	/* these point in buf, and so are not necessarily 0-terminated strings */
	const uint8_t *loader_cpu;	//LOADERxx CPU code
	const uint8_t *fid;	//firmware ID itself
	const uint8_t *fid_cpu;	//FID CPU code

	rom_offset p_cks;	//position of std_checksum sum
	rom_offset p_ckx;	//position of std_checksum xor

	rom_offset p_acs;	//pos of alt_cks sum
	rom_offset p_acx;	//pos of alt_cks xor

	rom_offset p_a2cs;
	rom_offset p_a2cx;	//pos of alt2 cks sum, xor

	/* real metadata here. Unknown values must be set to UINT32_MAX */
	rom_offset p_ivt2;	//pos of alt. vector table
	rom_offset p_acstart;	//start of alt_cks block
	rom_offset p_acend;	//end of alt_cks block
	rom_offset p_ecurec;	//if ROM_HAS_ECUREC

	rom_offset p_ac2start;	//start of alt2 cks block (end is always ROMEND ?)

	rom_offset	p_eepread;	//address of eeprom_read() func
	uint32_t	eep_port;	//PORT reg used for EEPROM pins

	/* some flags */
	bool	cks_alt_good;	//alt cks values found + valid
	bool	cks_alt2_good;	//alt2 cks values found + valid
	bool	has_rm160;	//RIPEMD160 hash found

	struct ramf_unified ramf;	//not useful atm
};


//load ROM to a new buffer
//ret 0 if OK
//caller MUST call close_rom() after
static int open_rom(struct romfile *rf, const char *fname) {
	FILE *fbin;
	uint8_t *buf;	//load whole ROM
	u32 file_len;

	rf->hf = NULL;	//not needed

	if ((fbin=fopen(fname,"rb"))==NULL) {
		ERR_PRINTF("error opening %s.\n", fname);
		return -1;
	}
	rf->filename = fname;

	file_len = flen(fbin);
	if ((!file_len) ||
		(file_len > MAX_ROMSIZE) ||
		(file_len < MIN_ROMSIZE)) {
		ERR_PRINTF("unlikely file size %lu\n", (unsigned long) file_len);
		if (!force_parse) {
			fclose(fbin);
			return -1;
		}
	}
	rf->siz = file_len;

	buf = malloc(file_len);
	if (!buf) {
		ERR_PRINTF("malloc choke\n");
		fclose(fbin);
		return -1;
	}

	/* load whole ROM */
	if (fread(buf,1,file_len,fbin) != file_len) {
		ERR_PRINTF("trouble reading\n");
		free(buf);
		fclose(fbin);
		return -1;
	}
	rf->buf = buf;

	fclose(fbin);

	return 0;
}

/** close & free romfile contents
 *
 * safe to call multiple times or if nothing is open yet
 */
void close_rom(struct romfile *rf) {
	if (!rf) return;
	if (rf->buf) {
		free(rf->buf);
		rf->buf = NULL;
	}
	return;
}


/** try to extract an ECUID from given full filename
 *
 * @param [out] ecuid if found, write 5+1 bytes here (includes 0 term)
 * @param filename can be full abs/rel path, or just filename. Must be 0-terminated
 *
 * return 1 if ok
*/
static bool ecuid_from_filename(const char *filename, char *ecuid) {
	// first : search backwards for a fwd/back slash
	// pathological test cases : "/", "abc"
	const char *pfile;
    pfile = filename + strlen(filename);
    for (; pfile > filename; pfile--) {
        if ((*pfile == '\\') || (*pfile == '/')) {
            pfile++;
            break;
        }
    }

    unsigned basename_len = strlen(pfile);
    if (basename_len < ECUID_LEN) {
		return 0;
    }

    // make temp copy because strtok needs a writeable char *
    char tmp_basename[1 + ECUID_LEN +2] =  {0};	//possible '1' prefix, + separator, + 0-term
	strncpy(tmp_basename, pfile, ECUID_LEN+2);

    // next : take first "token" of filename as the ECUID
    char *tok = strtok(tmp_basename, "-_. ");
    if (!tok) {
		return 0;
    }
    unsigned tok_len = strlen(tok);
    if ((tok_len != ECUID_LEN) &&
		(tok_len != (1 + ECUID_LEN))) {
		return 0;
    }

    //validate chars and make uppercase : 0-9, a-z, A-Z
    for (unsigned idx = 0; idx < tok_len; idx++) {
		if (!isalnum(tok[idx])) {
			return 0;
		}
		tok[idx] = toupper(tok[idx]);
    }


    if ((tok_len == (ECUID_LEN + 1)) &&
		(tok[0] == '1')) {
		// 6-char string starting with '1' , e.g. 18U92A
		memcpy(ecuid, &tok[1], ECUID_STR_LEN);
		return 1;
	} else if (tok_len == ECUID_LEN) {
		//normal ECUID
		memcpy(ecuid, tok, ECUID_STR_LEN);
		return 1;
	}

	return 0;
}

/** find offset of LOADER struct if possible, update romfile struct
 * ret -1 if not ok
 */
u32 find_loader(struct romfile *rf) {
	const uint8_t loadstr[]="LOADER";
	const uint8_t *sl;
	int loadv;

	if (!rf) return -1;
	if (!(rf->buf)) return -1;

	rf->loader_v = L_UNK;

	/* look for "LOADER", backtrack to beginning of struct. */
	sl = u8memstr(rf->buf, rf->siz, loadstr, 6);
	if (!sl) {
		DBG_PRINTF("LOADER not found !\n");
		return -1;
	}

	//decode version #
	if (sscanf((const char *) (sl + 6), "%d", &loadv) == 1) {
		rf->loader_v = loadv;
	}

	//convert to file offset
	rf->p_loader = (u32) (sl - rf->buf) - offsetof(struct loader_t, loader);

	// this is the same for all loader verions:
	rf->loader_cpu = &rf->buf[rf->p_loader + offsetof(struct loader_t, cpu)];

	return rf->p_loader;
}

//parse second part of FID struct and fill altcks + ivt2 + rf->ramf-> stuff
static void parse_ramf(struct romfile *rf) {
	const struct fidtype_t *ft;	//helper
	assert(rf);

	ft = rf->fidtype;
	unsigned features = ft->features;	//another helper

	if (ft->pRAMjump) {
		rf->ramf.pRAMjump = reconst_32(&rf->buf[rf->p_ramf + ft->pRAMjump]);
		rf->ramf.pRAM_DLAmax = reconst_32(&rf->buf[rf->p_ramf + ft->pRAM_DLAmax]);
	}

	if (features & ROM_HAS_ALTCKS) {
		//gross : find_romend may have filled these in
		if ((rf->p_acstart == 0) &&
				(rf->p_acend == 0)) {
			assert(ft->packs_start);
			rf->p_acstart = reconst_32(&rf->buf[rf->p_ramf + ft->packs_start]);
			rf->p_acend = reconst_32(&rf->buf[rf->p_ramf + ft->packs_end]);
		}
	} else {
		rf->p_acstart = UINT32_MAX;
		rf->p_acend = UINT32_MAX;
	}

	if (ft->pIVT2) {
		// same : find_romend may have filled in
		if (rf->p_ivt2 == 0) {
			rf->p_ivt2 = reconst_32(&rf->buf[rf->p_ramf + ft->pIVT2]);
		}
	} else {
		rf->p_ivt2 = UINT32_MAX;
	}

	return;
}


//find offset of FID struct, parse & update romfile struct. find_loader() must have been run before calling this.
//ret -1 if not ok
u32 find_fid(struct romfile *rf) {
	const uint8_t dbstr[]="DATAB";
	const uint8_t loadstr[]="LOADER";
	const uint8_t *sf;
	u32 sf_offset;	//offset in file

	if (!rf) return -1;
	if (!(rf->buf)) return -1;

	rf->fid_ic = FID_UNK;

	/* look for "DATABASE" */
	sf = u8memstr(rf->buf, rf->siz, dbstr, 5);
	if (!sf) {
		DBG_PRINTF("no DATABASE found !?\n");
		return -1;
	}
	//convert to file offset
	//Luckily, the database member is at the same offset for all variants of struct fid.
	sf_offset = (sf - rf->buf) - offsetof(struct fid_base1_t, database);

	/* check if this was the LOADER database */
	if (memcmp(sf - offsetof(struct loader_t, database), loadstr, 4) == 0 ) {
		//search again, skipping the first instance
		if ((sf_offset + 6) < rf->siz) {
			sf = u8memstr(&rf->buf[sf_offset + sizeof(struct loader_t)], rf->siz - sf_offset - 1 , dbstr, 5);
		}
		if (!sf) {
			DBG_PRINTF("no FID DATABASE found !\n");
			return -1;
		}
		//convert to file offset again
		sf_offset = (sf - rf->buf) - offsetof(struct fid_base1_t, database);
	}

	//bounds check
	if ((sf_offset + FID_MAXSIZE) >= rf->siz) {
		DBG_PRINTF("Possibly incomplete / bad dump ? FID too close to end of ROM\n");
		return -1;
	}

	rf->p_fid = sf_offset;

	/* independant of loader version : */
	rf->fid = &rf->buf[sf_offset + offsetof(struct fid_base1_t, FID)];
	rf->fid_cpu = &rf->buf[sf_offset + offsetof(struct fid_base1_t, cpu)];

	/* determine FID type : iterate through array of known types, matching the CPU string */
	rf->fid_ic = get_fidtype(rf->fid_cpu);
	if (rf->fid_ic == FID_UNK) {
		DBG_PRINTF("Unknown FID IC type %.8s ! Cannot proceed\n", rf->fid_cpu);
		return -1;
	}

	rf->fidtype = &fidtypes[rf->fid_ic];
	if (rf->siz != (rf->fidtype->ROMsize)) {
		DBG_PRINTF("Warning : ROM size %u k, expected %u k; possibly incomplete dump\n",
				rf->siz / 1024, rf->fidtype->ROMsize / 1024);
	}

	rf->sfid_size = rf->fidtype->FIDbase_size;

	return sf_offset;
}

/** validate alt cks block in pre-parsed romfile
 * needs (features & ROM_HAS_ALTCKS)
 *
 * @return 0 if ok
 */
int validate_altcks(struct romfile *rf) {
	/* validate alt_cks block; it's a std algo that skips 2 u32 locs (altcks_s, altcks_x).
	 * But it seems those locs are always outside the block?
	 */
	uint32_t acs=0, acx=0;
	u32 altcs_bsize;
	const uint8_t *pacs, *pacx;

	assert(rf);
	assert(rf->buf);
	if (!(rf->fidtype->features & ROM_HAS_ALTCKS)) return -1;

	if ((rf->p_acstart == UINT32_MAX) ||
		(rf->p_acend == UINT32_MAX) ||
		(rf->p_acstart >= rf->p_acend)) {
		return -1;
	}

	/* p_acstart is so far always u32 aligned, but not p_acend (usually 2 bytes before FID, except on some SH705828 ROMs....
	 * This gives rise to some weird behavior where
	 * sometimes the cks area includes the first u32 of the FID struct. I wonder if this was really intended by the Nissan devs !
	 */
	altcs_bsize = (((rf->p_acend + 1) - rf->p_acstart) & (~0x03)) + 4;

	sum32(&rf->buf[rf->p_acstart], altcs_bsize, &acs, &acx);

	DBG_PRINTF("alt cks block 0x%06lX - 0x%06lX: sumt=0x%08lX, xort=0x%08lX\n",
		(unsigned long) rf->p_acstart, (unsigned long) rf->p_acend,
			(unsigned long) acs, (unsigned long) acx);
	pacs = u32memstr(rf->buf, rf->siz, acs);
	pacx = u32memstr(rf->buf, rf->siz, acx);
	if (!pacs || !pacx) {
		DBG_PRINTF("altcks values not found in ROM, possibly unskipped vals or bad algo\n");
		return -1;
	} else {
		rf->p_acs = (u32) (pacs - rf->buf);
		rf->p_acx = (u32) (pacx - rf->buf);
		DBG_PRINTF("confirmed altcks values found : acs @ 0x%lX, acx @ 0x%lX\n",
				(unsigned long) rf->p_acs, (unsigned long) rf->p_acx);
		rf->cks_alt_good = 1;
		//TODO : validate altcks val offsets VS end-of-IVT2, i.e. they seem to be always @
		// IVT2 + 0x400
	}
	return 0;
}


/* if ROM_HAS_ECUREC, try to locate &IVT2 near ROMEND
 *
 * ret 1 if ok and update romfile struct
 */
bool find_ecurec(struct romfile *rf) {
	const struct fidtype_t *ft;
	assert(rf);

	ft = rf->fidtype;
	if (!(ft->features & ROM_HAS_ECUREC)) {
		return 0;
	}

	bool found = 0;
	rom_offset p_romend, temp_ivt2, pp_ecurec;

	u32 start_offs = 0;
	const u8 *ivt2_maybe = rf->buf;
	for (; start_offs < (rf->siz - 100); start_offs = (u32) (ivt2_maybe - rf->buf)+4) {
			// iterate over occurences of &IVT2
		ivt2_maybe = u32memstr(&rf->buf[start_offs], rf->siz - start_offs, ft->IVT2_expected);
		if (!ivt2_maybe) {
			return 0;
		}
		temp_ivt2 = (rom_offset) (ivt2_maybe - rf->buf);
		pp_ecurec = temp_ivt2 - ft->pIVT2;
		p_romend = pp_ecurec + ft->pROMend;
		if (p_romend >= (rf->siz - 4)) {
			continue;
		}
		u32 romend = reconst_32(&rf->buf[p_romend]);
		if ((romend + 1) != (ft->ROMsize)) {
			//IVT2/ROMEND field mismatch
			continue;
		}
		//found !
		found = 1;
		break;
	}
	if (!found) {
		DBG_PRINTF("IVT2/ROMEND not found\n");
		return 0;
	}
	rf->p_ivt2 = ft->IVT2_expected;
	rf->p_acstart = reconst_32(&rf->buf[pp_ecurec + ft->packs_start]);
	rf->p_acend = reconst_32(&rf->buf[pp_ecurec + ft->packs_end]);
	rf->p_ecurec = reconst_32(&rf->buf[pp_ecurec]);
	return 1;
}


/** find & analyze 'struct ramf'
 *
 * @return 0 if ok
 *
 * it's right after struct fid, easy. The rom must already have
 * loader and fid structs found (find_loader, find_fid)
 */

u32 find_ramf(struct romfile *rf) {
	uint32_t testval;
	const struct fidtype_t *ft;	//helper

	assert(rf);
	if ((rf->fid_ic == FID_UNK) ||
		(rf->fid_ic >= FID_MAX) ||
		(rf->p_fid == UINT32_MAX)) {
		 return -1;
	}

	rf->p_ramf = rf->p_fid + rf->sfid_size;
	ft = rf->fidtype;
	unsigned features = ft->features;	//helper

	if (ft->RAMF_header == 0) {
		bool found_stuff = 0;
		if (features & ROM_HAS_ECUREC) {
			// alternate structure : no RAMF, instead search for &IVT2 near ROMEND
			found_stuff = find_ecurec(rf);
		}
		if (!found_stuff) {
			DBG_PRINTF("not trying to find RAMF.\n");
			return 0;
		}
	} else {
		// try to find RAMF by looking for first member, typically FFFF8000.
		testval = reconst_32(&rf->buf[rf->p_ramf]);
		if (testval != ft->RAMF_header) {
			long ramf_adj = 4;
			long sign = 1;
			DBG_PRINTF("Unlikely contents for struct ramf; got 0x%lX.\n",
						(unsigned long) testval);
			while (ramf_adj < ft->pRAMF_maxdist) {
				//search around, in a pattern like +4, -4, +8, -8, +12  and then +16, +20 etc
				testval = reconst_32(&rf->buf[rf->p_ramf + (sign * ramf_adj)]);
				if (testval == ft->RAMF_header) {
					DBG_PRINTF("probable RAMF found @ delta = %+d\n",
								(int) (sign * ramf_adj));
					rf->ramf_offset = (sign * ramf_adj);
					rf->p_ramf += rf->ramf_offset;
					break;
				}
				if (ramf_adj < 0x0c) {
					sign = -sign;	//flip sign;
					if (sign == 1) ramf_adj += 4;
				} else {
					sign = 1;
					ramf_adj += 4;
				}
			}
		}
	}

	parse_ramf(rf);

	if (features & ROM_HAS_ALTCKS) {
		if ((rf->p_acstart >= rf->siz) ||
			(rf->p_acend >= rf->siz) ||
			(rf->p_acstart >= rf->p_acend)) {
			DBG_PRINTF("bad alt cks bounds; 0x%lX - 0x%lX\n",
					(unsigned long) rf->p_acstart, (unsigned long) rf->p_acend);
			rf->p_acstart = UINT32_MAX;
			rf->p_acend = UINT32_MAX;
		}
		if (rf->p_acstart != UINT32_MAX) {
			(void) validate_altcks(rf);
		}
	}

	if (rf->p_ivt2 != UINT32_MAX) {
		if (rf->p_ivt2 >= (rf->siz - IVT_MINSIZE)) {
			DBG_PRINTF("warning : IVT2 value out of bound, probably due to unusual RAMF structure.\n");
			rf->p_ivt2 = UINT32_MAX;
		} else {
			if (rf->p_ivt2 != ft->IVT2_expected) {
				DBG_PRINTF("Unexpected IVT2 0x%lX ! Please report this\n", (unsigned long) rf->p_ivt2);
			}
			if (!check_ivt(&rf->buf[rf->p_ivt2], rf->siz - rf->p_ivt2)) {
				DBG_PRINTF("Unlikely IVT2 location 0x%06lX :\n", (unsigned long) rf->p_ivt2);
				DBG_PRINTF("%08lX %08lX %08lX %08lX...\n", (unsigned long) reconst_32(&rf->buf[rf->p_ivt2+0]),
							(unsigned long) reconst_32(&rf->buf[rf->p_ivt2+4]),
							(unsigned long) reconst_32(&rf->buf[rf->p_ivt2+8]),
							(unsigned long) reconst_32(&rf->buf[rf->p_ivt2+12]));
				rf->p_ivt2 = UINT32_MAX;	//run the bruteforce IVT2 search instead
			}
		}
	}

	// edge case for 705822 which does have ECUREC but still uses the "normal" method : need to define p_ecurec manually here
	if (!(features & ROM_HAS_ECUREC)) {
		rf->p_ecurec = reconst_32(&rf->buf[rf->p_ramf + ft->pECUREC]);
	}

	rom_offset pecurec = rf->p_ecurec;

	//display some LOADER > 80 specific garbage
	if (features & ROM_HAS_ECUREC) {
		//parse ECUREC
		if ((pecurec + 6) >= rf->siz) {
			DBG_PRINTF("unlikely pecurec = %lX\n", (unsigned long) pecurec);
			pecurec = UINT32_MAX;
		} else {
			//skip leading '1'
			DBG_PRINTF("probable ECUID @ %lX: %.*s\n",
					(unsigned long) pecurec, 5,  &rf->buf[pecurec + 1]);
		}
	}

	/* Locate RIPEMD-160 magic numbers */
	if ((u32memstr(rf->buf, rf->siz, 0x67452301) != NULL) &&
		(u32memstr(rf->buf, rf->siz, 0x98BADCFE) != NULL)) {
		rf->has_rm160 = 1;
	}

	/* Locate cks_alt2 checksum. Starts at ECUREC */
	if ((features & ROM_HAS_ALT2CKS) &&
		(pecurec < rf->siz) &&
		(rf->p_ivt2 < rf->siz)) {

		u32 p_as = 0, p_ax = 0;
		u32 p_skip1, p_skip2;
		p_skip1 = UINT32_MAX;
		p_skip2 = (rf->p_ivt2 - 4) - pecurec;
		rf->p_ac2start = pecurec;
		if (checksum_alt2(&rf->buf[pecurec], rf->siz - pecurec, &p_as, &p_ax, p_skip1, p_skip2) == 0) {
			rf->cks_alt2_good = 1;
			rf->p_a2cs = p_as + pecurec;
			rf->p_a2cx = p_ax + pecurec;
		} else {
			DBG_PRINTF("alt2 checksum not found ?? Bad algo, bad skip, or other problem...\n");
		}
	}

	return rf->p_ramf;
}



void find_eep(struct romfile *rf) {
	uint32_t port;
	uint32_t eepread = find_eepread(rf->buf, rf->siz, &port);
	if (eepread > 0) {
		rf->p_eepread = eepread;
		rf->eep_port = port;
	}
}

// for every property that will be shown, fill one of these
struct printable_prop {
	const char *csv_name;	//CSV column header. NULL to mark end of array
	UT_string rendered_value;	//either quoted string or numeric value.
};

enum rom_properties {
	RP_ECUID = 0,
	RP_FILE,
	RP_SIZE,
	RP_LOADER,
	RP_LOADER_OFS,
	RP_LOADER_CPU,
	RP_LOADER_CPUCODE,
	RP_FID,
	RP_FID_OFS,
	RP_FID_CPU,
	RP_FID_CPUCODE,
	RP_RAMF_WEIRD,
	RP_RAMJUMP,
	RP_IVT2,
	RP_IVT2_CONF,
	RP_STD_CKS,
	RP_STD_S_OFS,
	RP_STD_X_OFS,
	RP_ALT_CKS,
	RP_ALT_S_OFS,
	RP_ALT_X_OFS,
	RP_ALT_START,
	RP_ALT_END,
	RP_ALT2_CKS,
	RP_ALT2_S_OFS,
	RP_ALT2_X_OFS,
	RP_ALT2_START,
	RP_RIPEMD160,
	RP_KEYSET_QUAL,
	RP_S27K,
	RP_S36K,
	RP_EEP_READ_OFFS,
	RP_EEP_PORT,
	RP_MD5,
	RP_MAX,	//not a property, just the end marker
};

const struct printable_prop props_template[] = {
	[RP_ECUID] = {"ECUID", {0}},
	[RP_FILE] = {"file", {0}},
	[RP_SIZE] = {"size", {0}},
	[RP_LOADER] = {"LOADER ##", {0}},
	[RP_LOADER_OFS] = {"LOADER ofs", {0}},
	[RP_LOADER_CPU] = {"LOADER CPU", {0}},
	[RP_LOADER_CPUCODE] = {"LOADER CPUcode", {0}},
	[RP_FID] = {"FID", {0}},
	[RP_FID_OFS] = {"&FID", {0}},
	[RP_FID_CPU] = {"FID CPU", {0}},
	[RP_FID_CPUCODE] = {"FID CPUcode", {0}},
	[RP_RAMF_WEIRD] = {"RAMF_weird", {0}},
	[RP_RAMJUMP] = {"RAMjump_entry", {0}},
	[RP_IVT2] = {"IVT2", {0}},
	[RP_IVT2_CONF] = {"IVT2 confidence", {0}},
	[RP_STD_CKS] = {"std cks?", {0}},
	[RP_STD_S_OFS] = {"&std_s", {0}},
	[RP_STD_X_OFS] = {"&std_x", {0}},
	[RP_ALT_CKS] = {"alt cks?", {0}},
	[RP_ALT_S_OFS] = {"&alt_s", {0}},
	[RP_ALT_X_OFS] = {"&alt_x", {0}},
	[RP_ALT_START] = {"alt_start", {0}},
	[RP_ALT_END] = {"alt_end", {0}},
	[RP_ALT2_CKS] = {"alt2 cks?", {0}},
	[RP_ALT2_S_OFS] = {"&alt2_s", {0}},
	[RP_ALT2_X_OFS] = {"&alt2_x", {0}},
	[RP_ALT2_START] = {"alt2_start", {0}},
	[RP_RIPEMD160] = {"RIPEMD160", {0}},
	[RP_KEYSET_QUAL] = {"keyset quality", {0}},
	[RP_S27K] = {"s27k", {0}},
	[RP_S36K] = {"s36k1", {0}},
	[RP_EEP_READ_OFFS] = {"&EEPROM_read()", {0}},
	[RP_EEP_PORT] = {"EEPROM PORT", {0}},
	[RP_MD5] = {"MD5", {0}},
	[RP_MAX] = {NULL, {0}},
};

// some fwd decls

static void free_properties(struct printable_prop *props);


static void print_csv_header(const struct printable_prop *props) {
	assert(props);
	const struct printable_prop *prop = props;

	//print first field separately to avoid extra field separator
	printf("\"%s\"", prop->csv_name);
	prop++;
	while (prop->csv_name != NULL) {
		printf(",\"%s\"", prop->csv_name);
		prop++;
	}
	printf("\n");
	return;
}

static void print_csv_values(const struct printable_prop *props) {
	assert(props);
	const struct printable_prop *prop = props;

	printf("%s", utstring_body(&prop->rendered_value));
	prop++;
	while (prop->csv_name != NULL) {
		printf(",%s", utstring_body(&prop->rendered_value));
		prop++;
	}
	printf("\n");
}

static void print_human(const struct printable_prop *props) {
	assert(props);
	const struct printable_prop *prop = props;

	while (prop->csv_name != NULL) {
		printf("\n%s\t", prop->csv_name);
		printf("%s", utstring_body(&prop->rendered_value));
		prop++;
	}
	printf("\n");
}


// trimmed version of MD5_End()
static void render_md5(const u8 digest[MD5_DIGEST_LENGTH], char buf[MD5_DIGEST_STRING_LENGTH]) {
	static const char hex[] = "0123456789abcdef";

	assert(digest);
	int i;

	for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
		buf[i + i] = hex[digest[i] >> 4];
		buf[i + i + 1] = hex[digest[i] & 0x0f];
	}
	buf[i + i] = '\0';
	return;
}

/** alloc + fill a new array of properties.
 * must be free'd with free_properties()
 *
 * return NULL if error
 */
static struct printable_prop *new_properties(struct romfile *rf) {
	assert(rf);
	struct printable_prop *prop, *props;
	props = malloc(sizeof(props_template));
	if (!props) return NULL;

	memcpy(props, props_template, sizeof(props_template));

	prop = props;
	while (prop->csv_name != NULL) {
		utstring_init(&prop->rendered_value);
		prop++;
	}

	/* fill in all properties now */

	char ecuid[ECUID_STR_LEN] = {0};
	if (ecuid_from_filename(rf->filename, ecuid)) {
		utstring_printf(&props[RP_ECUID].rendered_value, "\"%s\"", ecuid);
	}

	utstring_printf(&props[RP_FILE].rendered_value, "\"%s\"", rf->filename);
	utstring_printf(&props[RP_SIZE].rendered_value, "%luk", (unsigned long) rf->siz / 1024);

	u32 loaderpos = find_loader(rf);
	if (loaderpos != UINT32_MAX) {
		const char *scpu;
		scpu = (const char *) rf->loader_cpu;
		utstring_printf(&props[RP_LOADER].rendered_value, "%02d", rf->loader_v);
		utstring_printf(&props[RP_LOADER_OFS].rendered_value, "0x%lX", (unsigned long) loaderpos);
		utstring_printf(&props[RP_LOADER_CPU].rendered_value, "\"%.6s\"",scpu);
		utstring_printf(&props[RP_LOADER_CPUCODE].rendered_value, "\"%.2s\"",scpu+6);
	}

	u32 fidpos = find_fid(rf);
	if (fidpos != UINT32_MAX) {
		const char *scpu = (const char *) rf->fid_cpu;	//shortcut
		utstring_printf(&props[RP_FID_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_fid);
		utstring_printf(&props[RP_FID].rendered_value, "\"%.*s\"",
						(int) sizeof(((struct fid_base1_t *)NULL)->FID), (const char *) rf->fid);
		utstring_printf(&props[RP_FID_CPU].rendered_value, "%.8s", scpu);
		utstring_printf(&props[RP_FID_CPUCODE].rendered_value, "%.2s", scpu+6);
	} else {
		DBG_PRINTF("error: no FID struct ? Cannot continue.\n");
		free_properties(props);
		return NULL;
	}

	//"RAMF_off\RAMjump entry
	u32 ramfpos = find_ramf(rf);

	assert(rf->fidtype);
	unsigned features = rf->fidtype->features;
	if (features & ROM_HAS_ECUREC) {
		//no RAMF for these
	} else if (ramfpos == UINT32_MAX) {
		DBG_PRINTF("find_ramf() failed !!\n");
	} else {
		utstring_printf(&props[RP_RAMF_WEIRD].rendered_value, "%+d", rf->ramf_offset);
		utstring_printf(&props[RP_RAMJUMP].rendered_value, "0x%08X", rf->ramf.pRAMjump);
	}

	//IVT2\tIVT2 confidence\t"
	if (features & ROM_HAS_IVT2) {
		int ivt_conf = 0;
		if (rf->p_ivt2 != UINT32_MAX) {
			ivt_conf = 99;
		} else {
			u32 iter;
			DBG_PRINTF("no IVT2 ?? wtf. Last resort, brute force technique:\n");
			iter = 0x100;	//skip power-on IVT
			bool ivtfound = 0;
			while ((iter + 0x400) < rf->siz) {
				u32 new_offs;
				new_offs = find_ivt(rf->buf + iter, rf->siz - iter);
				if (new_offs == UINT32_MAX) {
					if (ivtfound) break;
					DBG_PRINTF("\t no IVT2 found.\n");
					break;
				}
				iter += new_offs;
				ivt_conf = 50;
				DBG_PRINTF("\tPossible IVT @ 0x%lX\n",(unsigned long) iter);
				if (reconst_32(rf->buf + iter + 4) ==0xffff7ffc) {
					ivt_conf = 75;
					DBG_PRINTF("\t\tProbable IVT !\n");
					ivtfound = 1;
				}
				iter += 0x4;
			}
		}
		utstring_printf(&props[RP_IVT2].rendered_value, "0x%lX", (unsigned long) rf->p_ivt2);
		utstring_printf(&props[RP_IVT2_CONF].rendered_value, "%02d", ivt_conf);
	}

	if (features & ROM_HAS_STDCKS) {
		if (!checksum_std(rf->buf, rf->siz, &rf->p_cks, &rf->p_ckx)) {
			utstring_printf(&props[RP_STD_CKS].rendered_value, "1");
			utstring_printf(&props[RP_STD_S_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_cks);
			utstring_printf(&props[RP_STD_X_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_ckx);
		} else {
			utstring_printf(&props[RP_STD_CKS].rendered_value, "0");
		}
	}

	if (features & ROM_HAS_ALTCKS) {
		// expecting altcks : either good or bad
		utstring_printf(&props[RP_ALT_CKS].rendered_value, "%d", (int) rf->cks_alt_good);
		utstring_printf(&props[RP_ALT_S_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_acs);
		utstring_printf(&props[RP_ALT_X_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_acx);
		utstring_printf(&props[RP_ALT_START].rendered_value, "0x%lX", (unsigned long) rf->p_acstart);
		utstring_printf(&props[RP_ALT_END].rendered_value, "0x%lX", (unsigned long) rf->p_acend);
	}

	if (features & ROM_HAS_ALT2CKS) {
		// expecting altcks : either good or bad
		utstring_printf(&props[RP_ALT2_CKS].rendered_value, "%d", (int) rf->cks_alt2_good);
		utstring_printf(&props[RP_ALT2_S_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_a2cs);
		utstring_printf(&props[RP_ALT2_X_OFS].rendered_value, "0x%lX", (unsigned long) rf->p_a2cx);
		utstring_printf(&props[RP_ALT2_START].rendered_value, "0x%lX", (unsigned long) rf->p_ac2start);
		utstring_printf(&props[RP_RIPEMD160].rendered_value, "%d", (int) rf->has_rm160);
	}

	//known / guessed keysets
	enum key_quality keyq;
	uint32_t s27k, s36k;
	keyq = find_s27_hardcore(rf->romdb, rf->buf, rf->siz, &s27k, &s36k);
	if (keyq > KEYQ_UNK) {
		utstring_printf(&props[RP_KEYSET_QUAL].rendered_value, "%d", keyq);
		utstring_printf(&props[RP_S27K].rendered_value, "0x%08lX", (unsigned long) s27k);
		utstring_printf(&props[RP_S36K].rendered_value, "0x%08lX", (unsigned long) s36k);
	} else {
		// only bruteforce if code analysis failed, since it's much slower
		const struct keyset_t *tmp_keyset;
		tmp_keyset = find_keys_bruteforce(rf->romdb, rf->buf, rf->siz, &keyq, 0);
		if (tmp_keyset && (keyq > KEYQ_UNK)) {
			utstring_printf(&props[RP_KEYSET_QUAL].rendered_value, "%d", keyq);
			utstring_printf(&props[RP_S27K].rendered_value, "0x%08lX",
							(unsigned long) tmp_keyset->s27k);
			utstring_printf(&props[RP_S36K].rendered_value, "0x%08lX",
							(unsigned long) tmp_keyset->s36k1);
		} else {
			utstring_printf(&props[RP_KEYSET_QUAL].rendered_value, "0");
		}
	}

	// EEPROM info
	find_eep(rf);
	if (rf->p_eepread) {
		utstring_printf(&props[RP_EEP_READ_OFFS].rendered_value, "0x%lX",
						(unsigned long) rf->p_eepread);
		utstring_printf(&props[RP_EEP_PORT].rendered_value, "0x%08lX",
						(unsigned long) rf->eep_port);
	}

	// MD5 digest of ROM
	MD5_CTX md5c;
	u8 md5_digest[MD5_DIGEST_LENGTH];
	char md5_str[MD5_DIGEST_STRING_LENGTH];
	MD5Init(&md5c);
	MD5Update(&md5c, rf->buf, rf->siz);
	MD5Final(md5_digest, &md5c);
	render_md5(md5_digest, md5_str);
	utstring_printf(&props[RP_MD5].rendered_value, "%s", md5_str);
	DBG_PRINTF("MD5: %s\n", md5_str);

	return props;
}

/** free array of properties and the value strings */
static void free_properties(struct printable_prop *props) {
	assert(props);
	struct printable_prop *prop = props;

	while (prop->csv_name != NULL) {
		utstring_done(&prop->rendered_value);
		prop++;
	}
	free(props);
	return;
}


/** get length of the path prefix of a given filename
 *
 * i.e. strips the filename and keeps the absolute or relative path, including
 * last trailing dir separator / or \
 *
 * "~/d/stuff/file.txt" => length of "~/d/stuff/"
 *
 * why do I need to reinvent wheels
 */
static size_t get_path_len(const char *filename) {
	// search backwards for a fwd/back slash

	const char *pfile;
    pfile = filename + strlen(filename);
    for (; pfile > filename; pfile--) {
        if ((*pfile == '\\') || (*pfile == '/')) {
            pfile++;
            break;
        }
    }
    return (size_t) (pfile - filename);
}


/* e.g. csv_filename = "file.csv", and argv0 = "~/d/stuff/nisrom",
 * this prints "~/d/stuff/file.csv" into the given UT_string (must already be initialized)
 */
static void generate_csv_path(UT_string *dest, const char *csv_filename, const char *argv0) {
	assert(dest && csv_filename && argv0);

	size_t base_len = get_path_len(argv0);
	utstring_bincpy(dest, argv0, base_len);	//no 0-term
	utstring_printf(dest, "%s", csv_filename);
	return;
}


static void usage(void) {
	printf(	"**** %s\n"
			"**** Analyze Nissan ROM\n"
			"**** (c) 2015-2022 fenugrec\n", progname);
	printf("Usage:\t%s <ROMFILE> [OPTIONS] : analyze ROM dump.\n"
			"OPTIONS:\n"
			"\t-c: CSV output\n"
			"\t-h: show this help\n"
			"\t-l: CSV headers (can be combined with -c)\n"
			"\t-v: human-readable output (default)\n"
			"\t-f: force parsing, ignoring errors (may cause crashes, do not use)\n", progname);
	return;
}

int main(int argc, char *argv[])
{
	bool	dbg_file;	//flag if dbgstream is a real file
	struct romfile rf = {0};

	bool enable_csv_header = 0;
	bool enable_csv_vals = 0;
	bool enable_human = 0;	//this overrides the previous print_csv_* flags

	const char *filename=NULL;

	char c;
	int optidx;

	while((c = getopt(argc, argv, "cfhlv")) != -1) {
		switch(c) {
		case 'h':
			usage();
			return 0;
		case 'c':
			enable_csv_vals = 1;
			break;
		case 'f':
			force_parse = 1;
			break;
		case 'l':
			enable_csv_header = 1;
			break;
		case 'v':
			enable_human = 1;
			break;
		default:
			usage();
			return 0;
		}
	}

	// default to human-readable
	if (!enable_csv_vals && !enable_csv_header) {
		enable_human = 1;
	}

		//second loop for non-option args
	for (optidx = optind; optidx < argc; optidx++) {
		if (!filename) {
			filename = argv[optidx];
			continue;
		}
		ERR_PRINTF("junk argument\n");
		return -1;
	}

	dbg_file = 1;
	dbg_stream = fopen(DBG_OUTFILE, "a");
	if (!dbg_stream) {
		dbg_file = 0;
		dbg_stream = stdout;
	}

	/* print headers if possible, regardless of missing args */
	if (!enable_human && enable_csv_header) {
		print_csv_header(props_template);
	}

	// only scenario where filename is not required is if we're just printing csv headers
	if (!filename) {
		if (enable_csv_header) {
			return 0;
		}
		ERR_PRINTF("Must specify a file name with these options !\n");
		return -1;
	}

	rf.romdb = romdb_new();
	if (!rf.romdb) {
		ERR_PRINTF("trouble in romdb_new\n");
		goto badexit;
	}

	UT_string csvpath;
	utstring_init(&csvpath);
	generate_csv_path(&csvpath, KEYSET_CSV, argv[0]);

	if (!romdb_keyset_addcsv(rf.romdb, utstring_body(&csvpath))) {
		ERR_PRINTF("csv trouble\n");
		goto badexit;
	}

	if (open_rom(&rf, filename)) {
		ERR_PRINTF("Trouble in open_rom()\n");
		goto badexit;
	}

	/* add header to dbg log */
	DBG_PRINTF("\n********************\n**** Started analyzing %s\n", filename);

	struct printable_prop *props = new_properties(&rf);
	if (!props) {
		goto badexit;
	}

	if (enable_human) {
		print_human(props);
	} else {
		/* print values */
		if (enable_csv_vals) {
			print_csv_values(props);
		}
	}

	free_properties(props);

	//test : find calltable
	unsigned ctlen = 0;
	uint32_t ctpos = 0;
	while (1) {
		ctpos = find_calltable(rf.buf, ctpos + ctlen * 4, rf.siz, &ctlen);
		if (ctpos == (u32) -1) break;
		DBG_PRINTF("possible calltable @ %lX, len=0x%X\n", (unsigned long) ctpos, ctlen);
	}

	romdb_close(rf.romdb);
	rf.romdb = NULL;
	close_rom(&rf);
	if (dbg_file) fclose(dbg_stream);
	return 0;

badexit:
	if (rf.romdb) {
		romdb_close(rf.romdb);
		rf.romdb = NULL;
	}
	close_rom(&rf);
	if (dbg_file) fclose(dbg_stream);
	return -1;
}
