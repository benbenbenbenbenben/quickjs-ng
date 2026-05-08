#ifndef QJS_DAP_H
#define QJS_DAP_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || (!defined(__wasi__) && !defined(__EMSCRIPTEN__))
#define QJS_DAP_HAVE_SOCKETS 1
#else
#define QJS_DAP_HAVE_SOCKETS 0
#endif

typedef struct DAPTransport DAPTransport;
struct DAPTransport {
    int (*recv)(DAPTransport *t, char *buf, size_t len);
    int (*send)(DAPTransport *t, const char *buf, size_t len);
    int (*poll)(DAPTransport *t, int timeout_ms);
    void (*close)(DAPTransport *t);
    void *opaque;
};

typedef struct DAPServer DAPServer;

/* Create a new DAP server using the given transport */
DAPServer *DAP_NewServer(JSContext *ctx, DAPTransport *transport);

/* Free the DAP server */
void DAP_FreeServer(DAPServer *server);

/* Process messages synchronously until 'launch' is received, then returns.
   Should be called before executing the JS code. */
int DAP_WaitForLaunch(DAPServer *server);

/* Send an 'output' event */
void DAP_SendOutput(DAPServer *server, const char *category, const char *output);

/* Notify the DAP server that the process is exiting */
void DAP_ProcessExited(DAPServer *server, int exit_code);

/* Creates a stdio transport */
DAPTransport *DAP_NewStdioTransport(void);

/* Creates a TCP transport that listens for sequential client sessions. */
DAPTransport *DAP_NewTCPTransport(int port);

/* Setup JS output redirection so print/console.log go to DAP */
void DAP_SetupOutputRedirection(DAPServer *server);
int DAP_SetLogFile(DAPServer *server, const char *path);
void DAP_PrepareExecution(DAPServer *server);
const char *DAP_GetLaunchProgram(DAPServer *server);
const char * const *DAP_GetLaunchArgs(DAPServer *server);
int DAP_GetLaunchArgc(DAPServer *server);
int DAP_GetLaunchModule(DAPServer *server);

#ifdef __cplusplus
}
#endif

#endif /* QJS_DAP_H */
