#include "fuzzuf/channel/fd_channel.hpp"
#include "fuzzuf/logger/logger.hpp"
#include "fuzzuf/utils/common.hpp"

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

FdChannel::FdChannel() {
    // FIXME: perror -> throw Exception
}

FdChannel::~FdChannel() {
    // TODO:
}

int FdChannel::Send(void *buf, size_t size) {
    int nbytes = write(forksrv_write_fd, buf, size);
    if (nbytes < 0) {
        ERROR("[FdChannel] Failed to send");
    }
    return nbytes;
}

// Recieved data to be stored to user allocated pointer `buf`
int FdChannel::Recv(void *buf, size_t size) {
    int nbytes = read(forksrv_read_fd, buf, size);
    if (nbytes < 0) {
        ERROR("[!] [FdChannel] Failed to recv");
    }
    return nbytes;
}

pid_t FdChannel::SetupForkServer(char *const pargv[]) {
    DEBUG("[*] [FdChannel] SetupForkserver");

    int par2chld[2], chld2par[2];

    if (pipe(par2chld) || pipe(chld2par)) {
        perror("pipe() failed");
        exit(1);
    }

    forksrv_pid = fork();
    if (forksrv_pid < 0) {
        perror("fork() failed");
        exit(1);
    }

    if (forksrv_pid == 0) {
        // In PUT process
        if (dup2(par2chld[0], FORKSRV_FD_READ) < 0) {
            perror("dup2() failed");
            exit(1);
        };
        if (dup2(chld2par[1], FORKSRV_FD_WRITE) < 0) {
            perror("dup2() failed");
            exit(1);
        }

        close(par2chld[0]);
        close(par2chld[1]);
        close(chld2par[0]);
        close(chld2par[1]);

        // FIXME: 無条件で標準（エラ）出力をクローズ
        int null_fd = Util::OpenFile("/dev/null", O_RDONLY | O_CLOEXEC);
        dup2(null_fd, 1);
        dup2(null_fd, 2);
        
        DEBUG("[*] [FdChannel] pargv[0]=\"%s\": pid=%d\n", pargv[0], getpid());

        execv(pargv[0], pargv);
        exit(0);
    }

    close(par2chld[0]);
    close(chld2par[1]);

    forksrv_write_fd = par2chld[1];
    forksrv_read_fd = chld2par[0];

    return forksrv_pid;
}