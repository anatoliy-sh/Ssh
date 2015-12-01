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

int acceptConnection(int epollfd, int socketfd){
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
  return 0;  
}

int handlerRequest(int epollfd, int connectionfd){

  char buffer[1024];
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
      printf("user message: %.*s", count, buffer); // Выводим сообщение
      dprintf(connectionfd, "Hello, %.*s", count, buffer);
  }
  printf("connection %d closed\n", connectionfd);
  close(connectionfd);
}

int handleEvent(struct epoll_event* event, int epollfd, int socketfd){
  if ((event->events & EPOLLERR) || (event->events & EPOLLHUP)){
    printf("impossible to handle event\n");
    close(event->data.fd);
    return -1;
  }
  return socketfd == event->data.fd ?
         acceptConnection(epollfd, socketfd) : 

         handlerRequest(epollfd, event->data.fd);

}

volatile sig_atomic_t done = 0;

void handleSignal(int signum){
  done = 1;
}

int main (int argc, char *argv[]){

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
      printf("handling event %d of %d\n", i + 1, eventsNumber);
      handleEvent(events + i, epollfd, socketfd);
    }
  }
  
  printf("server is going down\n");
  printf("closing connections\n");
  close(socketfd);
  close(epollfd);
  printf("done\n");
  return EXIT_SUCCESS;
}
