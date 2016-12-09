/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/

#define _DARWIN_UNLIMITED_SELECT

#include <pbs_config.h>   /* the master config generated by configure */
#include "lib_net.h"

#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>  /* added - CRI 9/05 */
#include <unistd.h>    /* added - CRI 9/05 */
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h> /* thread_func functions */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#if defined(NTOHL_NEEDS_ARPA_INET_H) && defined(HAVE_ARPA_INET_H)
#include <arpa/inet.h>
#endif
#include <pthread.h>



#include "portability.h"
#include "server_limits.h"
#include "net_connect.h"
#include "net_cache.h"
#include "log.h"
#include "../Liblog/pbs_log.h" /* log_err */
#include "../Liblog/log_event.h" /* log_event */
#include "lib_ifl.h" /* DIS_* */
#include "pbs_error.h" /* PBSE_NONE */
#include "dis.h"

char   local_host_name[PBS_MAXHOSTNAME + 1];
size_t local_host_name_len = PBS_MAXHOSTNAME;

extern char local_host_name[];

/* External Functions Called */

void initialize_connections_table();

/*extern time_t time(time_t *);*/
struct in_addr   net_serveraddr;
char            *net_server_name = NULL;
char             pbs_server_name[PBS_MAXSERVERNAME + 1];

/* Global Data (I wish I could make it private to the library, sigh, but
 * C don't support that scope of control.)
 *
 * This array of connection structures is used by the server to maintain
 * a record of the open I/O connections, it is indexed by the socket number.
 */

struct connection svr_conn[PBS_NET_MAX_CONNECTIONS];

/*
 * The following data is private to this set of network interface routines.
 */

static int       max_connection = PBS_NET_MAX_CONNECTIONS;
static int       num_connections = 0;
pthread_mutex_t *num_connections_mutex = NULL;
static struct pollfd *GlobalSocketReadArray = NULL;
static u_long   *GlobalSocketAddrSet = NULL;
static u_long   *GlobalSocketPortSet = NULL;
pthread_mutex_t *global_sock_read_mutex = NULL;

void *(*read_func[2])(void *);

pthread_mutex_t *nc_list_mutex  = NULL;

pbs_net_t        pbs_server_addr;

/* Private function within this file */

void *accept_conn(void *);


static struct netcounter nc_list[60];

void netcounter_incr(void)

  {
  time_t now;
  int    i;

  pthread_mutex_lock(nc_list_mutex);

  now = time(NULL);

  if (nc_list[0].time == now)
    {
    nc_list[0].counter++;
    }
  else
    {
    memmove(&nc_list[1], &nc_list[0], sizeof(struct netcounter) * 59);

    nc_list[0].time = now;
    nc_list[0].counter = 1;

    for (i = 1; i < 60; i++)
      {
      nc_list[i].time = 0;
      nc_list[i].counter = 0;
      }
    }

  pthread_mutex_unlock(nc_list_mutex);
  }


int get_num_connections()
  {
  int ret_num_connections;

  pthread_mutex_lock(num_connections_mutex);
  ret_num_connections = num_connections;
  pthread_mutex_unlock(num_connections_mutex);

  return(ret_num_connections);
  }


void netcounter_get(
    
  int netrates[])

  {
  int netsums[3] = {0, 0, 0};
  int i;

  pthread_mutex_lock(nc_list_mutex);

  for (i = 0;i < 5;i++)
    {
    netsums[0] += nc_list[i].counter;
    netsums[1] += nc_list[i].counter;
    netsums[2] += nc_list[i].counter;
    }

  for (i = 5;i < 30;i++)
    {
    netsums[1] += nc_list[i].counter;
    netsums[2] += nc_list[i].counter;
    }

  for (i = 30;i < 60;i++)
    {
    netsums[2] += nc_list[i].counter;
    }

  pthread_mutex_unlock(nc_list_mutex);

  if (netsums[0] > 0)
    {
    netrates[0] = netsums[0] / 5;
    netrates[1] = netsums[1] / 30;
    netrates[2] = netsums[2] / 60;
    }
  else
    {
    netrates[0] = 0;
    netrates[1] = 0;
    netrates[2] = 0;
    }

  } /* END netcounter_get() */

/**
 * init_network - initialize the network interface
 * allocate a socket and bind it to the service port,
 * add the socket to the read set for poll(),
 * add the socket to the connection structure and set the
 * processing function to accept_conn()
 */

