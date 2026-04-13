start_server {tags {"dump"}} {
    test {dump a non-exist key} {
        r del non-exists
        assert_equal {} [r dump non-exists]
    }

    test {dump restore string} {
        r del stringobj
        set stringval "Hello World!"
        r set stringobj $stringval
        set dumpval [r dump stringobj]
        r del stringobj
        r restore stringobj 0 $dumpval
        assert_equal $stringval [r get stringobj]
    }

    test {dump restore hash} {
        r del hashobj
        r hset hashobj k1 v1 k2 v2 k3 v3
        set dumpval [r dump hashobj]
        r del hashobj
        r restore hashobj 0 $dumpval
        assert_equal {k1 k2 k3} [lsort [r hkeys hashobj]]
        assert_equal {v1 v2 v3} [lsort [r hvals hashobj]]
    }

    test {dump restore set} {
        r del setobj
        r sadd setobj k1 k2 k3
        set dumpval [r dump setobj]
        r del setobj
        r restore setobj 0 $dumpval
        assert_equal {k1 k2 k3} [lsort [r smembers setobj]]
    }

    test {dump restore zset} {
        r del zsetobj
        r zadd zsetobj 1 k1 2 k2 3 k3
        set dumpval [r dump zsetobj]
        r del zsetobj
        r restore zsetobj 0 $dumpval
        assert_equal {k1 k2 k3} [r zrange zsetobj 0 -1]
    }

    test {RESTORE are able to serialize / unserialize a simple key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        list [r exists foo] [r restore foo 0 $encoded] [r ttl foo] [r get foo]
    } {0 OK -1 bar}

    test {RESTORE can set an arbitrary expire to the materialized key} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 5000 $encoded
        set ttl [r pttl foo]
        assert_range $ttl 3000 5500
        r get foo
    } {bar}

    test {RESTORE can set an expire that overflows a 32 bit integer} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 2569591501 $encoded
        set ttl [r pttl foo]
        assert_range $ttl [expr {2569591501-3000}] [expr {2569591501+500}]
        r get foo
    } {bar}

    test {RESTORE can set an absolute expire} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        set now [clock milliseconds]
        r restore foo [expr $now+3000] $encoded absttl
        set ttl [r pttl foo]
        assert_range $ttl 2000 3100
        r get foo
    } {bar}

    test {RESTORE expires a key after ttl elapses} {
        r set foo bar
        set encoded [r dump foo]
        r del foo
        r restore foo 100 $encoded
        after 250
        list [r exists foo] [r pttl foo]
    } {0 -2}

    test {RESTORE can set ttl on list payload} {
        r del ttl_list
        r rpush ttl_list a b c
        set encoded [r dump ttl_list]
        r del ttl_list
        r restore ttl_list 5000 $encoded
        set ttl [r pttl ttl_list]
        assert_range $ttl 3000 5500
        assert_equal {a b c} [r lrange ttl_list 0 -1]
    }

    test {RESTORE can set ttl on hash payload} {
        r del ttl_hash
        r hset ttl_hash f1 v1 f2 v2
        set encoded [r dump ttl_hash]
        r del ttl_hash
        r restore ttl_hash 5000 $encoded
        set ttl [r pttl ttl_hash]
        assert_range $ttl 3000 5500
        assert_equal {f1 f2} [lsort [r hkeys ttl_hash]]
        assert_equal {v1 v2} [lsort [r hvals ttl_hash]]
    }

    test {RESTORE can set ttl on set payload} {
        r del ttl_set
        r sadd ttl_set m1 m2 m3
        set encoded [r dump ttl_set]
        r del ttl_set
        r restore ttl_set 2569591501 $encoded
        set ttl [r pttl ttl_set]
        assert_range $ttl (2569591501-3000) 2569591501
        assert_equal {m1 m2 m3} [lsort [r smembers ttl_set]]
    }

    test {RESTORE can set absttl on zset payload} {
        r del ttl_zset
        r zadd ttl_zset 1 a 2 b
        set encoded [r dump ttl_zset]
        r del ttl_zset
        set now [clock milliseconds]
        r restore ttl_zset [expr $now+3000] $encoded absttl
        set ttl [r pttl ttl_zset]
        assert_range $ttl 2000 3100
        assert_equal {a b} [r zrange ttl_zset 0 -1]
    }

    test {restore with replace} {
        r del key1
        r del key2
        r set key1 "Hello World!"
        r hset key2 k1 v1 k2 v2 k3 v3

        set dumpkey1 [r dump key1]
        set dumpkey2 [r dump key2]

        r restore key1 0 $dumpkey2 replace
        assert_equal {k1 k2 k3} [lsort [r hkeys key1]]
        assert_equal {v1 v2 v3} [lsort [r hvals key1]]

        assert_error {*BUSYKEY*} {r restore key2 0 $dumpkey1}
    }

    # Payloads below were generated from local Redis 7.0.15 DUMP output.
    # The version 4 ziplist sample remains as a legacy compatibility case.

    test {restore redis dump payload for string} {
        r del redis_string
        set dumpval [binary format H* "0003666f6f0a00734939db4dc1ba97"]
        r restore redis_string 0 $dumpval
        assert_equal foo [r get redis_string]
    }

    test {restore redis dump payload for hash listpack} {
        r del redis_hash
        set dumpval [binary format H* "101717000000040082663103827631038266320382763203ff0a0003f84c0c15d4a2bc"]
        r restore redis_hash 0 $dumpval
        assert_equal {f1 f2} [lsort [r hkeys redis_hash]]
        assert_equal {v1 v2} [lsort [r hvals redis_hash]]
    }

    test {restore redis dump payload for list quicklist2} {
        r del redis_list
        set dumpval [binary format H* "12010210100000000300816102816202816302ff0a00c818b90c9533529e"]
        r restore redis_list 0 $dumpval
        assert_equal {a b c} [r lrange redis_list 0 -1]
    }

    test {restore redis dump payload for zset listpack} {
        r del redis_zset
        set dumpval [binary format H* "111111000000040081610201018162020201ff0a00ba07f224a9d28f55"]
        r restore redis_zset 0 $dumpval
        assert_equal {a b} [r zrange redis_zset 0 -1]
        assert_equal 1 [r zscore redis_zset a]
        assert_equal 2 [r zscore redis_zset b]
    }

    test {restore redis dump payload for list ziplist version 4} {
        r del redis_list_v4
        set dumpval [binary format H* "0a171700000012000000030000c0010004c0020004c00300ff040075233cc03b2ee9dd"]
        r restore redis_list_v4 0 $dumpval
        assert_equal {1 2 3} [r lrange redis_list_v4 0 -1]
    }
}
