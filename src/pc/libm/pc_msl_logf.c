/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube's logf, bit for bit, on every platform (as pc_msl_trig.c is
 * for the trig). Its one caller in the game is HSD bytecode op 0x13
 * (sysdolphin/baselib/bytecode.c, used by robj.c's reference expressions).
 * Each platform's libm gives its own bits there (glibc, mingw-w64, bionic
 * and Apple's differ from each other and from this table method), and a
 * Slippi peer runs this one in Dolphin.
 *
 * This is Metrowerks MSL's logf (the decomp's src/MSL/math.c, DOL
 * 0x803265A8). It follows the machine code: mwcc fused H*c1 + c0 and
 * LN2*E + ln_F[m] into fmadds, which gk_fmadds reproduces, and the tables
 * and constants are the DOL's bits (__ln_F 0x803B8F50, __one_over_F
 * 0x803B9154, .sdata2 0x804DE1B0). Quirks kept on purpose: the exponent
 * takes the sign bit with it (a negative x gives a large, wrong but
 * deterministic result, not NaN), and a denormal x gives -inf like 0. */
#include "pc_libm.h"
#include "pc/gekko_fp.h"

#include <stdint.h>
#include <string.h>

#undef logf

static const uint32_t ln_F[129] = {
    0x00000000, 0x3BFF0153, 0x3C7E0545, 0x3CBDC8D8, 0x3CFC14D8, 0x3D1CF43E,
    0x3D3BA2C8, 0x3D5A16EC, 0x3D785186, 0x3D8B29B7, 0x3D9A0EBD, 0x3DA8D83A,
    0x3DB78694, 0x3DC61A2F, 0x3DD4936A, 0x3DE2F2A4, 0x3DF1383B, 0x3DFF648A,
    0x3E06BBF4, 0x3E0DB957, 0x3E14AA98, 0x3E1B8FE1, 0x3E22695B, 0x3E29372F,
    0x3E2FF984, 0x3E36B07F, 0x3E3D5C48, 0x3E43FD03, 0x3E4A92D5, 0x3E511DE1,
    0x3E579E4A, 0x3E5E1434, 0x3E647FBE, 0x3E6AE10B, 0x3E71383B, 0x3E77856E,
    0x3E7DC8C3, 0x3E82012D, 0x3E851927, 0x3E882C60, 0x3E8B3AE5, 0x3E8E44C6,
    0x3E914A10, 0x3E944AD1, 0x3E974716, 0x3E9A3EED, 0x3E9D3263, 0x3EA02184,
    0x3EA30C5E, 0x3EA5F2FD, 0x3EA8D56C, 0x3EABB3B9, 0x3EAE8DEE, 0x3EB16418,
    0x3EB43641, 0x3EB70475, 0x3EB9CEC0, 0x3EBC952B, 0x3EBF57C2, 0x3EC2168F,
    0x3EC4D19C, 0x3EC788F4, 0x3ECA3CA1, 0x3ECCECAC, 0x3ECF991F, 0x3ED24205,
    0x3ED4E765, 0x3ED7894A, 0x3EDA27BC, 0x3EDCC2C5, 0x3EDF5A6D, 0x3EE1EEBD,
    0x3EE47FBE, 0x3EE70D78, 0x3EE997F4, 0x3EEC1F39, 0x3EEEA350, 0x3EF12441,
    0x3EF3A213, 0x3EF61CCF, 0x3EF8947B, 0x3EFB0920, 0x3EFD7AC4, 0x3EFFE970,
    0x3F012A95, 0x3F025EFD, 0x3F0391F3, 0x3F04C37A, 0x3F05F397, 0x3F07224C,
    0x3F084F9D, 0x3F097B8D, 0x3F0AA61F, 0x3F0BCF56, 0x3F0CF736, 0x3F0E1DC1,
    0x3F0F42FB, 0x3F1066E7, 0x3F118987, 0x3F12AADE, 0x3F13CAF1, 0x3F14E9C0,
    0x3F16074F, 0x3F1723A2, 0x3F183EBA, 0x3F19589A, 0x3F1A7145, 0x3F1B88BE,
    0x3F1C9F07, 0x3F1DB422, 0x3F1EC813, 0x3F1FDADC, 0x3F20EC7F, 0x3F21FCFF,
    0x3F230C5E, 0x3F241A9F, 0x3F2527C3, 0x3F2633CE, 0x3F273EC1, 0x3F28489E,
    0x3F295169, 0x3F2A5923, 0x3F2B5FCF, 0x3F2C656E, 0x3F2D6A02, 0x3F2E6D8F,
    0x3F2F7015, 0x3F307198, 0x3F317218,
};

