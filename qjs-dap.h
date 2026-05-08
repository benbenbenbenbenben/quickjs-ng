#ifndef QJS_DAP_H
#define QJS_DAP_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DAPTransport DAPTransport;
struct DAPTransport {
    int (*recv)(DAPTransport *t, char *buf, size_t len);
    int (*send)(DAPTransport *t, const char *buf, size_t len);
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

/* Creates a TCP transport (blocks until client connects) */
DAPTransport *DAP_NewTCPTransport(int port);

/* Setup JS output redirection so print/console.log go to DAP */
void DAP_SetupOutputRedirection(DAPServer *server);

#ifdef __cplusplus
}
#endif

#endif /* QJS_DAP_H */
