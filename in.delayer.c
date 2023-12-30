/*
  cc -Wall -o in.delayer in.delayer.c `pkg-config --cflags --libs sqlite3`
*/
/*
  Copyright (C) 2023  Hideki EIRAKU

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
#include <linux/tcp.h>
#include <poll.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <sqlite3.h>

#define SLEEP_FOR_ERROR 30
#define MAX_SLEEP_TIME_TEXT "60"
#define MAX_SLEEP_TIME_VAL 60
#define DELAY_AFTER_TIMEOUT 10
#define GOOD_SEGS_IN_THRESHOLD 16
#define GOOD_TIME_THRESHOLD 300
#define GOOD_BYTES_THRESHOLD 4096

/*
Create DB as follows:

$ sqlite3 /path/to/dbfile
SQLite version 3.40.1 2022-12-28 14:03:47
Enter ".help" for usage hints.
sqlite> PRAGMA journal_mode=TRUNCATE
sqlite> CREATE TABLE hosts(host TEXT PRIMARY KEY,sleep INTEGER);
sqlite> CREATE INDEX host_idx ON hosts(host);
sqlite> .quit

inetd.conf example:

ssh stream tcp nowait root:root /path/to/in.delayer in.delayer /path/to/dbfile 65534 65534 /usr/sbin/sshd -i
*/

enum db_mode
  {
    DB_SLEEP,
    DB_RECORD_BAD,
    DB_RECORD_GOOD,
  };

static void
myerr (int eval, const char *fmt, ...)
{
  char *err = strerror (errno);
  va_list ap;
  va_start (ap, fmt);
  openlog (NULL, LOG_PID, LOG_DAEMON);
  vsyslog (LOG_ERR, fmt, ap);
  syslog (LOG_ERR, "Error: %s", err);
  closelog ();
  va_end (ap);
  exit (eval);
}

static void
myerrx (int eval, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  openlog (NULL, LOG_PID, LOG_DAEMON);
  vsyslog (LOG_ERR, fmt, ap);
  closelog ();
  va_end (ap);
  exit (eval);
}

static void
mywarn (const char *fmt, ...)
{
  char *err = strerror (errno);
  va_list ap;
  va_start (ap, fmt);
  openlog (NULL, LOG_PID, LOG_DAEMON);
  vsyslog (LOG_WARNING, fmt, ap);
  syslog (LOG_WARNING, "Warning: %s", err);
  closelog ();
  va_end (ap);
}

static void
mywarnx (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  openlog (NULL, LOG_PID, LOG_DAEMON);
  vsyslog (LOG_WARNING, fmt, ap);
  closelog ();
  va_end (ap);
}

static void
get_hostname (int fd, size_t buflen, char buf[buflen])
{
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof addr;
  if (getpeername (fd, (struct sockaddr *)&addr, &addr_len) < 0)
    myerr (1, "getpeername");
  if (getnameinfo ((struct sockaddr *)&addr, addr_len, buf, buflen, NULL, 0,
		   NI_NUMERICHOST) < 0)
    myerr (1, "getnameinfo");
}