int init_network(

  unsigned int  port,
  void        *(*readfunc)(void *))

  {
  int         i;
  static int  initialized = 0;
  int         sock;

  int MaxNumDescriptors = 0;

  struct sockaddr_in socname;
  enum conn_type   type;
  pthread_mutexattr_t t_attr;
#ifdef ENABLE_UNIX_SOCKETS

  struct sockaddr_un unsocname;
  int unixsocket;
  memset(&unsocname, 0, sizeof(unsocname));
#endif

  pthread_mutexattr_init(&t_attr);
  pthread_mutexattr_settype(&t_attr, PTHREAD_MUTEX_ERRORCHECK);

  MaxNumDescriptors = get_max_num_descriptors();

  memset(&socname, 0, sizeof(socname));

  if (initialized == 0)
    {
    initialize_connections_table();

    if (net_server_name == NULL)
      {
      /* cache local server addr info */
      struct addrinfo        *addr_info = NULL;
      char                    namebuf[MAXLINE*2];

      if (pbs_getaddrinfo(pbs_server_name, NULL, &addr_info) == 0)
        {
        if (getnameinfo(addr_info->ai_addr, addr_info->ai_addrlen, namebuf, sizeof(namebuf), NULL, 0, 0) == 0)
          {
          net_server_name = strdup(namebuf);
          }

        net_serveraddr = ((struct sockaddr_in *)addr_info->ai_addr)->sin_addr; 
        }

      if (net_server_name == NULL)
        net_server_name = strdup(inet_ntoa(net_serveraddr));
      }

    disiui_();

    for (i = 0; i < PBS_NET_MAX_CONNECTIONS; i++)
      {
      svr_conn[i].cn_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
      pthread_mutex_init(svr_conn[i].cn_mutex,NULL);
      pthread_mutex_lock(svr_conn[i].cn_mutex);
      
      svr_conn[i].cn_active = Idle;
      
      pthread_mutex_unlock(svr_conn[i].cn_mutex);
      }
    
    /* initialize global "read" socket array */
    MaxNumDescriptors = get_max_num_descriptors();

    GlobalSocketReadArray = (struct pollfd *)calloc(MaxNumDescriptors, sizeof(struct pollfd));
    if (GlobalSocketReadArray == NULL)
      {
      return(-1); // no memory
      }

    GlobalSocketAddrSet = (u_long *)calloc(MaxNumDescriptors, sizeof(ulong));
    if (GlobalSocketAddrSet == NULL)
      {
      free(GlobalSocketReadArray);
      GlobalSocketReadArray = NULL;
      return(-1); // no memory
      }

    GlobalSocketPortSet = (u_long *)calloc(MaxNumDescriptors, sizeof(ulong));
    if (GlobalSocketPortSet == NULL)
      {
      free(GlobalSocketReadArray);
      GlobalSocketReadArray = NULL;

      free(GlobalSocketAddrSet);
      GlobalSocketAddrSet = NULL;
      return(-1); // no memory
      }

    global_sock_read_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(global_sock_read_mutex,&t_attr);

    num_connections_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(num_connections_mutex,&t_attr);
    
    type = Primary;
    }
  else if (initialized == 1)
    {
    type = Secondary;
    }
  else
    {
    /* FAILURE */

    return(-1); /* too many main connections */
    }

  /* save the routine which should do the reading on connections */
  /* accepted from the parent socket    */

  read_func[initialized++] = readfunc;

  if (port != 0)
    {
    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
      {
      /* FAILURE */

      return(-1);
      }

    if (MaxNumDescriptors < PBS_NET_MAX_CONNECTIONS)
      max_connection = MaxNumDescriptors;

    i = 1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));

    /* name that socket "in three notes" */

    socname.sin_port = htons((unsigned short)port);

    socname.sin_addr.s_addr = INADDR_ANY;

    socname.sin_family = AF_INET;

    if (bind(sock, (struct sockaddr *)&socname, sizeof(socname)) < 0)
      {
      /* FAILURE */

      close(sock);

      return(-1);
      }

    /* record socket in connection structure and poll set */

    add_conn(sock, type, (pbs_net_t)0, 0, PBS_SOCK_INET, accept_conn);

    /* start listening for connections */

    if (listen(sock, 512) < 0)
      {
      /* FAILURE */

      return(-1);
      }
    } /* END if (port != 0) */

