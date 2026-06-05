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

// HTTP API

int   RS_Health(const char* host, int port);
char* RS_GetNodeInfo(const char* host, int port);
char* RS_ListUnreal(const char* host, int port);
int   RS_KillUnreal(const char* host, int port, int pid);
void  RS_FreeString(char* s);

#ifdef __cplusplus
}
#endif
