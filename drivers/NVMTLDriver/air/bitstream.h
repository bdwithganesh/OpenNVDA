/* LLVM bitstream reader (the container under .air / .metallib bitcode).
 * Format: llvm/docs/BitCodeFormat.rst. Plain C, no LLVM needed, so it runs
 * inside the Metal bundle and on the build host for tests.
 *
 * The reader turns the stream into a tree of blocks and records with every
 * abbreviation expanded; the IR layer (airir.c) works on that tree only. */
#ifndef AIR_BITSTREAM_H
#define AIR_BITSTREAM_H
#include <stddef.h>
#include <stdint.h>

typedef struct bc_record {
    uint32_t code, nops;
    uint64_t *ops;
    const uint8_t *blob;        /* blob operand, if the abbreviation had one */
    uint32_t blob_len;
} bc_record;

typedef struct bc_block {
    uint32_t id;
    uint32_t nrecords, nblocks;
    bc_record *records;
    struct bc_block *blocks;
    /* order of records and sub-blocks as they appear: >= 0 record index,
     * < 0 is -(block index + 1). Function blocks need the interleaving. */
    int32_t *order;
    uint32_t norder;
} bc_block;

/* Parse raw bitcode or a wrapper (0x0B17C0DE) image. The root is a pseudo
 * block (id ~0) holding the top-level blocks. 0 on success. */
int bc_parse(const uint8_t *data, size_t len, bc_block *root, char *err, size_t errlen);
void bc_free(bc_block *b);
const bc_block *bc_find(const bc_block *b, uint32_t id);   /* first child with id */

/* block ids */
enum {
    BC_BLOCKINFO = 0, BC_MODULE = 8, BC_PARAMATTR = 9, BC_PARAMATTR_GROUP = 10,
    BC_CONSTANTS = 11, BC_FUNCTION = 12, BC_IDENTIFICATION = 13, BC_VALUE_SYMTAB = 14,
    BC_METADATA = 15, BC_METADATA_ATTACHMENT = 16, BC_TYPE = 17, BC_USELIST = 18,
    BC_MODULE_STRTAB = 19, BC_GLOBALVAL_SUMMARY = 20, BC_OPERAND_BUNDLE_TAGS = 21,
    BC_METADATA_KIND = 22, BC_STRTAB = 23, BC_FULL_LTO = 24, BC_SYMTAB = 25,
    BC_SYNC_SCOPE_NAMES = 26,
};
#endif