#ifdef ENABLE_UNIX_SOCKETS
  if (port == 0)
    {
    /* setup unix domain socket */

    unixsocket = socket(AF_UNIX, SOCK_STREAM, 0);

    if (unixsocket < 0)
      {
      return(-1);
      }

    unsocname.sun_family = AF_UNIX;

    strncpy(unsocname.sun_path, TSOCK_PATH, sizeof(unsocname.sun_path) - 1);  

    unlink(TSOCK_PATH);  /* don't care if this fails */

    if (bind(unixsocket,
             (struct sockaddr *)&unsocname,
             sizeof(unsocname)) < 0)
      {
      close(unixsocket);

      return(-1);
      }

    if (chmod(TSOCK_PATH, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != 0)
      {
      close(unixsocket);

      return(-1);
      }

    add_conn(unixsocket, type, (pbs_net_t)0, 0, PBS_SOCK_UNIX, accept_conn);

    if (listen(unixsocket, 512) < 0)
      {
      /* FAILURE */

      return(-1);
      }
    }   /* END if (port == 0) */

#endif  /* END ENABLE_UNIX_SOCKETS */

  /* allocate a minute's worth of counter structs */
  if (nc_list_mutex == NULL)
    {
    nc_list_mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    pthread_mutex_init(nc_list_mutex,NULL);

    pthread_mutex_lock(nc_list_mutex);

    memset(nc_list, 0, sizeof(nc_list));
    nc_list[0].time = time(NULL);

    pthread_mutex_unlock(nc_list_mutex);
    }

  return(PBSE_NONE);
  }  /* END init_network() */

/* ping_trqauthd - Send a "ping" request to trqauthd to see 
   if it is up.

RETURN:
   0 if trqauthd is up
   PBSE_BADHOST if trqauthd is no responding
 */

int ping_trqauthd(
    
  const char *unix_socket_name)

  {
  int   rc;
  int   local_socket;
  int   write_buf_len;
  char  *err_msg = NULL;
  char  write_buf[MAX_LINE];
  char  *read_buf = NULL;
  long long   ccode;
  long long   read_buf_len = 0;

  sprintf(write_buf, "%d|", TRQ_PING_SERVER);
  write_buf_len = strlen(write_buf);

  if ((local_socket = socket_get_unix()) < 0)
    {
    fprintf(stderr, "socket_get_unix error\n");
    return(PBSE_SOCKET_FAULT);
    }

  rc = socket_connect_unix(local_socket, unix_socket_name, &err_msg);

  if (err_msg != NULL)
    free(err_msg);

  if (rc != 0)
    {
    /* If we are here we could not connect to trqauthd. That is ok though. That tells us trqauthd is not up.  */
    socket_close(local_socket);
    local_socket = -1;
    return(rc);
    }
  else if ((rc = socket_write(local_socket, write_buf, write_buf_len)) != write_buf_len)
    {
    socket_close(local_socket);
    local_socket = -1;
    rc = PBSE_SOCKET_WRITE;
    fprintf(stderr, "socket_write error\n");
    }
  else if ((rc = socket_read_num(local_socket, &ccode)) != PBSE_NONE)
    {
    socket_close(local_socket);
    local_socket = -1;
    fprintf(stderr, "socket_read_num error\n");
    }
  else if ((rc = parse_daemon_response(ccode, read_buf_len, read_buf)) != PBSE_NONE)
    {
    socket_close(local_socket);
    local_socket = -1;
    fprintf(stderr, "parse_daemon_response error %lld %s\n", ccode, pbse_to_txt(ccode));
    }

  if (local_socket >= 0)
    socket_close(local_socket);

  return(rc);
  }



/**
 * Check to see if the unix domain socket file exists for trqauthd. If
 * it exists then trqauthd is running. Return an error. Otherwise
 * trqauthd is not running. Return PBSE_NONE
 */

int check_trqauthd_unix_domain_port(
    
  const char *unix_socket_name)

  {
  struct stat statbuf;
  int rc = PBSE_NONE;

  rc = stat(unix_socket_name, &statbuf);

  if (rc == 0)
    {
    /* ping trqauthd to see if there is one already running */
    rc = ping_trqauthd(unix_socket_name);
    if (rc == PBSE_NONE)
      {
      /* trqauthd is up. Return a failure */
      return(PBSE_SOCKET_LISTEN);
      }

    /* trqauthd is no listening. Remove the domain socket file */
    unlink(unix_socket_name);
    
    }

  /* trqauthd unix domain socket not found. trqauthd not started */
  return(PBSE_NONE);

  }


/**
 * check_network_port - initialize the network interface
 * allocate a socket and bind it to the service port,
 */

int check_network_port(

  unsigned int  port)

  {
  int   i;
  int    sock;

  struct sockaddr_in socname;

  memset(&socname, 0, sizeof(socname));

  if (port == 0)
    {
    return(-1);
    }
  else
    {
    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
      {
      /* FAILURE */

      return(-1);
      }

    i = 1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));

    /* name that socket "in three notes" */

    socname.sin_port = htons((unsigned short)port);

    socname.sin_addr.s_addr = INADDR_ANY;

    socname.sin_family = AF_INET;

    if (bind(sock, (struct sockaddr *)&socname, sizeof(socname)) < 0)
      {
      /* FAILURE */

      close(sock);

      return(-1);
      }

    close (sock);

    } /* END if (port != 0) */

  return(0);
  }  /* END check_network_port() */



