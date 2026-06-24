// rs_client.h — C API for the rs-agent HTTP interface.
//
// All functions return RS_ERROR_SUCCESS (0) on success.
// String outputs use a double-call pattern:
//   1. buf=NULL  → *size = required bytes
//   2. buf!=NULL → fills buf, *size = bytes written
//
// Caller manages all memory. RS_FreeNodeList is the only
// caller-side free needed (array size is discovery-dependent).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// ── Error codes ──────────────────────────────────────────────────

#define RS_ERROR_SUCCESS      0
#define RS_ERROR_NETWORK     -1   // HTTP connect / request failed
#define RS_ERROR_API         -2   // Agent returned an error response
#define RS_ERROR_PARAM       -3   // Invalid parameter (null pointer, etc.)
#define RS_ERROR_TOO_SMALL   -4   // Buffer too small — *size set to required bytes

// ── Discovery ────────────────────────────────────────────────────

typedef struct {
    char name[256];
    char ip[64];
    int  port;
    char apis[512];
} RS_NodeInfo;

typedef struct {
    RS_NodeInfo* nodes;
    int          count;
} RS_NodeList;

int  RS_DiscoverNodes(int timeout_ms, RS_NodeList* out);
void RS_FreeNodeList(RS_NodeList* list);

// ── Node info / Session ──────────────────────────────────────────

typedef struct {
    char    state[32];          // "idle", "launching", "running", "stopping"
    int     pid;
    int     exit_code;          // -1 = no exit recorded
    int64_t launched_at;        // ms since epoch, 0 if idle
    int64_t pipe_connected_at;  // ms since epoch, 0 if never connected
} RS_SessionStatus;

int RS_Health(const char* host, int port);

int RS_GetNodeInfo(const char* host, int port, char* buf, int* size);
// Returns JSON with keys: hostname, virtual_screen{x,y,w,h}, displays[]

int RS_ListUnreal(const char* host, int port, char* buf, int* size);
// Returns JSON array of {pid}

int RS_GetSessionStatus(const char* host, int port, RS_SessionStatus* out);

int RS_KillUnreal(const char* host, int port, int pid);
// Responds with {"success":true} on success.

// ── Schema / Launch ──────────────────────────────────────────────

int RS_GetSchema(const char* host, int port, const char* project_path,
                 char* buf, int* size);
// Returns JSON: channels[], scenes[{name, hash, parameters[]}]

int RS_LaunchUnreal(const char* host, int port, const char* config_json,
                    char* buf, int* size);
// config_json is the full launch body:
// {"engine_exe":..., "project":..., "map":..., "node_name":...,
//  "ndisplay":{...}, "streams":[...]}
// Returns JSON: {"pid":pid} or {"error":"..."}

#ifdef __cplusplus
}
#endif
