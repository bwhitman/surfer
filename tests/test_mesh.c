/* glTF mesh loader + software rasterizer. The GLB is BUILT here, byte
 * by byte, because the thing worth testing is the contract: known
 * geometry in, known pixels out. A cube with a different vertex color
 * on every face makes orientation, depth and culling all readable from
 * one center pixel. */
#include <string.h>

#include "mock_hal.h"
#include "surfer.h"

void run_mesh_tests(void);

static uint16_t px565(const surf_image *img, int x, int y)
{
    return *(const uint16_t *)((const uint8_t *)img->pixels +
                               y * img->stride + x * 2);
}

static void put_f(uint8_t *p, float v) { memcpy(p, &v, 4); }
static void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* a unit cube: 24 vertices (4 per face), CCW-outward triangles, each
 * face a flat COLOR_0. Face order: +z, -z, +x, -x, +y, -y. */
static const float face_axis[6][3] = {
    {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
};
static const float face_col[6][3] = {
    {1, 0, 0},  /* +z red — faces the viewer at rot 0 */
    {0, 1, 0},  /* -z green */
    {0, 0, 1},  /* +x blue */
    {1, 1, 0},  /* -x yellow */
    {1, 0, 1},  /* +y magenta */
    {0, 1, 1},  /* -y cyan */
};

static size_t build_cube_glb(uint8_t *out, size_t cap, bool with_colors)
{
    /* bin: 24 vec3 positions, 24 vec3 colors, 36 u16 indices */
    uint8_t bin[24 * 12 + 24 * 12 + 36 * 2 + 4];
    memset(bin, 0, sizeof bin);
    int vi = 0;
    uint8_t *pp = bin, *pc = bin + 24 * 12;
    for (int f = 0; f < 6; f++) {
        const float *n = face_axis[f];
        /* t2 = n x t1, so (t1, t2, n) is right-handed and the corner
         * loop below is CCW seen from outside the face */
        float t1[3] = {n[1], n[2], n[0]};
        float t2[3] = {n[1] * t1[2] - n[2] * t1[1],
                       n[2] * t1[0] - n[0] * t1[2],
                       n[0] * t1[1] - n[1] * t1[0]};
        float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int c = 0; c < 4; c++, vi++) {
            for (int a = 0; a < 3; a++) {
                put_f(pp + (size_t)vi * 12 + a * 4,
                      n[a] + t1[a] * corner[c][0] + t2[a] * corner[c][1]);
                put_f(pc + (size_t)vi * 12 + a * 4, face_col[f][a]);
            }
        }
    }
    uint8_t *pi = bin + 48 * 12;
    for (int f = 0; f < 6; f++) {
        int b = f * 4;
        int quad[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        for (int k = 0; k < 6; k++)
            put_u16(pi + (size_t)(f * 6 + k) * 2, (uint16_t)quad[k]);
    }
    int binlen = 48 * 12 + 36 * 2;

    char json[2048];
    int jl = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0%s},"
        "\"indices\":2,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":"
        "{\"baseColorFactor\":[1.0,1.0,1.0,1.0]}}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":288},"
        "{\"buffer\":0,\"byteOffset\":288,\"byteLength\":288},"
        "{\"buffer\":0,\"byteOffset\":576,\"byteLength\":72}],"
        "\"buffers\":[{\"byteLength\":%d}]}",
        with_colors ? ",\"COLOR_0\":1" : "", binlen);
    while (jl % 4)
        json[jl++] = ' ';
    int binpad = (4 - binlen % 4) % 4;

    size_t need = 12 + 8 + (size_t)jl + 8 + (size_t)binlen + binpad;
    if (need > cap)
        return 0;
    put_u32(out, 0x46546c67u);
    put_u32(out + 4, 2);
    put_u32(out + 8, (uint32_t)need);
    put_u32(out + 12, (uint32_t)jl);
    put_u32(out + 16, 0x4e4f534au);
    memcpy(out + 20, json, (size_t)jl);
    uint8_t *bc = out + 20 + jl;
    put_u32(bc, (uint32_t)(binlen + binpad));
    put_u32(bc + 4, 0x004e4942u);
    memcpy(bc + 8, bin, (size_t)binlen);
    memset(bc + 8 + binlen, 0, (size_t)binpad);
    return need;
}

/* which of the six face colors a lit 565 pixel came from: channel
 * presence survives the lighting, so compare on >0 per channel */