/*
 * wait_request - wait for a request (socket with data to read)
 * This routine does a poll on the read set of sockets,
 * when data is ready, the processing routine associated with
 * the socket is invoked.
 */

int wait_request(

  time_t  waittime,   /* I (seconds) */
  long   *SState)     /* I (optional) */

  {
  int             i;
  int             n;
  time_t          now;

  struct pollfd  *PollArray = NULL;
  struct timespec ts;
  int             PollArraySize = 0;
  int             MaxNumDescriptors = 0;
  int             SetSize = 0;
  u_long   	 *SocketAddrSet = NULL;
  u_long         *SocketPortSet = NULL;


  char            tmpLine[1024];
  long            OrigState = 0;

  if (SState != NULL)
    OrigState = *SState;

  // set timeout for ppoll()
  ts.tv_sec = waittime;
  ts.tv_nsec = 0;

  MaxNumDescriptors = get_max_num_descriptors();

  PollArraySize = MaxNumDescriptors * sizeof(struct pollfd);
  PollArray = (struct pollfd *)malloc(PollArraySize);
  
  if (PollArray == NULL)
    {
    return(-1);
    }

  // since we don't want to hold the mutex for too long,
  //   just make a local copy of the data and act on it

  pthread_mutex_lock(global_sock_read_mutex);
 
  // copy global array to local one 
  memcpy(PollArray, GlobalSocketReadArray, PollArraySize);

  SetSize = MaxNumDescriptors * sizeof(u_long);

  SocketAddrSet = (u_long *)malloc(SetSize);
  SocketPortSet = (u_long *)malloc(SetSize);

  if ((SocketAddrSet == NULL) || (SocketPortSet == NULL))
    {
    free(PollArray);

    if (SocketAddrSet != NULL)
      free(SocketAddrSet);
    if (SocketPortSet != NULL)
      free(SocketPortSet);

    pthread_mutex_unlock(global_sock_read_mutex);
    return (-1);
    }

  memcpy(SocketAddrSet, GlobalSocketAddrSet, SetSize);
  memcpy(SocketPortSet, GlobalSocketPortSet, SetSize);

  pthread_mutex_unlock(global_sock_read_mutex);

  n = ppoll(PollArray, MaxNumDescriptors, &ts, NULL);

  if (n == -1)
    {
     if (errno == EINTR)
      {
      n = 0; /* interrupted, cycle around */
      }
    else
      {
      int i;

      struct stat fbuf;

      /* check all file descriptors to verify they are valid */

      for (i = 0; i < MaxNumDescriptors; i++)
        {
        // skip this entry, fd not set
        if (PollArray[i].fd < 0)
          continue;

        // skip this entry, return events present
        if (PollArray[i].revents != 0)
          continue;

        // skip this entry, it's a valid descriptor
        if (fstat(i, &fbuf) == 0)
          continue;

        // remove socket from the global set since it's no longer valid
        globalset_del_sock(i);
        } /* END for each socket in global read array */

      free(PollArray);
      free(SocketAddrSet);
      free(SocketPortSet);

      log_err(errno, __func__, "Unable to poll sockets to read requests");

      return(-1);
      }  /* END else (errno == EINTR) */
    }    /* END if (n == -1) */

  for (i = 0; (n > 0) && (i < max_connection); i++)
    {
    // skip entry if it has no return events
    if (PollArray[i].revents == 0)
      continue;

    // decrement the count of structures that have non-zero revents
    n--;

    // is there data ready to read?
    if ((PollArray[i].revents & POLLIN))
      {
      pthread_mutex_lock(svr_conn[i].cn_mutex);
      /* this socket has data */

      svr_conn[i].cn_lasttime = time(NULL);

      if (svr_conn[i].cn_active != Idle)
        {
        void *(*func)(void *) = svr_conn[i].cn_func;

        netcounter_incr();

        pthread_mutex_unlock(svr_conn[i].cn_mutex);

        if (func != NULL)
          {
          int args[3];

          args[0] = i;
          args[1] = (int)SocketAddrSet[i];
          args[2] = (int)SocketPortSet[i];
          func((void *)args);
          }

        /* NOTE:  breakout if state changed (probably received shutdown request) */

        if ((SState != NULL) && 
            (OrigState != *SState))
          break;
        }
      else
        {
        pthread_mutex_unlock(svr_conn[i].cn_mutex);

        globalset_del_sock(i);
        close_conn(i, FALSE);

        pthread_mutex_lock(num_connections_mutex);

        sprintf(tmpLine, "closed connections to fd %d - num_connections=%d (poll bad socket)",
          i,
          num_connections);

        pthread_mutex_unlock(num_connections_mutex);
        log_err(-1, __func__, tmpLine);
        }
      }
    } /* END for i */

  free(PollArray);
  free(SocketAddrSet);
  free(SocketPortSet);

  /* NOTE:  break out if shutdown request received */

  if ((SState != NULL) && (OrigState != *SState))
    return(0);

  /* have any connections timed out ?? */

  now = time((time_t *)0);

  for (i = 0; i < max_connection; i++)
    {
    struct connection *cp;

    pthread_mutex_lock(svr_conn[i].cn_mutex);

    cp = &svr_conn[i];

    if (cp->cn_active != FromClientDIS)
      {
      pthread_mutex_unlock(svr_conn[i].cn_mutex);

      continue;
      }

    if ((now - cp->cn_lasttime) <= PBS_NET_MAXCONNECTIDLE)
      {
      pthread_mutex_unlock(svr_conn[i].cn_mutex);
  
      continue;
      }

    if (cp->cn_authen & PBS_NET_CONN_NOTIMEOUT)
      {
      pthread_mutex_unlock(svr_conn[i].cn_mutex);
  
      continue; /* do not time-out this connection */
      }

    /* NOTE:  add info about node associated with connection - NYI */

    {
    char buf[80];

    snprintf(tmpLine, sizeof(tmpLine), "connection %d to host %s has timed out after %d seconds - closing stale connection\n",
      i,
      netaddr_long(cp->cn_addr, buf),
      PBS_NET_MAXCONNECTIDLE);
    }
    
    log_err(-1, __func__, tmpLine);

    /* locate node associated with interface, mark node as down until node responds */

    /* NYI */

    close_conn(i, TRUE);

    pthread_mutex_unlock(svr_conn[i].cn_mutex);
    }  /* END for (i) */

  return(PBSE_NONE);
  }  /* END wait_request() */





