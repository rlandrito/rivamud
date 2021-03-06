#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <unistd.h>
#include <netdb.h>
#include "network_listener.h"
#include "network_util.h"
#include "user.h"

void *connection_handler(void *socket_desc) {
  int sock = *(int*)socket_desc;
  struct sockaddr_storage ss;
  socklen_t sslen;
  char ipstr[INET6_ADDRSTRLEN];
  char portstr[NI_MAXSERV];
  User *me;

  sslen = sizeof ss;
  getpeername(sock, (struct sockaddr *)&ss, &sslen);
  getnameinfo((struct sockaddr *)&ss, sslen,
              ipstr, sizeof ipstr, portstr, sizeof portstr,
              NI_NUMERICHOST | NI_NUMERICSERV);

  if ((me = user_login(sock)) != NULL)
    user_thread_handler(me);

  if (0 > close(sock)) {
    perror("Error shutting down socket");
  }
  fprintf(stderr, "Connection closed\n");

  return NULL;
}

int start_server(int port) {
  int listenfd, client_sock;
  char portstr[NI_MAXSERV];
  char ipstr[INET6_ADDRSTRLEN];
  int rv;
  int yes=1;
  struct addrinfo hints, *ai, *p;
  struct sockaddr_storage ss;
  socklen_t sslen;

  snprintf(portstr, NI_MAXSERV, "%d", port);

  memset(&hints, 0, sizeof hints);
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags    = AI_PASSIVE;
  if ( (rv=getaddrinfo(NULL, portstr, &hints, &ai)) != 0 ) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    exit(1);
  }

  for (p = ai; p != NULL; p = p->ai_next) {
    listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (listenfd == -1) {
      perror("server: socket");
      continue;
    }

    rv = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    if (rv == -1) {
      perror("setsockopt");
      exit(2);
    }

    rv = bind(listenfd, p->ai_addr, p->ai_addrlen);
    if (rv == -1) {
      perror("server: bind");
      close(listenfd);
      continue;
    }

    break;
  }

  if (p == NULL) {
    fprintf(stderr, "server: could not bind\n");
    exit(2);
  }
  getnameinfo(p->ai_addr, p->ai_addrlen,
      ipstr, sizeof ipstr, portstr, sizeof portstr,
      NI_NUMERICHOST | NI_NUMERICSERV);
  freeaddrinfo(ai);

  // listen
  if ((rv = listen(listenfd, BACKLOG)) == -1) {
    perror("server: listen");
    exit(2);
  }
  printf("server: listening on %s:%s...\n", ipstr, portstr);

  pthread_t thread_id;

  // accept connections
  while (1) {
    sslen = sizeof ss;
    client_sock = accept(listenfd, (struct sockaddr *)&ss,
                         &sslen);
    if (client_sock == -1) {
      perror("accept");
      continue;
    }

    getnameinfo((struct sockaddr *)&ss, sslen,
        ipstr, sizeof ipstr, portstr, sizeof portstr,
        NI_NUMERICHOST | NI_NUMERICSERV);
    printf("server: accepted connection from %s:%s\n", ipstr, portstr);

    if (pthread_create(&thread_id, NULL, connection_handler, 
		       (void*)&client_sock) < 0) {
      perror("Could not spawn thread to handle connection");
    }
  }

  return 0;
}
