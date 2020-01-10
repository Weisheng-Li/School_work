#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <assert.h>
#include "csapp.h"
#include "cache.h"

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = " Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// function declaration
void doit(int fd, CacheList* cache);
int parse_url(const char *url, char *host, char *port, char *path);
int read_requesthdrs(rio_t *rp, char* buf, char* host);
int get_headername(char* header, char* buf);
int change_headervalue(char* header, const char* new_val);

int main(int argc, char **argv) 
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  Signal(SIGPIPE, SIG_IGN);
  CacheList cachelist;
  cache_init(&cachelist);

  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = accept(listenfd, (SA *)&clientaddr, &clientlen); 
    if (connfd == -1) continue;

    int rt;
    if ((rt = getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
        port, MAXLINE, 0)) != 0) {
      printf("getnameinfo Error: %s\n", gai_strerror(rt));
      close(connfd);
      continue;
    }
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd, &cachelist);                                             
    close(connfd);                                            
  }
}

/*
 * doit - handle one HTTP request/response transaction
 */
void doit(int fd, CacheList* cache) 
{
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  memset(buf, 0, MAXLINE);
  char host[MAXLINE], port[MAXLINE], path[MAXLINE];
  rio_t rio, rio_server;

  /* Read request line and headers */
  rio_readinitb(&rio, fd); 
  if (!rio_readlineb(&rio, buf, MAXLINE))  
    return;
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version);       
  if (strcasecmp(method, "GET")) {                     
    printf("Proxy does not implement this method.\n");
    return;
  }                 

  // check if the uri is currently cached
  CachedItem* item = find(uri, cache); 
  if (item != NULL) {
    rio_writen(fd, item->headers, strlen(item->headers));
    rio_writen(fd, item->item_p, item->size);
    printf("\n\n\nFound!\n\n\n");
    return;
  }                                  
  
  /* Parse URI from GET request, and make sure the url
     is using http protocol */
  if (!parse_url(uri, host, port, path)) {
    printf("This proxy only accept url with http protocol.");
    return;
  }

  // handle request headers
  memset(buf, 0, MAXLINE);
  if (!read_requesthdrs(&rio, buf, host)) return;

  // Make a connection with webserver
  int clientfd;
  if ((clientfd = open_clientfd(host, port)) < 0) {
    printf("Unable to connect to server.");
    return;
  }

  // sending request
  char request_line[1024];
  sprintf(request_line, "GET %s HTTP/1.0\r\n", path);
  rio_writen(clientfd, request_line, strlen(request_line)); // request line
  rio_writen(clientfd, buf, strlen(buf));                   // headers
  rio_writen(clientfd, "\r\n", 2);                          // terminating empty line

  printf("=======================================\nrequest headers: %s%s", request_line, buf);    

  // read response from server
  memset(buf, 0, MAXLINE);
  rio_readinitb(&rio_server, clientfd);
  char header_name[MAXLINE];
  long content_length = 0;
  short fl1, fl2, fl3, fl4;   // flags to determine if the response
  fl1 = fl2 = fl3 = fl4 = 0;  // is qualified to be cached

  char* temp_buf = buf;
  short rt;
  while((rt = rio_readlineb(&rio_server, temp_buf, MAXLINE)) > 2) {
    // get the content-length
    get_headername(temp_buf, header_name);
    if (!strncasecmp(header_name, "content-length", strlen(header_name))
        && strlen(header_name)) {
      fl2 = !fl2;    
      content_length = atoi(temp_buf + (strlen("content-length") + 1));
      if (content_length <= MAX_OBJECT_SIZE) fl3 = !fl3;
    }

    // check the return status
    if (!strncasecmp(temp_buf, "http", 4)) {
      if (!strncasecmp(temp_buf + strlen("HTTP/1.0 "), "200 OK", 6)) 
        fl1 = !fl1;
    }
    if (rio_writen(fd, temp_buf, strlen(temp_buf)) < 0) {
      close(clientfd);
      return;
    }
    temp_buf += strlen(temp_buf);
  }

  printf("\nresponce headers: %s\n", buf);    
  
  if (rt < 0) return;
  if (rio_writen(fd, temp_buf, 2) < 0) {
    close(clientfd);
    return;
  }

  int hasLength = 1;
  if (content_length == 0) hasLength = 0;

  // read response body into buf
  char* binary_buf = (char*)malloc(content_length + 1);
  long rt_val = rio_readnb(&rio_server, binary_buf, content_length);
  if (rt_val == content_length && content_length != 0) {
    fl4 = !fl4;
  } else if (!hasLength && rt_val > 0) {
    content_length = rt_val;
  } else {
    free(binary_buf);
    close(clientfd);
    return;
  }
  binary_buf[content_length] = '\0'; // add tailing zero

  // forward to client
  if (rio_writen(fd, binary_buf, content_length) < 0) {
    free(binary_buf);
    close(clientfd);
    return;
  }

  // do the caching 
  if (fl1 & fl2 & fl3 & fl4) {
    cache_URL(uri, buf, binary_buf, content_length, cache);
  } else {
    free(binary_buf);
  }

  close(clientfd);
}

