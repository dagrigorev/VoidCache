/*
 * net/commands.h  –  VoidCache command dispatch table.
 *
 * Standard Redis commands supported:
 *   PING, HELLO, AUTH, SELECT, QUIT
 *   GET, SET, DEL, EXISTS, EXPIRE, TTL, PTTL, PERSIST, TYPE, RENAME
 *   KEYS, SCAN, DBSIZE, FLUSHDB, FLUSHALL
 *   APPEND, INCR, INCRBY, DECR, DECRBY, INCRBYFLOAT
 *   MGET, MSET, MSETNX
 *   INFO, CLIENT, CONFIG, DEBUG, COMMAND
 *
 * VoidCache extended commands:
 *   VCSET  <key> <type> <value>  – set with explicit type
 *   VCGET  <key>                 – get with type metadata in response
 *   VCJSET <key> <json>          – set JSON value (validates UTF-8)
 *   VCJGET <key>                 – get JSON value
 *   VCTYPE <key>                 – get VoidCache type byte
 *   VCINFO                       – extended server info (JSON)
 *   CLUSTER INFO                 – cluster topology
 *   CLUSTER NODES                – node list (Redis cluster format)
 *   CLUSTER MYID                 – this node's ID
 */
#pragma once
#include "server.h"

/* Dispatch one command.  Writes response into conn->wbuf. */
void vc_dispatch(vcserver_t *srv, vc_conn_t *conn, vc_cmd_t *cmd);
