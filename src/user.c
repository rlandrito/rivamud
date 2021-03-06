#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <errno.h>
#include "user.h"
#include "network_listener.h"
#include "network_util.h"
#include "messaging.h"
#include "string_util.h"

User *allUsers = NULL;
pthread_rwlock_t allUsersLock = PTHREAD_RWLOCK_INITIALIZER;

/**
 * some exposed functions are wrappers around internal static _functions
 * the wrappers are to lock before and after the static _function call
 *
 * this is provided so that other user functions in this file can call
 * the static _function without requesting a lock, in the case that the
 * caller is already holding a lock
 */

static int user_add_allUsers(User *user);
static User *_user_find_by_name(char *name);

static User *_user_find_by_name(char *name) {
  User *current;

  for (current = allUsers; current != NULL; current = current->next) {
    if (strcmp(name, current->name) == 0)
      break;
  }

  if (current == NULL)
    return NULL;

  return current;
}


User *user_find_by_name(char *name) {
  User *user;

  pthread_rwlock_rdlock(&allUsersLock);

  user = _user_find_by_name(name);

  pthread_rwlock_unlock(&allUsersLock);

  return user;
}

// this just normalizes, it doesn't validate
static char *normalize_name(char *name) {
  int len;

  name = trim(name);
  len = strlen(name);

  name[0] = toupper(name[0]);
  for (int i = 1; i < len; i++)
    name[i] = tolower(name[i]);

  return name;
}

User *user_login(int sockfd) {
  char recvBuf[256];    // this is random, the bufsize should be set once we
                        // encapsulate recv calls
  char sendBuf[32];
  char *name;
  int nbytes;
  int namelen;
  User *me;

  while (1) {
    sprintf(sendBuf, "Login: ");
    sendline(sockfd, sendBuf, strlen(sendBuf) + 1);
    // TODO wrap recv calls
    nbytes = readline(sockfd, recvBuf, sizeof recvBuf);
    if (nbytes <= 0)
      return NULL;

    recvBuf[MAX_NAME_LEN] = '\0';
    name = normalize_name(recvBuf);
    namelen = strlen(name);
    if (namelen < MIN_NAME_LEN || namelen > MAX_NAME_LEN)
      continue;

    break;
    // TODO do some more name verification, keep asking for login until
    // we find a suitable name
  }
  sendline(sockfd, "Welcome ", 8);
  sendline(sockfd, name, namelen);
  sendline(sockfd, "!\r\n", 3);

  me = user_create(name, sockfd);
  return me;
}

User *user_create(char *name, int sockfd) {
  User *newUser;

  // TODO encapsulate malloc calls
  if ((newUser = malloc(sizeof(User))) == NULL)
    return NULL;

  newUser->sockfd = sockfd;
  newUser->name   = strdup(name);
  if (msg_create(newUser->pfds) == -1)
    return NULL;

  user_add_allUsers(newUser);

  return newUser;
}


static int user_add_allUsers(User *user) {
  // TODO test returns on locking calls.. once implemented, remember to
  // change ALL calls !
  pthread_rwlock_wrlock(&allUsersLock);
  user->next = allUsers;
  allUsers = user;
  pthread_rwlock_unlock(&allUsersLock);

  return 1;
}

int user_destroy(User *user) {
  User dummy;
  User *currentUser;
  User *prev;

  pthread_rwlock_wrlock(&allUsersLock);

  dummy.next    = allUsers;
  prev          = &dummy;
  currentUser   = allUsers;

  // find
  while (currentUser != NULL && currentUser != user) {
    prev = prev->next;
    currentUser = currentUser->next;
  }
  if (currentUser == NULL) {// lol wut
    pthread_rwlock_unlock(&allUsersLock);
    return -1;
  }

  prev = currentUser->next;
  pthread_rwlock_unlock(&allUsersLock);

  msg_destroy(currentUser->pfds);
  free(currentUser->name);
  free(currentUser);

  return 1;
}

int user_thread_handler(User *me) {
  int sockfd = me->sockfd;
  int fdmax;
  fd_set m_readfds, m_writefds, readfds, writefds;
  int recvbytes;
  char recvbuf[MAX_BUF_SIZE];
  char msgbuf[MAX_BUF_SIZE];
  char *trimmed;
  int msglen;
  int nready;
  int closed_connection = 0;

  FD_ZERO(&m_readfds);
  FD_ZERO(&m_writefds);
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_SET(sockfd, &m_readfds);
  FD_SET(me->pfds[0], &m_readfds);
  fdmax = sockfd > me->pfds[0] ? sockfd : me->pfds[0];

  while (1) {
    readfds = m_readfds;
    writefds = m_writefds;
    if ((nready = select(fdmax+1, &readfds, NULL, NULL, NULL)) < 1) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    for (; nready > 0; nready--) {
      if (FD_ISSET(sockfd, &readfds)) {
        recvbytes = readline(sockfd, recvbuf, (sizeof recvbuf) - 1);
        if (recvbytes <= 0) {
          closed_connection = 1;
          break;
        }

        printf("%d received\n", recvbytes);
        // TODO this is where we need proper command parsing and verification, 
        // for now, we're just a chat server
        recvbuf[recvbytes] = '\0';
        trimmed = trim(recvbuf);
        if (strlen(trimmed) > 0) {
          msglen = snprintf(msgbuf, sizeof msgbuf, "%s says, %s\r\n",
                            me->name, trimmed);
          msg_broadcast(me, RM_SCOPE_ALL, 0, msgbuf, msglen+1);
        }

        continue;
      }
      if (FD_ISSET(sockfd, &writefds)) {
        // handle socket writes
        continue;
      }
      if (FD_ISSET(me->pfds[0], &readfds)) {
        // handle pipe
        msglen = read(me->pfds[0], msgbuf, sizeof msgbuf);
        printf("%d bytes in pipe\n", msglen);
        sendline(sockfd, msgbuf, msglen);

        continue;
      }
    }
    if (closed_connection)
      break;
  }

  user_destroy(me);

  return 1;
}
