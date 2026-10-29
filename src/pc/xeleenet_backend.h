/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lobby service behind xeleenet.h's calls: the XeleeNet service over
 * HTTPS (xeleenet.c) or this network (xeleenet_lan.c). Each entry is the
 * public call of the same name; xeleenet.c dispatches to the network set
 * (pc_xn_set_network). */
#ifndef PC_XELEENET_BACKEND_H
#define PC_XELEENET_BACKEND_H
#include "xeleenet.h"

typedef struct PcXnBackend {
    bool (*start)(const char* url, const PcNetIdentity* id, const char* name, const char* code, int seats,
        const uint32_t* ip, const uint16_t* udp, int ncands);
    PcXnStatus (*status)(void);
    const char* (*error)(const char** reason);
    bool (*lobby)(char id[33], char code[8], int* port);
    int (*members)(PcXnMember* out, int max);
    void (*set_roster)(const uint8_t (*keys)[32], int n);
    void (*stop)(void);
    void (*host_options)(const char* name, bool open, const char* settings_json);
    bool (*lobby_info)(PcXnListing* out);
    bool (*browse)(const char* url, const PcNetIdentity* id, bool full);
    int (*browse_poll)(PcXnListing* out, int max, int* n, int* others);
} PcXnBackend;

extern const PcXnBackend pc_xn_service_backend;
extern const PcXnBackend pc_xn_lan_backend;

#endif