/*
 * accept_conn - accept request for new connection
 * this routine is normally associated with the main socket,
 * requests for connection on the socket are accepted and
 * the new socket is added to the poll set and the connection
 * structure - the processing routine is set to the external
 * function: process_request(socket)
 *
 * NOTE: accept conn is called by functions that have a mutex on the socket already 
 */

void *accept_conn(

  void *new_conn)  /* main socket with connection request pending */

  {
  int                newsock;
  struct sockaddr_in from;
  struct sockaddr_un unixfrom;
  torque_socklen_t   fromsize;
  int                sd = *(int *)new_conn;
  unsigned short     sock_type = 0;
  enum conn_type     cn_active;

  from.sin_addr.s_addr = 0;
  from.sin_port = 0;

  pthread_mutex_lock(num_connections_mutex);
  if (num_connections >= max_connection)
    {
    pthread_mutex_unlock(num_connections_mutex);

    return(NULL);
    }
  pthread_mutex_unlock(num_connections_mutex);

  /* update lasttime of main socket */
  pthread_mutex_lock(svr_conn[sd].cn_mutex);
  svr_conn[sd].cn_lasttime = time((time_t *)0);
  sock_type = svr_conn[sd].cn_socktype;
  cn_active = svr_conn[sd].cn_active;
  pthread_mutex_unlock(svr_conn[sd].cn_mutex);

  if (sock_type == PBS_SOCK_INET)
    {
    fromsize = sizeof(from);
    newsock = accept(sd, (struct sockaddr *) & from, &fromsize);
    }
  else
    {
    fromsize = sizeof(unixfrom);
    newsock = accept(sd, (struct sockaddr *) & unixfrom, &fromsize);
    }

  if (newsock == -1)
    {
    }
  else if (newsock >= PBS_NET_MAX_CONNECTIONS)
    {
    close(newsock);
    }
  else
    {
    /* add the new socket to the poll set and connection structure */
    add_conn(
      newsock,
      FromClientDIS,
      (pbs_net_t)ntohl(from.sin_addr.s_addr),
      (unsigned int)ntohs(from.sin_port),
      sock_type,
      read_func[cn_active]);
    }


  return(NULL);
  }  /* END accept_conn() */




void globalset_add_sock(

  int sock,
  u_long addr,
  u_long port)

  {
  pthread_mutex_lock(global_sock_read_mutex);
  GlobalSocketReadArray[sock].fd = sock;
  GlobalSocketReadArray[sock].events = POLLIN;
  GlobalSocketAddrSet[sock] = addr;
  GlobalSocketPortSet[sock] = port;
  pthread_mutex_unlock(global_sock_read_mutex);
  } /* END globalset_add_sock() */



 
