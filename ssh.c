#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <signal.h>
#include <pthread.h>
#include "dict.h"
#include "dict.c"

struct addrinfo* getAvilableAddresses(struct addrinfo* hints, char* port){
  struct addrinfo* addresses; 
  int status = getaddrinfo(NULL, port, hints, &addresses);
  if(status != 0){
    fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
    return NULL;
  }
  return addresses;
}

int assignSocket(struct addrinfo* addresses){
  int yes = 1;
  int socketfd = 0;
  for(struct addrinfo *address = addresses; address != NULL; address = address->ai_next){
    socketfd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if(socketfd == -1){
      perror("impossible to create socket\n");
      continue;
    }
    if(setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1){
      perror("impossible to make soket reusable\n");
      continue;
    }
    if(bind(socketfd, address->ai_addr, address->ai_addrlen) == -1){
      close(socketfd);
      perror("impossible to bind socket\n");
      continue;
    }
    return socketfd;
  }
  perror("socket bind error\n");
  return -1;
}

int setNonBlock(int socketfd){
  int flags = fcntl(socketfd, F_GETFL, 0);
  if (fcntl(socketfd, F_SETFL, flags | O_NONBLOCK)){
    perror ("impossible to make socket nonblock");
    return -1;
  }
  return 0;
}

int addToEpoll(int epollfd, int socketfd){
  struct epoll_event event;
  event.data.fd = socketfd;
  event.events = EPOLLIN | EPOLLET;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, socketfd, &event) == -1){
    perror("failed to add descriptor to epoll");
    return -1;
  }
}

int acceptConnection(int epollfd, int socketfd, Dict login, Dict fLogin){
  struct sockaddr addr;
  socklen_t addrlen = sizeof addr;

  int connectionfd = accept(socketfd, &addr, &addrlen);
  if (connectionfd == -1){
    perror ("failed to accept connection");
    return -1;
  }
  
  char host[NI_MAXHOST]; 
  char service[NI_MAXSERV]; 

  if(!getnameinfo(&addr, addrlen, host, sizeof host, service, sizeof service, NI_NUMERICHOST | NI_NUMERICSERV)){

    printf("new connection: %s:%s\n", host, service);
  }

  if (setNonBlock(connectionfd) == -1){
    fprintf(stderr, "failed to set socket %d nonblock", connectionfd);
    return -1;
  }

  if (addToEpoll(epollfd, connectionfd) == -1){
    fprintf(stderr, "failed to add socket %d to epoll", connectionfd);
    return -1;
  }
  dprintf(connectionfd,"Enter login: ");
  
  char conBuf[4];
  snprintf(conBuf, 4,"%d",connectionfd);
  DictInsert(login, conBuf, "");

  snprintf(conBuf, 4,"%d",connectionfd);
  DictInsert(fLogin, conBuf, "0");
  return 0;  
}
int checkLogin(const char *login, char * password){
  printf("%s\n", login);
  printf("%s\n", password);
  FILE *mf;
  char *estr;
  char str[50];
  mf = fopen("registr","r");
  while(1)
  {

    estr = fgets(str,50,mf);
    if (estr == NULL)
      break;
    printf("%s\n", estr);
    printf("%s\n", login);
  
    if (!strcmp(str, login)){
      fclose(mf);
      return 1;
    }

    estr = fgets(str,50,mf);
    printf("%s\n", estr);
    printf("%s\n", password);

    if (!strcmp(str, password)){
      fclose(mf);
      return 1;
    }

  }
  fclose(mf);
  return 0;
}
int handlerRequest(int epollfd, int connectionfd, Dict login, Dict fLogin){

  char buffer[1024];
  char conBuf[4];
  ssize_t count = read(connectionfd, buffer, sizeof buffer);
  switch(count){
    case -1:
      if (errno != EAGAIN)
        perror ("failed to read data");
      break;
    case 0:
      printf("client closed the connection");
      break;
    default:
      snprintf(conBuf, 4,"%d",connectionfd);
      printf("->%s\n", DictSearch(fLogin, conBuf));
      if(!strcmp(DictSearch(login, conBuf),"")){
        printf("->%d\n", count);
        if (count > 1){
          char *tmpLogin = (char*) malloc(count+1);
          strncpy(tmpLogin, buffer, count);
          tmpLogin[count] = '\0';
          DictInsert(login,conBuf,tmpLogin);

          printf("!!!%s!!!\n", DictSearch(login, conBuf));
          dprintf(connectionfd, "Enter password: ");
        }
        else
          dprintf(connectionfd,"Enter login: ");
      }
      else{
        if (!strcmp(DictSearch(fLogin, conBuf),"0")){
          char *tmpPass=(char*) malloc(count+1); 
          strncpy(tmpPass, buffer, count);
          tmpPass[count] = '\0';
          int check = checkLogin(DictSearch(login, conBuf), tmpPass);
          if(check == 1){
            DictDelete(fLogin,conBuf);
            DictInsert(fLogin,conBuf,"1");
            dprintf(connectionfd, "!!Enter command: ");
          }
          else{
            DictDelete(login,conBuf);
            DictInsert(login,conBuf,"");
            dprintf(connectionfd,"Enter login: ");
          }
        }
        else{
          if(!strncmp(buffer, "logout",6))
            close(connectionfd);
          else{
          int c;
          FILE *pp;
          extern FILE *popen();
 
          if ( !(pp=popen(buffer, "r")) ) 
            return 1;
 
          while ( (c=fgetc(pp)) != EOF ) {
          putc(c, stdout); 
          dprintf(connectionfd,"%c",c);
          fflush(pp);
          }   
          pclose(pp);

          printf("%d user message: %.*s", connectionfd, count, buffer); 
          dprintf(connectionfd, "Enter command: ");
        }
        }
      }
  }
  printf("connection %d closed\n", connectionfd);
  close(connectionfd);
}

