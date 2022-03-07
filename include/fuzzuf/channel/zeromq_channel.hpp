#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zmq.h>
#include <czmq.h>

class ZeroMqChannel /* : public Channel */ {
public:
    ZeroMqChannel(const char *endpoint);
    ~ZeroMqChannel();

    pid_t SetupForkServer(char *const pargv[]);

    int Send(void *buf, size_t size);

    int Recv(void *buf, size_t size);

private:
    pid_t forksrv_pid;
    const char *endpoint;
    void *context; // TODO: Use smart pointer
    void *socket; // TODO: Use smart pointer
};

// FIXME: 一時的な定義。Protocol buffer使って
enum ExecutePUTError {
    None = 0,
    DaemonAlreadyExit,
    DaemonBusyError,
    SpawnPUTError,
};

// FIXME: 一時的な定義。Protocol buffer使って
typedef struct {
    enum ExecutePUTError error;
    int32_t exit_code;
    int32_t signal_number;
} ExecutePUTAPIResponse;