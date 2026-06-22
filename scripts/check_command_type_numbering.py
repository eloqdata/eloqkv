#!/usr/bin/env python3
"""Verify RedisCommandType is an append-only, explicitly-numbered wire format.

RedisCommandType (include/redis_command.h) is serialized as the first byte of
every command in the WAL / replication stream. Its numeric values are therefore
a stable on-disk / on-wire contract that must never change. This script parses
the enum and asserts every member's explicit value matches the frozen table
below. Run it in CI; a mismatch means someone renumbered/reordered a command and
broke cross-version compatibility.

Usage: python3 scripts/check_command_type_numbering.py
Exits non-zero on any mismatch.
"""
import os
import re
import sys

HEADER = os.path.join(os.path.dirname(__file__), os.pardir, "include",
                      "redis_command.h")

# Frozen numbering of the always-compiled ("base") members: the 1.3.1 release
# build values. NEVER edit an existing entry; only append new commands.
BASE = """
NOOP=0, ECHO=1, PING=2, AUTH=3, QUIT=4, SELECT=5, CLIENT=6, UNKNOWN=7,
GET=8, SET=9, GETDEL=10, SETNX=11, GETSET=12, MGET=13, MSET=14, MSETNX=15,
STRLEN=16, PSETEX=17, SETEX=18, GETBIT=19, GETRANGE=20, INCRBYFLOAT=21,
SETBIT=22, SETRANGE=23, APPEND=24, SUBSTR=25, INCR=26, INCRBY=27, DECR=28,
DECRBY=29, BITCOUNT=30, BITFIELD=31, BITFIELD_RO=32, BITPOS=33, BITOP=34,
BLOCKPOP=35, BLOCKDISCARD=36, BLMOVE=37, BLMPOP=38, BLPOP=39, BRPOP=40,
BRPOPLPUSH=41, LINDEX=42, LINSERT=43, LPOS=44, LLEN=45, LPOP=46, LPUSH=47,
LPUSHX=48, LRANGE=49, LMOVE=50, LMOVEPOP=51, LMOVEPUSH=52, LREM=53, LSET=54,
LTRIM=55, RPOP=56, RPOPLPUSH=57, RPUSH=58, RPUSHX=59, LMPOP=60, HGET=61,
HSET=62, HSETNX=63, HLEN=64, HSTRLEN=65, HINCRBY=66, HINCRBYFLOAT=67, HMGET=68,
HKEYS=69, HVALS=70, HGETALL=71, HEXISTS=72, HDEL=73, HRANDFIELD=74, HSCAN=75,
ZADD=76, ZCOUNT=77, ZCARD=78, ZRANGE=79, ZRANGESTORE=80, ZREM=81, ZSCORE=82,
ZRANGEBYSCORE=83, ZRANGEBYLEX=84, ZRANGEBYRANK=85, ZREMRANGE=86, ZLEXCOUNT=87,
ZPOPMIN=88, ZPOPMAX=89, ZSCAN=90, ZUNION=91, ZUNIONSTORE=92, ZINTER=93,
ZINTERCARD=94, ZINTERSTORE=95, ZRANDMEMBER=96, ZRANK=97, ZREVRANK=98,
ZREVRANGE=99, ZREVRANGEBYSCORE=100, ZREVRANGEBYLEX=101, ZMSCORE=102, ZMPOP=103,
ZDIFF=104, ZDIFFSTORE=105, ZINCRBY=106, SZSCAN=107, SADD=108, SMEMBERS=109,
SREM=110, SCARD=111, SDIFF=112, SDIFFSTORE=113, SINTER=114, SINTERCARD=115,
SINTERSTORE=116, SISMEMBER=117, SMISMEMBER=118, SMOVE=119, SPOP=120,
SRANDMEMBER=121, SUNION=122, SUNIONSTORE=123, SSCAN=124, STORE_LIST=125,
SORTABLE_LOAD=126, MHGET=127, SORT=128, WATCH=129, UNWATCH=130, MULTI=131,
EXEC=132, DISCARD=133, BEGIN=134, COMMIT=135, ROLLBACK=136, SCAN=137, KEYS=138,
TYPE=139, DEL=140, EXISTS=141, DUMP=142, RESTORE=143, DBSIZE=144, INFO=145,
COMMAND=146, CONFIG=147, CLUSTER=148, FAILOVER=149, FLUSHDB=150, FLUSHALL=151,
READONLY=152, SLOWLOG=153, EVAL=154, EVALSHA=155, MEMORY_USAGE=156, TTL=157,
PTTL=158, EXPIRETIME=159, PEXPIRETIME=160, EXPIRE=161, PEXPIRE=162,
EXPIREAT=163, PEXPIREAT=164, PERSIST=165, GETEX=166, RECOVER=167, TIME=168,
PUBLISH=169, SUBSCRIBE=170, UNSUBSCRIBE=171, PSUBSCRIBE=172, PUNSUBSCRIBE=173,
UNLINK=174, NAMESPACE=175,
"""

# Build-flag-gated members live in a reserved trailing range, fixed forever.
RESERVED = {
    "COMPACT": 176, "FAULT_INJECT": 177,
    "ELOQVEC_CREATE": 178, "ELOQVEC_INFO": 179, "ELOQVEC_DROP": 180,
    "ELOQVEC_ADD": 181, "ELOQVEC_BADD": 182, "ELOQVEC_UPDATE": 183,
    "ELOQVEC_DELETE": 184, "ELOQVEC_SEARCH": 185,
}


def expected():
    exp = dict(RESERVED)
    for tok in BASE.replace("\n", " ").split(","):
        tok = tok.strip()
        if not tok:
            continue
        name, val = tok.split("=")
        exp[name.strip()] = int(val)
    return exp


def actual():
    with open(HEADER) as f:
        lines = f.read().splitlines()
    i0 = next(i for i, l in enumerate(lines)
              if l.strip() == "enum struct RedisCommandType")
    i1 = next(i for i, l in enumerate(lines[i0:], i0) if l.strip() == "};")
    out = {}
    for l in lines[i0:i1]:
        m = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(\d+)\s*,", l)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def main():
    exp, act = expected(), actual()
    errs = []
    for name, v in exp.items():
        if name not in act:
            errs.append(f"missing in header: {name} (expected {v})")
        elif act[name] != v:
            errs.append(f"value changed: {name} is {act[name]}, expected {v}")
    for name, v in act.items():
        if name not in exp:
            errs.append(f"unexpected member (append to this script too): "
                        f"{name}={v}")
    dups = {}
    for name, v in act.items():
        dups.setdefault(v, []).append(name)
    for v, names in sorted(dups.items()):
        if len(names) > 1:
            errs.append(f"duplicate value {v}: {', '.join(names)}")
        if v > 255:
            errs.append(f"value {v} ({', '.join(names)}) exceeds uint8_t cap")
    if errs:
        print("RedisCommandType numbering check FAILED:")
        for e in errs:
            print("  -", e)
        return 1
    print(f"RedisCommandType numbering OK ({len(act)} members, "
          f"max={max(act.values())}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
