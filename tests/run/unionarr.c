#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
size_t ee_size(void){ return sizeof(struct epoll_event); }
size_t ee_off_data(void){ return offsetof(struct epoll_event, data); }
size_t tv_size(void){ return sizeof(struct timeval); }
size_t su_size(void){ return sizeof(struct sockaddr_un); }
size_t su_off_path(void){ return offsetof(struct sockaddr_un, sun_path); }
int epoll_roundtrip(void){
  int ep = epoll_create1(0); if (ep < 0) return -1;
  int p[2]; if (pipe(p) != 0) return -2;
  struct epoll_event ev; memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN; ev.data.fd = p[0];
  if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0) return -3;
  if (write(p[1], "x", 1) != 1) return -4;
  struct epoll_event out[2];
  int n = epoll_wait(ep, out, 2, 1000);
  if (n != 1) return -5;
  int ok = (out[0].data.fd == p[0]) && (out[0].events == EPOLLIN);
  close(p[0]); close(p[1]); close(ep);
  return ok ? 1 : -6;
}
void tv_set(struct timeval* tv, long s, long us){ tv->tv_sec = s; tv->tv_usec = us; }
long tv_get(struct timeval* tv){ return tv->tv_sec; }
void su_fill(struct sockaddr_un* su, const char* path){
  memset(su, 0, sizeof *su); su->sun_family = 1;
  strncpy(su->sun_path, path, sizeof su->sun_path - 1);
}
int su_path0(struct sockaddr_un* su){ return (unsigned char)su->sun_path[0]; }
int su_path_sum(struct sockaddr_un* su, int n){ int s=0; for (int i=0;i<n;i++) s += (unsigned char)su->sun_path[i]; return s; }
struct timeval* make_tv(void){ return (struct timeval*)calloc(1, sizeof(struct timeval)); }
struct sockaddr_un* make_su(void){ return (struct sockaddr_un*)calloc(1, sizeof(struct sockaddr_un)); }
struct epoll_event* ee_make(void){ return (struct epoll_event*)calloc(1, sizeof(struct epoll_event)); }
uint32_t ee_get_u32(struct epoll_event* e){ return e->data.u32; }
int ee_get_fd(struct epoll_event* e){ return e->data.fd; }
void ee_set_u64(struct epoll_event* e, unsigned long long v){ e->data.u64 = v; e->events = 1; }
