#include "fuzzuf/channel/zeromq_channel.hpp"
#include "fuzzuf/logger/logger.hpp"

ZeroMqChannel::ZeroMqChannel(
    const char *endpoint
) : 
    endpoint(endpoint)
{
    // FIXME: perror -> throw Exception
    DEBUG("[*] [ZeroMqChannel] Connecting to fockserver server %s", endpoint);
    context = zmq_ctx_new();
    assert(context);
    socket = zmq_socket(context, ZMQ_REQ);
    assert(socket);
    int err = zmq_connect(socket, endpoint);
    if (err == -1) {
        ERROR("[!] [ZeroMqChannel] Failed to connect to %s", endpoint);
    }
}

ZeroMqChannel::~ZeroMqChannel() {
    zmq_close(socket);
    zmq_ctx_destroy(context);
}

int ZeroMqChannel::Send(void *buf, size_t size) {
    int nbytes = zmq_send(socket, buf, size, 0);
    if (nbytes == -1) {
        ERROR("[!] [ZeroMqChannel] Failed to send");
        exit(1);
    }
    DEBUG("[*] [ZeroMqChannel] Send %d bytes", nbytes);
    return nbytes;
}

// Recieved data to be stored to user allocated pointer `buf`
int ZeroMqChannel::Recv(void *buf, size_t size) {
    int nbytes = zmq_recv(socket, buf, size, 0);
    if (nbytes == -1) {
        ERROR("[!] [ZeroMqChannel] Failed to recv");
        exit(1);
    }
    DEBUG("[*] [ZeroMqChannel] Recv %d bytes", nbytes);
    return nbytes;
}

pid_t ZeroMqChannel::SetupForkServer(char *const pargv[]) {
    DEBUG("[*] [ZeroMqChannel] pargv[0] = %s", pargv[0]);

    forksrv_pid = fork();
    if (forksrv_pid < 0) {
        perror("fork() failed");
        exit(1);
    }

    if (forksrv_pid == 0) {
        // FIXME: 無条件で標準（エラ）出力をクローズ
        int null_fd = Util::OpenFile("/dev/null", O_RDONLY | O_CLOEXEC);
        dup2(null_fd, 1);
        dup2(null_fd, 2);

        execv(pargv[0], pargv);
        exit(0);
    }

    return forksrv_pid;
}