static int face_of(uint16_t p)
{
    int r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    for (int f = 0; f < 6; f++) {
        if ((face_col[f][0] > 0) == (r > 0) &&
            (face_col[f][1] > 0) == (g > 0) &&
            (face_col[f][2] > 0) == (b > 0))
            return f;
    }
    return -1;
}

void run_mesh_tests(void)
{
    fresh(64, 64, 8);

    uint8_t glb[4096];
    size_t n = build_cube_glb(glb, sizeof glb, true);
    OK(n > 0);

    char err[64] = "";
    surf_mesh *m = surf_mesh_from_glb(glb, n, NULL, 0, err, sizeof err);
    OK(m != NULL);
    if (!m) {
        printf("  load: %s\n", err);
        return;
    }
    OK(surf_mesh_tris(m) == 12);

    surf_image *im = surf_image_new(64, 64, SURF_FMT_RGB565);
    OK(im != NULL);

    /* rot 0: the +z face looks at the viewer, so the center is red */
    OK(surf_mesh_render(m, im, 0, 0, 0, 24, 32, 32, 1));
    OK(face_of(px565(im, 32, 32)) == 0);
    OK(px565(im, 1, 1) == 0);                 /* corners untouched */

    /* a quarter turn about y brings +x or -x around; either way the
     * center must change face and the green -z face stays hidden */
    surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
    OK(surf_mesh_render(m, im, 0, 90, 0, 24, 32, 32, 1));
    int f90 = face_of(px565(im, 32, 32));
    OK(f90 == 2 || f90 == 3);

    /* 180: the green back face */
    surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
    OK(surf_mesh_render(m, im, 0, 180, 0, 24, 32, 32, 1));
    OK(face_of(px565(im, 32, 32)) == 1);

    /* tilt down: magenta +y on top half, red still lower center */
    surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
    OK(surf_mesh_render(m, im, 45, 0, 0, 24, 32, 32, 1));
    OK(face_of(px565(im, 32, 16)) == 4);
    OK(face_of(px565(im, 32, 48)) == 0);

    /* scale is pixels: a radius-8 render stays inside its box */
    surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
    OK(surf_mesh_render(m, im, 30, 40, 0, 8, 32, 32, 1));
    OK(px565(im, 32, 32) != 0);
    for (int x = 0; x < 64; x++)
        OK(px565(im, x, 6) == 0);

    /* off-center placement lands where it was told */
    surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
    OK(surf_mesh_render(m, im, 0, 0, 0, 10, 14, 14, 1));
    OK(face_of(px565(im, 14, 14)) == 0);
    OK(px565(im, 50, 50) == 0);

    /* ARGB target draws too, alpha opaque */
    surf_image *ia = surf_image_new(64, 64, SURF_FMT_ARGB8888);
    OK(surf_mesh_render(m, ia, 0, 0, 0, 24, 32, 32, 1));
    uint32_t pa = *(const uint32_t *)((const uint8_t *)ia->pixels +
                                      32 * ia->stride + 32 * 4);
    OK((pa >> 24) == 0xff && ((pa >> 16) & 0xff) > 0 && (pa & 0xff) == 0);
    surf_image_destroy(ia);
    surf_mesh_destroy(m);

    /* no COLOR_0: every face renders the material's white, lit */
    n = build_cube_glb(glb, sizeof glb, false);
    m = surf_mesh_from_glb(glb, n, NULL, 0, err, sizeof err);
    OK(m != NULL);
    if (m) {
        surf_image_fill(im, (surf_rect){0, 0, 64, 64}, 0);
        OK(surf_mesh_render(m, im, 0, 0, 0, 24, 32, 32, 1));
        uint16_t p = px565(im, 32, 32);
        OK(((p >> 11) & 0x1f) > 20 && ((p >> 5) & 0x3f) > 40 &&
           (p & 0x1f) > 20);
        surf_mesh_destroy(m);
    }

    /* refusals: junk, truncation, and a truncated bin chunk must all
     * come back NULL with a reason rather than reading anything */
    OK(surf_mesh_from_glb("hello", 5, NULL, 0, err, sizeof err) == NULL);
    OK(err[0] != 0);
    n = build_cube_glb(glb, sizeof glb, true);
    OK(surf_mesh_from_glb(glb, n / 2, NULL, 0, err, sizeof err) == NULL);

    surf_image_destroy(im);
    printf("mesh tests done\n");
}
