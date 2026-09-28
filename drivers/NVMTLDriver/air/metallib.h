/* .metallib container: header, function list (tagged entries) and the
 * bitcode section holding one LLVM module per function. */
#ifndef AIR_METALLIB_H
#define AIR_METALLIB_H
#include <stddef.h>
#include <stdint.h>

enum { MTL_FN_VERTEX = 0, MTL_FN_FRAGMENT = 1, MTL_FN_KERNEL = 2, MTL_FN_UNQUALIFIED = 3,
       MTL_FN_VISIBLE = 4, MTL_FN_EXTERN = 5, MTL_FN_INTERSECTION = 6 };

typedef struct {
    char name[256];
    uint32_t type;
    const uint8_t *bitcode;     /* points into the caller's buffer */
    uint32_t bitcode_len;
    uint8_t hash[32];
} mtllib_function;

typedef struct {
    uint32_t nfunctions;
    mtllib_function *functions;
} mtllib;

int mtllib_parse(const uint8_t *data, size_t len, mtllib *out, char *err, size_t errlen);
void mtllib_free(mtllib *l);
#endif