void globalset_del_sock(

  int sock)

  {
  pthread_mutex_lock(global_sock_read_mutex);
  GlobalSocketReadArray[sock].events = 0;
  GlobalSocketReadArray[sock].revents = 0;
  GlobalSocketAddrSet[sock] = 0;
  GlobalSocketPortSet[sock] = 0;
  pthread_mutex_unlock(global_sock_read_mutex);
  } /* END globalset_del_sock() */



/*
 * add_connection - add a connection to the svr_conn array.
 * The params addr and port are in host order.
 *
 * NOTE:  This routine cannot fail.
 */

int add_connection(

  int            sock,    /* socket associated with connection */
  enum conn_type type,    /* type of connection */
  pbs_net_t      addr,    /* IP address of connected host */
  unsigned int   port,    /* port number (host order) on connected host */
  unsigned int   socktype, /* inet or unix */
  void *(*func)(void *),  /* function to invoke on data rdy to read */
  int            add_wait_request) /* True to add into global poll set */

  {
  if ((sock < 0) ||
      (sock >= PBS_NET_MAX_CONNECTIONS))
    {
    char log_buf[LOCAL_LOG_BUF_SIZE];

    snprintf(log_buf, sizeof(log_buf), "Ignoring request to add connection for invalid socket %d", sock);
    log_err(PBSE_BAD_PARAMETER, __func__, log_buf);
    return(PBSE_BAD_PARAMETER);
    }

  if (num_connections_mutex == NULL)
    {
    usleep(100000);
    /* To solve a race condition where there are jobs in the queue, but the network hasn't been initialized yet. */
    return PBSE_SOCKET_FAULT;
    }
  pthread_mutex_lock(num_connections_mutex);
  num_connections++;
  pthread_mutex_unlock(num_connections_mutex);

  if (add_wait_request)
    {
    globalset_add_sock(sock,(u_long)addr,port);
    }
  else
    {
    /* just to make sure it's cleared */
    globalset_del_sock(sock);
    }

  pthread_mutex_lock(svr_conn[sock].cn_mutex);

  svr_conn[sock].cn_active   = type;
  svr_conn[sock].cn_addr     = addr;
  svr_conn[sock].cn_port     = (unsigned short)port;
  svr_conn[sock].cn_lasttime = time((time_t *)0);
  svr_conn[sock].cn_func     = func;
  svr_conn[sock].cn_oncl     = 0;
  svr_conn[sock].cn_socktype = socktype;

#ifndef NOPRIVPORTS

  if ((socktype == PBS_SOCK_INET) && (port < IPPORT_RESERVED))
    {
    svr_conn[sock].cn_authen = PBS_NET_CONN_FROM_PRIVIL;
    }
  else
    {
    /* AF_UNIX sockets */
    svr_conn[sock].cn_authen = 0;
    }

#else /* !NOPRIVPORTS */

  if (socktype == PBS_SOCK_INET)
    {
    /* All TCP connections are privileged */
    svr_conn[sock].cn_authen = PBS_NET_CONN_FROM_PRIVIL;
    }
  else
    {
    /* AF_UNIX sockets */
    svr_conn[sock].cn_authen = 0;
    }

#endif /* !NOPRIVPORTS */

  pthread_mutex_unlock(svr_conn[sock].cn_mutex);

  return(PBSE_NONE);
  }  /* END add_connection() */



/*
 * add_conn - add a connection to the svr_conn array.
 * The params addr and port are in host order.
 *
 * NOTE:  This routine cannot fail.
 */
#ifdef __cplusplus
extern "C"
{
#endif
int add_conn(

  int            sock,    /* socket associated with connection */
  enum conn_type type,    /* type of connection */
  pbs_net_t      addr,    /* IP address of connected host */
  unsigned int   port,    /* port number (host order) on connected host */
  unsigned int   socktype, /* inet or unix */
  void *(*func)(void *))  /* function to invoke on data rdy to read */

  {
  return(add_connection(sock, type, addr, port, socktype, func, TRUE));
  }  /* END add_conn() */
#ifdef __cplusplus
}
#endif



/*
 * add_scheduler_conn - add a connection to the svr_conn array.
 * The params addr and port are in host order.
 * This version is specific to connections the server makes to the scheduler.
 * These connections must be outside the control of wait_request().
 *
 * NOTE:  This routine cannot fail.
 */