int handleEvent(struct epoll_event* event, int epollfd, int socketfd, Dict login, Dict fLogin){
  if ((event->events & EPOLLERR) || (event->events & EPOLLHUP)){
    printf("impossible to handle event\n");
    close(event->data.fd);
    return -1;
  }
  return socketfd == event->data.fd ?
         acceptConnection(epollfd, socketfd, login, fLogin) : 

         handlerRequest(epollfd, event->data.fd, login, fLogin);

}

volatile sig_atomic_t done = 0;

void handleSignal(int signum){
  done = 1;
}

int main (int argc, char *argv[]){

  Dict login;

  login = DictCreate();

  Dict fLogin;

  fLogin = DictCreate();
  printf("starting server\n");
  struct addrinfo hints; 
  memset(&hints, 0, sizeof hints);
  hints.ai_flags =  AI_PASSIVE;
  hints.ai_family = AF_UNSPEC; 
  hints.ai_socktype = SOCK_STREAM; 
  hints.ai_protocol = IPPROTO_TCP; 
  

  char* port = "8080";
  struct addrinfo* addresses = getAvilableAddresses(&hints, port); 
  if(!addresses)
    return EXIT_FAILURE;
  

  int socketfd = assignSocket(addresses);
  if(!socketfd)
    return EXIT_FAILURE;


  if (setNonBlock(socketfd) == -1){
    perror("non_block");
    return EXIT_FAILURE;
  }


  if (listen (socketfd, SOMAXCONN) == -1){
    perror ("listen");
    return EXIT_FAILURE;
  }
  printf("listening port %s\n", port);
  

  int epollfd = epoll_create1(0);
  if (epollfd == -1){
    perror ("epoll_create");
    return EXIT_FAILURE;
  }


  if (addToEpoll(epollfd, socketfd) == -1)
    return EXIT_FAILURE;
  

  signal(SIGINT, handleSignal);
    
    

  int maxEventNum = 8;
  struct epoll_event events[maxEventNum * sizeof(struct epoll_event)];
  

  int timeout = -1 ;
  while(!done){
    printf("waiting new events\n");
    int eventsNumber = epoll_wait(epollfd, events, maxEventNum, timeout);
    if (eventsNumber == 0) 
      printf("no events\n");
    for (int i = 0; i < eventsNumber; ++i) {
      handleEvent(events + i, epollfd, socketfd, login, fLogin);
    }
  }
  
  printf("server is going down\n");
  printf("closing connections\n");
  close(socketfd);
  close(epollfd);
  printf("done\n");
  return EXIT_SUCCESS;
}