// read request headers into buf and modify them accordingly.
int read_requesthdrs(rio_t *rp, char* buf, char* host) {
  int rt; // return value from readline
  char header_name[MAXLINE];
  memset(header_name, 0, MAXLINE);

  short has_host = 0;
  while((rt = rio_readlineb(rp, buf, MAXLINE)) > 2) {
    if (rt == -1) {
      *buf = 0;
      printf("Error on rio_readlineb");
      return 0;
    }
    get_headername(buf, header_name);
    if (!strncasecmp(header_name, "host", strlen(header_name))) {
      has_host = 1;
    } 
    else if (!strncasecmp(header_name, "user-agent", strlen(header_name))) {
      change_headervalue(buf, user_agent_hdr);
      rt = strlen(buf);
    }
    else if (!strncasecmp(header_name, "connection", strlen(header_name)) ||
              !strncasecmp(header_name, "proxy-connection", strlen(header_name)))
    {
      change_headervalue(buf, " close\r\n");
      rt = strlen(buf);
    }
    else if (!strncasecmp(header_name, "If-Modified-Since", strlen(header_name)) ||
              !strncasecmp(header_name, "If-None-Match", strlen(header_name)))
    {
      *buf = '\0';
      continue; // skip these two headers
    } 

    buf += rt;
  }

  if (!has_host) {
    char host_header[MAXLINE];
    sprintf(host_header, "host: %s\r\n", host);
    strncpy(buf, host_header, strlen(host)+1);
  }
  return 1;
}


// return the name of the given header into the buffer
// Return 1 if succeed, otherwise return 0
int get_headername(char* header, char* buf) {
  char* colon = strchr(header, ':');
  if (colon == NULL) {
    *buf = '\0';
    return 0;
  }
  strncpy(buf, header, (colon - header));
  buf[colon-header] = '\0';
  return 1;
}

// Given a header, change it's value to new value，
int change_headervalue(char* header, const char* new_val) {
  char* colon = strchr(header, ':');
  if (colon == NULL) return 0;
  char* end = strchr(header, '\n');
  if (end == NULL) return 0;

  // compute the number of byte to copy,
  // which is the max of two length
  int old_length = (end + 1) - colon;
  int new_length = strlen(new_val);
  int length = (new_length > old_length)? new_length:old_length;

  strncpy(colon + 1, new_val, length + 1);
  return 1;
}

// Return 1 if succeed, otherwise return 0
int parse_url(const char *url, char *host, char *port, char *path) {
  // copy the string into internal buffer
  char url_buf[1024];
  strcpy(url_buf, url); 

  // if url doesn't contain http at beginning, return 0 
  if (strncasecmp(url_buf, "http://", strlen("http://"))) return 0;
  char* host_ptr = url_buf + strlen("http://");

  // if path is found, copy to given path pointer
  // also replace '/' with '\0' to seperate path
  // from the rest of url. 
  char* path_ptr = strchr(host_ptr, '/');
  if (path_ptr) {
    strncpy(path, path_ptr, strlen(path_ptr)+1);
    *path_ptr = '\0';
  }
  else { // if path not found, add default '/'
    strncpy(path, "/", 2);
  }

  // same structure as path.
  char* port_ptr = strchr(host_ptr, ':');
  if (port_ptr) {
    strncpy(port, port_ptr + 1, strlen(port_ptr+1)+1);
    *port_ptr = '\0';
  }
  else {
    strncpy(port, "80", 3);
  }
  
  strncpy(host, host_ptr, strlen(host_ptr)+1);
  return 1;
}