static int
db_access (char *host, char *db_name, int db_uid, int db_gid,
	   enum db_mode mode)
{
  if (setgid (db_gid) < 0 || setuid (db_uid) < 0)
    {
      mywarn ("setgid or setuid");
      if (mode == DB_SLEEP)
	sleep (SLEEP_FOR_ERROR);
      return 1;
    }
  if (db_uid > 0 && !setuid (0))
    {
      mywarnx ("setuid still usable");
      if (mode == DB_SLEEP)
	sleep (SLEEP_FOR_ERROR);
      return 1;
    }
  sqlite3 *db;
  sqlite3_stmt *res;
  int rc;
  rc = sqlite3_open_v2 (db_name, &db, mode == DB_SLEEP ?
			SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, NULL);
  if (rc != SQLITE_OK)
    myerrx (1, "sqlite3_open %s", sqlite3_errmsg (db));
  if (mode != DB_SLEEP)
    {
      rc = sqlite3_prepare_v2 (db, "PRAGMA journal_mode=TRUNCATE",
			       -1, &res, 0);
      if (rc != SQLITE_OK)
	myerrx (1, "sqlite3_prepare_v2 %s", sqlite3_errmsg (db));
      rc = sqlite3_step (res);
      if (rc != SQLITE_ROW)
	myerrx (1, "sqlite3_step %s", sqlite3_errmsg (db));
      sqlite3_finalize (res);
    }
  int sleep_time = 0;
  switch (mode)
    {
    case DB_SLEEP:
      rc = sqlite3_prepare_v2 (db, "SELECT sleep FROM hosts WHERE host = ?",
			       -1, &res, 0);
      if (rc != SQLITE_OK)
	myerrx (1, "sqlite3_prepare_v2 %s", sqlite3_errmsg (db));
      sqlite3_bind_text (res, 1, host, strlen (host), SQLITE_STATIC);
      rc = sqlite3_step (res);
      if (rc == SQLITE_ROW)
	sleep_time = sqlite3_column_int (res, 0);
      sqlite3_finalize (res);
      break;
    case DB_RECORD_BAD:
      rc = sqlite3_prepare_v2 (db, "INSERT INTO hosts(host,sleep) VALUES(?,1)"
			       " ON CONFLICT(host)"
			       " DO UPDATE SET sleep=min(sleep+1,"
			       MAX_SLEEP_TIME_TEXT ")",
			       -1, &res, 0);
      if (rc != SQLITE_OK)
	myerrx (1, "sqlite3_prepare_v2 %s", sqlite3_errmsg (db));
      sqlite3_bind_text (res, 1, host, strlen (host), SQLITE_STATIC);
      rc = sqlite3_step (res);
      if (rc != SQLITE_DONE)
	myerrx (1, "sqlite3_step %s", sqlite3_errmsg (db));
      sqlite3_finalize (res);
      break;
    case DB_RECORD_GOOD:
      rc = sqlite3_prepare_v2 (db, "DELETE FROM hosts WHERE host=?",
			       -1, &res, 0);
      if (rc != SQLITE_OK)
	myerrx (1, "sqlite3_prepare_v2 %s", sqlite3_errmsg (db));
      sqlite3_bind_text (res, 1, host, strlen (host), SQLITE_STATIC);
      rc = sqlite3_step (res);
      if (rc != SQLITE_DONE)
	myerrx (1, "sqlite3_step %s", sqlite3_errmsg (db));
      sqlite3_finalize (res);
      break;
    }
  sqlite3_close (db);
  if (sleep_time > 0)
    {
      if (sleep_time > MAX_SLEEP_TIME_VAL)
	sleep_time = MAX_SLEEP_TIME_VAL;
      sleep (sleep_time);
    }
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc < 5)
    myerrx (1, "usage: %s database uid gid program parameters", argv[0]);
  int db_uid = atoi (argv[2]);
  int db_gid = atoi (argv[3]);
  char host[512];
  get_hostname (0, sizeof host, host);
  pid_t pid;
  /* First fork: for database access with restricted privilege */
  pid = fork ();
  if (pid < 0)
    myerr (1, "fork");
  if (!pid)
    return db_access (host, argv[1], db_uid, db_gid, DB_SLEEP);
  int wstatus;
  if (waitpid (pid, &wstatus, 0) < 0)
    myerr (1, "wait");
  /* Check whether the connection was closed during sleep */
  struct tcp_info tcp_info;
  socklen_t tcp_info_len;
  tcp_info_len = sizeof tcp_info;
  if (getsockopt (0, IPPROTO_TCP, TCP_INFO, &tcp_info, &tcp_info_len) < 0)
    myerr (1, "getsockopt");
  if (tcp_info.tcpi_state == 8 /* TCP_CLOSE_WAIT */)
    {
      sleep (DELAY_AFTER_TIMEOUT);
      return 1;
    }
  /* Second fork: execute program */
  pid = fork ();
  if (pid < 0)
    myerr (1, "fork");
  if (!pid)
    {
      extern char **environ;
      execve (argv[4], &argv[4], environ);
      myerr (1, "execve");
    }
  struct timespec t1;
  if (clock_gettime (CLOCK_MONOTONIC, &t1) < 0)
    myerr (1, "clock_gettime");
  if (waitpid (pid, &wstatus, 0) < 0)
    myerr (1, "wait");
  struct timespec t2;
  if (clock_gettime (CLOCK_MONOTONIC, &t2) < 0)
    myerr (1, "clock_gettime");
  tcp_info_len = sizeof tcp_info;
  if (getsockopt (0, IPPROTO_TCP, TCP_INFO, &tcp_info, &tcp_info_len) < 0)
    myerr (1, "getsockopt");
  /* Sometimes "sshd [net]", privilege separation process, remains.
     To stop the process, shut the connection down forcefully. */
  if (shutdown (0, SHUT_RDWR) < 0 && errno != ENOTCONN)
    mywarn ("shutdown");
  /* Number of segs in less than GOOD_SEGS_IN_THRESHOLD looks bad */
  if (tcp_info.tcpi_segs_in < GOOD_SEGS_IN_THRESHOLD)
    goto bad;
  /* Connection more than GOOD_TIME_THRESHOLD seconds looks good */
  if (t2.tv_sec - t1.tv_sec >= GOOD_TIME_THRESHOLD)
    goto good;
  /* Number of acked bytes less than GOOD_BYTES_THRESHOLD looks bad */
  if (tcp_info.tcpi_bytes_acked >= GOOD_BYTES_THRESHOLD)
    goto good;
 bad:
  return db_access (host, argv[1], db_uid, db_gid, DB_RECORD_BAD);
 good:
  return db_access (host, argv[1], db_uid, db_gid, DB_RECORD_GOOD);
}
