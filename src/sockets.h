// The small part of the BSD socket API which the remote control needs.

#ifndef VAMIGA_LUA_SOCKETS_H
#define VAMIGA_LUA_SOCKETS_H

typedef int host_socket;
#define HOST_SOCKET_INVALID -1
#define HOST_SOCKET_REUSEADDR 2
#define HOST_SELECT_READ 1

// Listens on the host and port given as strings. Returns HOST_SOCKET_INVALID
// on errors.
host_socket host_tcp_listen(const char *host, const char *port, int flags);
// Accepts a connection, or returns HOST_SOCKET_INVALID.
host_socket host_socket_accept(host_socket s);
int host_socket_read(host_socket s, void *buf, int count);
int host_socket_write(host_socket s, const void *buf, int count);
// Returns HOST_SELECT_READ if the socket can be read without waiting.
int host_socket_select_read(host_socket s);
bool host_socket_close(host_socket s);

#endif
