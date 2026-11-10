/* tcptest HOST PORT: resolves HOST (getaddrinfo) and opens a TCP connection, printing each step
 * and the Winsock error. For telling a phone network problem from an engine one. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
int main(int argc, char** argv)
{
  if (argc < 3) { printf("usage: tcptest HOST PORT\n"); return 2; }
  WSADATA w; printf("WSAStartup %d\n", WSAStartup(MAKEWORD(2, 2), &w));
  struct addrinfo hints = { 0 }, *res = NULL;
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  int r = getaddrinfo(argv[1], argv[2], &hints, &res);
  printf("getaddrinfo(%s) = %d\n", argv[1], r);
  for (struct addrinfo* a = res; a; a = a->ai_next) {
    char ip[64] = "?"; DWORD n = sizeof(ip);
    WSAAddressToStringA(a->ai_addr, (DWORD)a->ai_addrlen, NULL, ip, &n);
    SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    int c = connect(s, a->ai_addr, (int)a->ai_addrlen);
    printf("  %s family %d: connect %s (%d)\n", ip, a->ai_family, c == 0 ? "ok" : "failed",
           c == 0 ? 0 : WSAGetLastError());
    closesocket(s);
  }
  return 0;
}