int add_scheduler_conn(

  int            sock,    /* socket associated with connection */
  enum conn_type type,    /* type of connection */
  pbs_net_t      addr,    /* IP address of connected host */
  unsigned int   port,    /* port number (host order) on connected host */
  unsigned int   socktype, /* inet or unix */
  void *(*func)(void *))  /* function to invoke on data rdy to read */

  {
  return(add_connection(sock, type, addr, port, socktype, func, FALSE));
  }  /* END add_scheduler_conn() */





/*
 * close_conn - close a network connection
 * does physical close, also marks the connection table
 */

void close_conn(

  int sd,        /* I */
  int has_mutex) /* I */

  {
  char log_message[LOG_BUF_SIZE];

  /* close conn shouldn't be called on local connections */
  if (sd == PBS_LOCAL_CONNECTION)
    return;
  
  if ((sd < 0) ||
      (max_connection <= sd))
    {
    snprintf(log_message, sizeof(log_message), "sd is invalid %d!!!", sd);
    log_event(PBSEVENT_SYSTEM,PBS_EVENTCLASS_NODE,__func__,log_message);

    return;
    }

  if (has_mutex == FALSE)
    pthread_mutex_lock(svr_conn[sd].cn_mutex);

  if (svr_conn[sd].cn_active == Idle)
    {
    if (has_mutex == FALSE)
      pthread_mutex_unlock(svr_conn[sd].cn_mutex);

    return;
    }

  /* if there is a function to call on close, do it */

  if (svr_conn[sd].cn_oncl != NULL)
    {
    snprintf(log_message, sizeof(log_message),
      "Closing connection %d and calling its accompanying function on close",
      sd);
    log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_NODE, __func__, log_message);

    svr_conn[sd].cn_oncl(sd);
    }

  /* 
   * In the case of a -t cold start, this will be called prior to
   * GlobalSocketReadArray being initialized
   */

  if (GlobalSocketReadArray != NULL)
    {
    globalset_del_sock(sd);
    }

  close(sd);

  svr_conn[sd].cn_addr = 0;
  svr_conn[sd].cn_handle = -1;
  svr_conn[sd].cn_active = Idle;
  svr_conn[sd].cn_func = (void *(*)(void *))0;
  svr_conn[sd].cn_authen = 0;
  svr_conn[sd].cn_stay_open = FALSE;
    
  if (has_mutex == FALSE)
    pthread_mutex_unlock(svr_conn[sd].cn_mutex);

  pthread_mutex_lock(num_connections_mutex);
  num_connections--;
  pthread_mutex_unlock(num_connections_mutex);

  return;
  }  /* END close_conn() */


/*
 * clear_conn is used to reset the connection table for 
 * a socket that has been already closed by pbs_disconnect
 * but the socket was added to the svr_conn table with add_conn
 * or some other method. This is the case for the socket
 * used in obit_reply
 */

void clear_conn(

  int sd,        /* I */
  int has_mutex) /* I */

  {
  char log_message[LOG_BUF_SIZE];

  /* close conn shouldn't be called on local connections */
  if (sd == PBS_LOCAL_CONNECTION)
    return;
  
  if ((sd < 0) ||
      (max_connection <= sd))
    {
    snprintf(log_message, sizeof(log_message), "sd is invalid %d!!!", sd);
    log_event(PBSEVENT_SYSTEM,PBS_EVENTCLASS_NODE,__func__,log_message);

    return;
    }

  if (has_mutex == FALSE)
    pthread_mutex_lock(svr_conn[sd].cn_mutex);

  if (svr_conn[sd].cn_active == Idle)
    {
    if (has_mutex == FALSE)
      pthread_mutex_unlock(svr_conn[sd].cn_mutex);

    return;
    }

  /* 
   * In the case of a -t cold start, this will be called prior to
   * GlobalSocketReadArray being initialized
   */

  if (GlobalSocketReadArray != NULL)
    {
    globalset_del_sock(sd);
    }

  svr_conn[sd].cn_addr = 0;
  svr_conn[sd].cn_handle = -1;
  svr_conn[sd].cn_active = Idle;
  svr_conn[sd].cn_func = (void *(*)(void *))0;
  svr_conn[sd].cn_authen = 0;
  svr_conn[sd].cn_stay_open = FALSE;
    
  if (has_mutex == FALSE)
    pthread_mutex_unlock(svr_conn[sd].cn_mutex);

  pthread_mutex_lock(num_connections_mutex);
  num_connections--;
  pthread_mutex_unlock(num_connections_mutex);

  return;
  }  /* END close_conn() */





