#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

class FdChannel /* : public Channel */ {
public:
    FdChannel();
    ~FdChannel();

    pid_t SetupForkServer(char *const pargv[]);

    int Send(void *buf, size_t size);

    int Recv(void *buf, size_t size);

private:
    pid_t forksrv_pid;
    int forksrv_read_fd;
    int forksrv_write_fd;

    static const int FORKSRV_FD_READ = 196; // 本家とバッチングしない値で
    static const int FORKSRV_FD_WRITE = 197;
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
} ExecutePUTAPIResponse;