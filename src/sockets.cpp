#include "sockets.h"

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

host_socket host_tcp_listen(const char *host, const char *port, int flags)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *info;
    if (getaddrinfo(host, port, &hints, &info) != 0) {
        return HOST_SOCKET_INVALID;
    }
    host_socket s = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (s != HOST_SOCKET_INVALID) {
        if (flags & HOST_SOCKET_REUSEADDR) {
            int on = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        }
        if (bind(s, info->ai_addr, info->ai_addrlen) != 0 || listen(s, 8) != 0) {
            close(s);
            s = HOST_SOCKET_INVALID;
        }
    }
    freeaddrinfo(info);
    return s;
}

host_socket host_socket_accept(host_socket s)
{
    return accept(s, NULL, NULL);
}

int host_socket_read(host_socket s, void *buf, int count)
{
    return (int) recv(s, buf, count, 0);
}

int host_socket_write(host_socket s, const void *buf, int count)
{
    return (int) send(s, buf, count, 0);
}

int host_socket_select_read(host_socket s)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    struct timeval timeout = {0, 0};
    return select(s + 1, &set, NULL, NULL, &timeout) > 0 ? HOST_SELECT_READ : 0;
}

bool host_socket_close(host_socket s)
{
    return close(s) == 0;
}