static const uint32_t one_over_F[129] = {
    0x3F800000, 0x3F7E03F8, 0x3F7C0FC1, 0x3F7A232D, 0x3F783E10, 0x3F76603E,
    0x3F74898D, 0x3F72B9D6, 0x3F70F0F1, 0x3F6F2EB7, 0x3F6D7304, 0x3F6BBDB3,
    0x3F6A0EA1, 0x3F6865AC, 0x3F66C2B4, 0x3F652598, 0x3F638E39, 0x3F61FC78,
    0x3F607038, 0x3F5EE95C, 0x3F5D67C9, 0x3F5BEB62, 0x3F5A740E, 0x3F5901B2,
    0x3F579436, 0x3F562B81, 0x3F54C77B, 0x3F53680D, 0x3F520D21, 0x3F50B6A0,
    0x3F4F6475, 0x3F4E168A, 0x3F4CCCCD, 0x3F4B8728, 0x3F4A4588, 0x3F4907DA,
    0x3F47CE0C, 0x3F46980C, 0x3F4565C8, 0x3F443730, 0x3F430C31, 0x3F41E4BC,
    0x3F40C0C1, 0x3F3FA030, 0x3F3E82FA, 0x3F3D6910, 0x3F3C5264, 0x3F3B3EE7,
    0x3F3A2E8C, 0x3F392144, 0x3F381703, 0x3F370FBB, 0x3F360B61, 0x3F3509E7,
    0x3F340B41, 0x3F330F63, 0x3F321643, 0x3F311FD4, 0x3F302C0B, 0x3F2F3ADE,
    0x3F2E4C41, 0x3F2D602B, 0x3F2C7692, 0x3F2B8F6A, 0x3F2AAAAB, 0x3F29C84A,
    0x3F28E83F, 0x3F280A81, 0x3F272F05, 0x3F2655C4, 0x3F257EB5, 0x3F24A9CF,
    0x3F23D70A, 0x3F23065E, 0x3F2237C3, 0x3F216B31, 0x3F20A0A1, 0x3F1FD80A,
    0x3F1F1166, 0x3F1E4CAD, 0x3F1D89D9, 0x3F1CC8E1, 0x3F1C09C1, 0x3F1B4C70,
    0x3F1A90E8, 0x3F19D723, 0x3F191F1A, 0x3F1868C8, 0x3F17B426, 0x3F17012E,
    0x3F164FDA, 0x3F15A025, 0x3F14F209, 0x3F144581, 0x3F139A86, 0x3F12F114,
    0x3F124925, 0x3F11A2B4, 0x3F10FDBC, 0x3F105A38, 0x3F0FB824, 0x3F0F177A,
    0x3F0E7835, 0x3F0DDA52, 0x3F0D3DCB, 0x3F0CA29C, 0x3F0C08C1, 0x3F0B7034,
    0x3F0AD8F3, 0x3F0A42F8, 0x3F09AE41, 0x3F091AC7, 0x3F088889, 0x3F07F781,
    0x3F0767AB, 0x3F06D905, 0x3F064B8A, 0x3F05BF37, 0x3F053408, 0x3F04A9FA,
    0x3F042108, 0x3F039930, 0x3F03126F, 0x3F028CC0, 0x3F020821, 0x3F01848E,
    0x3F010204, 0x3F008080, 0x3F000000,
};

static inline float f32(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

float pc_logf(float x)
{
    uint32_t raw;
    uint32_t exp;
    memcpy(&raw, &x, sizeof raw);
    exp = raw & 0x7F800000u;

    if (exp == 0x7F800000u) {
        if (raw & 0x007FFFFFu) {
            return x; /* NaN, as it came */
        }
        return f32((raw & 0x80000000u) ? 0x7FFFFFFFu : 0x7F800000u);
    }
    if (exp == 0) {
        return -f32(0x7F800000u); /* zero and denormals */
    }
    {
        /* E keeps the sign bit (srwi 23, then -127), as the DOL does. */
        const int32_t E = (int32_t) (raw >> 23) - 127;
        const float fE = (float) E; /* exact: |E| < 2^9 */
        const float ln2 = f32(0x3F317218u);
        uint32_t m = (raw >> 16) & 0x7F;

        if (raw & 0xFFFFu) {
            uint32_t raw_fm = (raw & 0x7F0000u) | 0x3F800000u;
            const uint32_t raw_fM = (raw & 0x7FFFFFu) | 0x3F800000u;
            float H, h2_poly;
            if (raw & 0x8000u) {
                raw_fm += 0x10000u;
                m += 1;
            }
            H = f32(raw_fM) - f32(raw_fm);
            H = H * f32(one_over_F[m]);
            h2_poly = (H * H) * gk_fmadds(H, f32(0x3EAAAA36u), f32(0xBF000030u));
            return gk_fmadds(ln2, fE, f32(ln_F[m])) + (H + h2_poly);
        }
        return gk_fmadds(ln2, fE, f32(ln_F[m]));
    }
}
