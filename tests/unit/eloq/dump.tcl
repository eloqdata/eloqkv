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
}
