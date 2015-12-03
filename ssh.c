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

#define WORKING_THREADS_COUNT 5

//стуктура с пользовательскими данными
struct UserParams{
  Dict login;
  Dict fLogin;
  Dict location;
};
//стуктура с параметрами потоков
struct Params{
  pthread_mutex_t mutex;
  pthread_cond_t condvar;
  int currrent;
  int end;
  struct epoll_event* events;
  int epollfd;
  int socketfd;
  struct UserParams userParams;
};



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

//делаем сокет не блокируемым
int setNonBlock(int socketfd){
  int flags = fcntl(socketfd, F_GETFL, 0);
  if (fcntl(socketfd, F_SETFL, flags | O_NONBLOCK)){
    perror ("impossible to make socket nonblock");
    return -1;
  }
  return 0;
}

// добавление сокета в epoll для отслеживания событий
int addToEpoll(int epollfd, int socketfd){
  struct epoll_event event;
  event.data.fd = socketfd;
  event.events = EPOLLIN | EPOLLET;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, socketfd, &event) == -1){
    perror("failed to add descriptor to epoll");
    return -1;
  }
}

// прием соединения, если новых соединений нет или произошла ошибка то connectionfd == -1 
int acceptConnection(int epollfd, int socketfd, struct UserParams userParams){
  struct sockaddr addr;
  socklen_t addrlen = sizeof addr;

  int connectionfd = accept(socketfd, &addr, &addrlen);
  if (connectionfd == -1){
    perror ("failed to accept connection");
    return -1;
  }
  
  char host[NI_MAXHOST]; 
  char service[NI_MAXSERV]; 

  //резервирование символьного адреса и имени сервиса
  if(!getnameinfo(&addr, addrlen, host, sizeof host, service, sizeof service, NI_NUMERICHOST | NI_NUMERICSERV)){
  //вывод информации о полльзователе
    printf("new connection: %s:%s\n", host, service);
  }

  if (setNonBlock(connectionfd) == -1){
    fprintf(stderr, "failed to set socket %d nonblock", connectionfd);
    return -1;
  }
  //добавление в epol
  if (addToEpoll(epollfd, connectionfd) == -1){
    fprintf(stderr, "failed to add socket %d to epoll", connectionfd);
    return -1;
  }
  dprintf(connectionfd,"Enter login: ");
  //добавление пользователя в структуру
  char conBuf[4];
  snprintf(conBuf, 4,"%d",connectionfd);
  DictInsert(userParams.login, conBuf, "");

  snprintf(conBuf, 4,"%d",connectionfd);
  DictInsert(userParams.fLogin, conBuf, "0");
  return 0;  
}
//проверка введенных пользовательских данных
int checkLogin(const char *login, char * password){
  printf("%s\n", login);
  printf("%s\n", password);
  FILE *mf;
  char *estr;
  char str[50];
  mf = fopen("registr","r");
  while(1) {
    estr = fgets(str,50,mf);
    if (estr == NULL)
      break;
  
    if (!strcmp(str, login)){
      fclose(mf);
      return 1;
    }

    estr = fgets(str,50,mf);

    if (!strcmp(str, password)){
      fclose(mf);
      return 1;
    }
  }
  fclose(mf);
  return 0;
}
//чтение данных, присланных пользователем
int handlerRequest(int epollfd, int connectionfd, struct UserParams userParams, pthread_mutex_t mutex){

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
      printf("->%s\n", DictSearch(userParams.fLogin, conBuf));
      //ввод логина
      if(!strcmp(DictSearch(userParams.login, conBuf),"")){
        printf("->%d\n", count);
        if (count > 1){
          char *tmpLogin = (char*) malloc(count+1);
          strncpy(tmpLogin, buffer, count);
          tmpLogin[count] = '\0';
          DictInsert(userParams.login,conBuf,tmpLogin);

          printf("%s\n", DictSearch(userParams.login, conBuf));
          dprintf(connectionfd, "Enter password: ");
        }
        else
          dprintf(connectionfd,"Enter login: ");
      }
      else{
        //ввод пароля
        if (!strcmp(DictSearch(userParams.fLogin, conBuf),"0")){
          char *tmpPass=(char*) malloc(count+1); 
          strncpy(tmpPass, buffer, count);
          tmpPass[count] = '\0';
          //вызов проверки
          pthread_mutex_lock(&mutex);
          int check = checkLogin(DictSearch(userParams.login, conBuf), tmpPass);
          pthread_mutex_unlock(&mutex);
          if(check == 1){
            DictDelete(userParams.fLogin,conBuf);
            DictInsert(userParams.fLogin,conBuf,"1");
            DictInsert(userParams.location,conBuf," /home/anatoly/Ssh/user_root/");
            dprintf(connectionfd, "!!Enter command: ");
          }
          else{
            DictDelete(userParams.login,conBuf);
            DictInsert(userParams.login,conBuf,"");
            dprintf(connectionfd,"Enter login: ");
          }
        }
        else{
          //обработка комманд, присланных пользователем
          if(!strncmp(buffer, "logout",6))
            close(connectionfd);
          else{
          int c;
          FILE *pp;
          extern FILE *popen();
          
          if ( !(pp=popen(strcat(buffer, DictSearch(userParams.location, conBuf)), "r")) ) 
            return 1;
 
          while ( (c=fgetc(pp)) != EOF ) {
          putc(c, stdout); 
          dprintf(connectionfd,"%c",c);
          fflush(pp);
          }   
          pclose(pp);

          printf("%d user command: %.*s", connectionfd, count, buffer); 
          dprintf(connectionfd, "Enter command: ");
        }
        }
      }
  }
}
//обработка события на сокете
int handleEvent(struct epoll_event* event, int epollfd, int socketfd, struct UserParams userParams, pthread_mutex_t mutex){
  if ((event->events & EPOLLERR) || (event->events & EPOLLHUP)){
    printf("impossible to handle event\n");
    close(event->data.fd);
    return -1;
  }
  return socketfd == event->data.fd ?
        // событие на серверном сокете
        acceptConnection(epollfd, socketfd, userParams) : 
        // событие на сокете соединения
        handlerRequest(epollfd, event->data.fd, userParams, mutex);

}

