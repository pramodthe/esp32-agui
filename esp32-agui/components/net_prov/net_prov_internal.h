// net_prov internal shared declarations (not part of the public API).
#pragma once

#include "esp_err.h"

#define NET_PROV_MAX_CREDS 5

typedef struct {
    char ssid[33];   // 32 + NUL
    char pass[65];   // 64 + NUL
} net_cred_t;

// Load saved credentials from NVS into out[], returns count (<= max).
int net_creds_load(net_cred_t *out, int max);
