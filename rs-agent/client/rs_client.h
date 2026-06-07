#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Discovery

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

RS_NodeList RS_DiscoverNodes(int timeout_ms);
void        RS_FreeNodeList(RS_NodeList* list);

// Session status

#define RS_STATE_IDLE      "idle"
#define RS_STATE_LAUNCHING "launching"
#define RS_STATE_RUNNING   "running"

typedef struct {
    char    state[32];         // "idle", "launching", or "running"
    int     pid;               // 0 if idle
    int     exit_code;         // -1 = no exit recorded
    int64_t launched_at;       // ms since epoch, 0 if idle
    int64_t pipe_connected_at; // ms since epoch, 0 if never connected in this session
} RS_SessionStatus;

// HTTP API

int              RS_Health(const char* host, int port);
char*            RS_GetNodeInfo(const char* host, int port);
char*            RS_ListUnreal(const char* host, int port);
int              RS_KillUnreal(const char* host, int port, int pid);
RS_SessionStatus RS_GetSessionStatus(const char* host, int port);
void             RS_FreeString(char* s);

#ifdef __cplusplus
}
#endif