volatile sig_atomic_t done = 0;

void handleSignal(int signum){
  done = 1;
}
//обработка событий потоком
void* workThread(void* p){
  struct Params* params = (struct Params*) p;
  printf("Wait");
  pthread_mutex_lock(&params->mutex);
  pthread_cond_wait(&params->condvar,&params->mutex);
  pthread_mutex_unlock(&params->mutex);
  printf("Hi");
  int cur = malloc(sizeof(int));
  while(!done)
  {
    
    int flag = 0;
    pthread_mutex_lock(&params->mutex);
    if(params->currrent < params->end){
      cur = params->currrent;
      params->currrent++;
      flag = 1;
    } 
    else{
      //если нет новых событий
      pthread_cond_wait(&params->condvar,&params->mutex);
    }
    pthread_mutex_unlock(&params->mutex);
    if(flag){
    printf("handling event %d of %d\n", params->currrent, params->end);
    printf("%d\n",cur );
    handleEvent(params->events+cur, params->epollfd, params->socketfd, params->userParams, params->mutex);
    }
  }

}

int main (int argc, char *argv[]){
  //пользовательскике данные
  struct UserParams userParams;
  Dict login = DictCreate();
  Dict fLogin = DictCreate();
  Dict location = DictCreate();;
  userParams.login = login;
  userParams.fLogin = fLogin;
  userParams.location = location;

  //параметры адреса
  printf("starting server\n");
  struct addrinfo hints; 
  memset(&hints, 0, sizeof hints);
  hints.ai_flags =  AI_PASSIVE;
  hints.ai_family = AF_UNSPEC; 
  hints.ai_socktype = SOCK_STREAM; 
  hints.ai_protocol = IPPROTO_TCP; 
  
  // получаение списка доступных адресов
  char* port = "8080";
  struct addrinfo* addresses = getAvilableAddresses(&hints, port); 
  if(!addresses)
    return EXIT_FAILURE;
  
  //коннект сокета
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
  
  //создание epoll
  int epollfd = epoll_create1(0);
  if (epollfd == -1){
    perror ("epoll_create");
    return EXIT_FAILURE;
  }


  if (addToEpoll(epollfd, socketfd) == -1)
    return EXIT_FAILURE;
  
  //регистрация обработчика событий
  signal(SIGINT, handleSignal);
    
  int maxEventNum = 8;
  struct epoll_event events[maxEventNum * sizeof(struct epoll_event)];
  //заполнение параметров для потоков
  struct Params params;
  pthread_mutex_init(&params.mutex, NULL);
  pthread_cond_init(&params.condvar, NULL); 
  params.end = 0;
  params.currrent = 0;
  params.epollfd = epollfd;
  params.events = events;
  params.socketfd = socketfd;
  params.userParams = userParams;
  pthread_t working[WORKING_THREADS_COUNT]; 
  for(int i = 0; i<WORKING_THREADS_COUNT; i++){   
    pthread_create(&working[i], NULL, workThread, &params);
  } 

  //обработка событий
  int timeout = -1 ;
  while(!done){
    if(params.currrent >= params.end){
      printf("waiting new events\n");
      int eventsNumber = epoll_wait(epollfd, events, maxEventNum, timeout);
      params.currrent = 0;
      params.end = eventsNumber;
      printf("Send\n");
      pthread_cond_signal(&params.condvar); 
    }
  }
  
  printf("server is going down\n");
  printf("closing connections\n");
  close(socketfd);
  close(epollfd);
  printf("done\n");
  return EXIT_SUCCESS;
}
