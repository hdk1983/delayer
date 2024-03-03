# Delayer #

Delay before OpenSSH server on Linux to make SSH attacks slow down.

## How It Works ##

This program is started by inetd, with a TCP connection accepted by
inetd.  It sleeps for several seconds based on database record before
starting OpenSSH server, then starts the server if the connection
still alives.  After the server termination, it checks connection
information and elapsed time and classifies the client IP address as
follows:

1. If `tcpi_segs_in` is less than 16, the connection is bad.
2. If elapsed time is more than or equal to 300 seconds, the connection is good.
3. If `tcpi_bytes_acked` is less than 4096, the connection is bad.
4. Otherwise, the connection is good.

If the connection is good, the IP address is deleted from the
database.  The next connection from the IP address will have no sleep
time.

If the connection is bad, the IP address is recorded to the database.
The database contains sleep time in seconds for each IP address.  The
sleep time is 1 to 60, started with 1 and incremented every bad
connection.  Therefore, if the host at the IP address repeats bad
connection, it will have longer sleep time.

SQLite3 database is used.  The database access is executed in a
different user for restricted privilege while this program is started
with root user.

This program is not a proxy to make it simple and get good throughput.
The server communicates directly with the TCP socket.

When the sleep time is 60, "max-banner" file specified in fourth
parameter will be sent to the client.  The file is optional.

## Build ##

Use a C compiler with SQLite3 library:

```
$ cc -Wall -o in.delayer in.delayer.c `pkg-config --cflags --libs sqlite3`
```

Create an initial database file:

```
$ sqlite3 in.delayer.db
SQLite version 3.40.1 2022-12-28 14:03:47
Enter ".help" for usage hints.
sqlite> PRAGMA journal_mode=TRUNCATE;
sqlite> CREATE TABLE hosts(host TEXT PRIMARY KEY,sleep INTEGER);
sqlite> CREATE INDEX host_idx ON hosts(host);
sqlite> .quit
```

## Installation ##

Copy the binary to somewhere:

```
# install in.delayer /usr/local/sbin/
```

Copy the database file to somewhere (only if the file does not exist):

```
# test -e /srv/local/in.delayer.db || install -o nobody -g nogroup in.delayer.db* /srv/local/in.delayer.db
```

Create the max-banner file if needed:

```
# echo message-for-client > /srv/local/in.delayer.max-banner
```

Change a port number of OpenSSH server (or disable the server):

```
# editor /etc/ssh/sshd_config
# service ssh reload
```

Add inetd configuration:

```
# editor /etc/inetd.conf
# service openbsd-inetd reload
```

inetd configuration example:

```
ssh stream tcp nowait root:root /usr/local/sbin/in.delayer in.delayer /srv/local/in.delayer.db /srv/local/in.delayer.max-banner 65534 65534 /usr/sbin/sshd -i
```

## Monitor ##

Because of the delay, the delayed connections can easily be seen with
`ss` command:

```
$ watch "ss -nt|grep :22;sqlite3 /srv/local/in.delayer.db  'select * from hosts order by sleep desc limit 10;'"
```

If this program does not work properly, syslog might be useful for
debugging.  `attempt to write a readonly database` might be caused by
incorrect database file permission, or incorrect database directory
permission if journal file is not created.
