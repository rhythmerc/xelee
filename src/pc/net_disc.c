/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The game image's identity for netplay compatibility (pc_net_disc_id,
 * net.h): XeleeNet's lobbies, LAN's and the matchmaking backends compare it
 * with their peers'. */
#include "pc/net.h"

#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <xxhash.h>

#include <stdio.h>
#include <string.h>

/* Identity of the game image, so two peers on different discs (region,
 * revision, or a modified ISO) are told apart before a match instead of
 * desyncing in it. XXH3 over the boot info the DVD layer read out of
 * the image (game id, disc number, disc version, streaming flags), the base
 * FST entry count and the disc's main.dol, folded to 32 bits to keep the
 * LAN announcement (an mDNS answer is size constrained) and the service's
 * requests short. Both blobs are already in memory (aurora keeps the partition
 * metadata for the life of the disc), so nothing is read off the image here;
 * computed once and cached.
 *
 * ponytail: pins the code and the file-table shape, not every file's bytes,
 * so a data-only mod that leaves the FST shape alone (a swapped Pl*.dat of
 * the same size) still matches. Fold in every base FST entry's name and
 * size when that shows up. */
const char* pc_net_disc_id(void) {
    static char id[9];
    if (id[0] != '\0') {
        return id;
    }
    const DVDDiskID* disk = DVDGetCurrentDiskID();
    s32 dol_size = 0;
    const u8* dol = DVDGetDOLLocation(&dol_size);
    if (disk == NULL || dol == NULL || dol_size <= 0) {
        return "0"; /* no disc open: only before the launcher hands one over */
    }
    uint32_t entries = (uint32_t)aurora_dvd_base_entry_count();
    uint32_t bytes = (uint32_t)dol_size;
    uint8_t meta[18];
    memcpy(meta, disk->gameName, sizeof disk->gameName);
    memcpy(meta + 4, disk->company, sizeof disk->company);
    meta[6] = disk->diskNumber;
    meta[7] = disk->gameVersion;
    meta[8] = disk->streaming;
    meta[9] = disk->streamingBufSize;
    for (int i = 0; i < 4; i++) { /* spelled out: the peers can be x86 and ARM */
        meta[10 + i] = (uint8_t)(entries >> (24 - 8 * i));
        meta[14 + i] = (uint8_t)(bytes >> (24 - 8 * i));
    }
    uint64_t h = XXH3_64bits_withSeed(dol, (size_t)dol_size, XXH3_64bits(meta, sizeof meta));
    snprintf(id, sizeof id, "%08x", (uint32_t)(h ^ h >> 32));
    return id;
}
