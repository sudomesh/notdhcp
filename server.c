#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h> 
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <assert.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "protocol.h"
#include "phyconnect.h"

#define MAX_CERT_SIZE (65536)

// structs below

struct interface {
  char* ifname;
  char* ip;
  char* netmask;
  int sock;
  struct sockaddr_in addr;
  struct interface* next;
};

// global variables below

int verbose = 0;
int src_port = 4242;
int dest_port = 4243;
struct interface* interfaces = NULL;
char* ssl_cert = NULL;

// functions declarations below

int seed_prng() {
  struct timeval time;
  
  if(gettimeofday(&time, NULL) < 0) {
    perror("seeding prng failed");
    return -1;
  }
  srand(time.tv_usec * time.tv_sec);
}

// writes a null-terminated password string
// of size len-1 to a buffer of size len
// (assumes that buffer has already been allocated)
void generate_password(char* buffer, int len) {
  const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJK";
  int key;

  buffer[--len] = '\0';

  while(len) {
    key = rand() % (int) (sizeof(charset) - 1);
    buffer[len - 1] = charset[key];
    len--;
  }

  return;
}

int broadcast_packet(int sock, void* buffer, size_t len) {
  struct sockaddr_in broadcast_addr;
  int sent = 0;
  int ret;
  int max;

  memset(&broadcast_addr, 0, sizeof(broadcast_addr));
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  broadcast_addr.sin_port = htons(dest_port);

  while(sent < len) {
    ret = sendto(sock, buffer + sent, len, 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr));
    if(ret < 0) {
      return ret;
    }
    sent += ret;
  }

  return 0;
}

int send_response(int sock, char* lease_ip, char* lease_netmask) {
  struct response resp;
  void* sendbuf;
  int ret;

  resp.type = 42;
  resp.lease_ip = inet_addr(lease_ip);
  resp.lease_netmask = inet_addr(lease_netmask);
  generate_password(resp.password, PASSWORD_LENGTH + 1);

  if(!ssl_cert) {
    resp.cert_size = 0;
    return broadcast_packet(sock, (void*) &resp, sizeof(resp));
  }

  resp.cert_size = strlen(ssl_cert);
  sendbuf = malloc(sizeof(resp) + resp.cert_size);
  memcpy(sendbuf, &resp, sizeof(resp));
  memcpy(sendbuf+sizeof(resp), ssl_cert, resp.cert_size);
  ret = broadcast_packet(sock, sendbuf, sizeof(resp) + resp.cert_size);

  free(sendbuf);
  return ret;
}

int handle_incoming(struct interface* iface) {
  struct request req;
  ssize_t ret;
  socklen_t addrlen = sizeof(iface->addr);
  
  ret = recvfrom(iface->sock, &req, sizeof(req), 0, (struct sockaddr*) &(iface->addr), &addrlen);
  if(ret < 0) {
    perror("error receiving packet");
    return -1;
  }

  // ignore partially received packets
  if(ret < sizeof(req)) {
    return -1;
  }
  
  if(req.type == 42) {
    return send_response(iface->sock, iface->ip, iface->netmask);
  }

  //  if(req.type == 43) {
  //    return stop_listening(iface);
  //  }

  return -1;
}

// call with e.g. usage(argv[0], stdin) or usage(argv[0], stderr)
void usage(char* command_name, FILE* out) {
  char default_command_name[] = "notdhcpserver";
  if(!command_name) {
    command_name = (char*) &default_command_name;
  }
  fprintf(out, "Usage: %s [-v] ifname=ip/netmask [ifname2=ip2/netmask2 ...]\n", command_name);
  fprintf(out, "\n");
  fprintf(out, "  -v: Enable verbose mode\n");
  fprintf(out, "  -h: This help text\n");
  fprintf(out, "\n");
  fprintf(out, "For each interface where you want nothcpserver to hand out an IP \"lease\"\n");
  fprintf(out, "specify an interface+ip pair. E.g:\n");
  fprintf(out, "\n");
  fprintf(out, "  %s eth0.2=100.64.0.2/255.255.255.192 eth0.3=100.64.0.3/255.255.255.192\n", command_name);
  fprintf(out, "\n");
}

void usagefail(char* command_name) {
  fprintf(stderr, "Error: Missing required command-line arguments.\n\n");
  usage(command_name, stderr);
  return;
}

struct interface* new_interface() {
  struct interface* iface = (struct interface*) malloc(sizeof(struct interface));
  
  return iface;
}

// add interface to linked list and return the previous end of the linked list
struct interface* add_interface(struct interface* iface) {
  struct interface* cur = interfaces;

  if(!interfaces) {
    interfaces = iface;
  } else {
    while(cur->next) {
      cur = cur->next;
    }
    cur->next = iface;
  }
  return cur;
}