/*
 * net_close - close all network connections but the one specified,
 * if called with impossible socket number (-1), all will be closed.
 * This function is typically called when a server is closing down and
 * when it is forking a child.
 *
 * We clear the cn_oncl field in the connection table to prevent any
 * "special on close" functions from being called.
 */

void net_close(

  int but)  /* I */

  {
  int i;

  for (i = 0; i < max_connection; i++)
    {
    if (i != but)
      {
      pthread_mutex_lock(svr_conn[i].cn_mutex);

      svr_conn[i].cn_oncl = NULL;

      close_conn(i,TRUE);

      pthread_mutex_unlock(svr_conn[i].cn_mutex);
      }
    }    /* END for (i) */

  return;
  }  /* END net_close() */




/*
 * get_connectaddr - return address of host connected via the socket
 * This is in host order.
 *
 * mutex is TRUE if the mutex should be obtained, false otherwise
 */
pbs_net_t get_connectaddr(

  int sock,   /* I */
  int mutex)  /* I */

  {
  pbs_net_t tmp;

  if (mutex == TRUE)
    pthread_mutex_lock(svr_conn[sock].cn_mutex);

  tmp = svr_conn[sock].cn_addr;

  if (mutex == TRUE)
    pthread_mutex_unlock(svr_conn[sock].cn_mutex);

  return(tmp);
  } /* END get_connectaddr() */




void set_localhost_name(
    
  char   *localhost_name,
  size_t  len)

  {
  struct sockaddr sa;
  int             rc;
 
  memset(&sa, 0, sizeof(struct sockaddr));

  sa.sa_family = AF_INET;
  sa.sa_data[2] = 0x7F;
  sa.sa_data[5] = 1;

  if ((rc = getnameinfo(&sa, sizeof(sa), local_host_name, local_host_name_len, NULL, 0, 0)) != 0)
    {
    strcpy(local_host_name, "localhost");
    strncpy(localhost_name, "localhost", len);
    }
  else
    strncpy(localhost_name, local_host_name, len);
  } /* END set_localhost_name() */




/*
 * get_connecthost - return name of host connected via the socket
 */

int get_connecthost(

  int   sock,     /* I */
  char *namebuf,  /* O (minsize=size) */
  int   size)     /* I */

  {
  struct in_addr          addr;

  struct sockaddr        *addr_info_ptr;
  struct sockaddr_in      addr_in;
  const char             *name;
  int                     socktype_flag;

  addr_in.sin_family = AF_INET;
  addr_in.sin_port = 0;

  size--;

  pthread_mutex_lock(svr_conn[sock].cn_mutex);
  addr.s_addr = htonl(svr_conn[sock].cn_addr);
  socktype_flag = svr_conn[sock].cn_socktype & PBS_SOCK_UNIX;
  pthread_mutex_unlock(svr_conn[sock].cn_mutex);

  addr_in.sin_addr = addr;
  addr_info_ptr = (struct sockaddr *)&addr_in;

  if ((net_server_name != NULL) &&
      (socktype_flag != 0))
    {
    /* lookup request is for local server */

    strcpy(namebuf, net_server_name);
    }
  else if ((net_server_name != NULL) &&
           (addr.s_addr == net_serveraddr.s_addr))
    {
    /* lookup request is for local server */

    snprintf(namebuf, size, "%s", net_server_name);
    }
  else if ((name = get_cached_nameinfo(&addr_in)) != NULL)
    {
    snprintf(namebuf, size, "%s", name);
    }
  else if (getnameinfo(addr_info_ptr, sizeof(addr_in), namebuf, size, NULL, 0, 0) != 0)
    {
    snprintf(namebuf, size, "%s", inet_ntoa(addr));
    }
  else
    {
    if (strcmp(namebuf, local_host_name) == 0)
      {
      snprintf(namebuf, size, "%s", local_host_name);
      }
    else
      {
      /* already in namebuf, NO-OP */
      }
    }

  /* SUCCESS */

  return(PBSE_NONE);
  }  /* END get_connecthost() */


/*
** Put a human readable representation of a network address into
** a staticly allocated string.
*/
char *netaddr_pbs_net_t(

  pbs_net_t ipadd)

  {
  char  out[80];
  char *return_value;

  if (ipadd == 0)
    {
    strcpy(out, "unknown");
    }
  else
    {
    sprintf(out, "%ld.%ld.%ld.%ld",
          (ipadd & 0xff000000) >> 24,
          (ipadd & 0x00ff0000) >> 16,
          (ipadd & 0x0000ff00) >> 8,
          (ipadd & 0x000000ff));
    }

  return_value = (char *)calloc(1, strlen(out) + 1);
  strcpy(return_value,out);

  return(return_value);
  }

/* END net_server.c */

