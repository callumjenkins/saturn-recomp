// saturn-recomp runtime — BSD sockets as Winsock and POSIX both give them, for the NetBand tunnel, http_get and tests.
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
inline const Socket kNoSocket = INVALID_SOCKET;
inline void close_socket(Socket s) { closesocket(s); }
inline void nonblocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
inline void blocking(Socket s) { u_long off = 0; ioctlsocket(s, FIONBIO, &off); }
inline bool start_sockets() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
inline const Socket kNoSocket = -1;
inline void close_socket(Socket s) { close(s); }
inline void nonblocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
inline void blocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK); }
inline bool start_sockets() { return true; }
#endif