int monitor_interface(struct interface* iface) {

  struct sockaddr_in bind_addr;
  int broadcast_perm;
  int packetlen;
  int sockmode;
  int sock;

  if((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    perror("creating socket failed");
    return -1;
  }

  if(setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, iface->ifname, strlen(iface->ifname)) < 0) {
    perror("binding to device failed");
    return -1;
  }

  sockmode = fcntl(sock, F_GETFL, 0);
  if(sockmode < 0) {
    perror("error getting socket mode");
    return -1;
  }
  
  if(fcntl(sock, F_SETFL, sockmode | O_NONBLOCK) < 0) {
    perror("failed to set non-blocking mode for socket");
    return -1;
  }

  broadcast_perm = 1;
  if(setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &broadcast_perm, sizeof(broadcast_perm)) < 0) {
    perror("setting broadcast permission on socket failed");
    return -1;
  }

  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  bind_addr.sin_port = htons(src_port);

  if(bind(sock, (struct sockaddr*) &bind_addr, sizeof(bind_addr)) < 0) {
    perror("failed to bind udp socket");
    return -1;
  }

  iface->sock = sock;
  iface->addr = bind_addr;
  add_interface(iface);

  printf("Listening on interface %s:\n", iface->ifname);
  printf("  client IP: %s\n", iface->ip);
  printf("  client netmask %s\n\n", iface->netmask);

  return 0;
}

int parse_arg(char* arg) {

  struct interface* iface;
  int arglen = strlen(arg);
  int i;
  int ip_offset;
  int netmask_offset;
  int ip_len;
  int netmask_len;

  iface = new_interface();

  for(i=0; i < arglen; i++) {

    if(arg[i] == '=') {
      iface->ifname = (char*) malloc(i+1);
      memcpy(iface->ifname, arg, i);
      iface->ifname[i] = '\0';
      ip_offset = i + 1;
    }
    if(arg[i] == '/') {
      if(!iface->ifname) {
        return -1;
      }
      
      ip_len = i - ip_offset;
      iface->ip = (char*) malloc(ip_len + 1);
      memcpy(iface->ip, arg + ip_offset, ip_len);
      iface->ip[ip_len] = '\0';
      netmask_offset = i + 1;

      netmask_len = strlen(arg) - netmask_offset;
      iface->netmask = (char*) malloc(netmask_len + 1);
      memcpy(iface->netmask, arg + netmask_offset, netmask_len);
      iface->netmask[netmask_len] = '\0';

      break;
    }
  }

  if(!iface->ifname || !iface->ip || !iface->netmask) {
    fprintf(stderr, "Failed to parse argument: %s\n", arg);
    return -1;
  }

  if(monitor_interface(iface) < 0) {
    return -1;
  }
  
  return 0;
}


int parse_args(int argc, char** argv) {
  
  int i;
  for(i=0; i < argc; i++) {
    if(parse_arg(argv[i]) < 0) {
      return -1;
    }
  }

  return 0;
}

char* load_cert(char* path) {
  FILE* f;
  char* buf = malloc(MAX_CERT_SIZE);
  size_t bytes_read;

  f = fopen(path, "r");
  if(!f) {
    perror("Opening certificate file failed");
    return NULL;
  }

  bytes_read = fread(buf, 1, MAX_CERT_SIZE, f);
  if(ferror(f)) {
    perror("Error reading certificate file");
    return NULL;
  }
  if(bytes_read <= 0) {
    fprintf(stderr, "Reading certificate file failed. Is the file empty?\n");
    return NULL;
  }

  if(fclose(f) == EOF) {
    fprintf(stderr, "Closing certificate file failed.\n");
    return NULL;
  }

  if(verbose) {
    printf("Loaded SSL certificate from %s\n", path);
  }

  return buf;
}


int main(int argc, char** argv) {

  fd_set fdset;
  int max_fd;
  int num_ready;
  extern int optind;
  struct interface* iface;
  int c;

  if(argc <= 0) {
    usagefail(NULL);
    exit(1);
  }

  while((c = getopt(argc, argv, "c:vh")) != -1) {
    switch (c) {
    case 'c':
      ssl_cert = load_cert(optarg);
      if(!ssl_cert) {
        exit(1);
      }
      break;
    case 'v': 
      printf("Verbose mode enabled\n");
      verbose = 1; 
      break; 
    case 'h':
      usage(argv[0], stdout);
      exit(0);
    }
  }

  // need at least one non-option argument
  if(argc < optind + 1) {
    usagefail(argv[0]);
    exit(1);
  }

  if(seed_prng() < 0) {
    exit(1);
  }         

  if(parse_args(argc - optind, argv + optind) < 0) {
    exit(1);
  }

  printf("Ready.\n");

  for(;;) {

    // initialize fdset
    FD_ZERO(&fdset);
    max_fd = 0;
    iface = interfaces;
    do {
      FD_SET(iface->sock, &fdset);
      if(iface->sock > max_fd) {
        max_fd = iface->sock;
      }
    } while(iface = iface->next);

    if((num_ready = select(max_fd + 1, &fdset, NULL, NULL, NULL)) < 0) {
      if(errno == EINTR) {
        printf("huh?\n"); // TODO when does this happen
        continue;
      }
      perror("error during select");
    }

    iface = interfaces;
    do {
      if(FD_ISSET(iface->sock, &fdset)) {
        if(verbose) {
          printf("Packet received on interface %s\n", iface->ifname);
        }

        if(handle_incoming(iface) < 0) {
          perror("Error handling incoming packet");
        } else {
          printf("Response sent!\n");
        }

      }
    } while(iface = iface->next);
  }  
  
  return 0;
}