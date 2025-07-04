start_server {tags {"string"}} {

    test {Very big key in GET/SET} {
        set key [string repeat "a" [expr {32 * 1024 * 1024 + 1}]]
        set val "some value"
        assert_error "*max key size limit of 32 MB exceeded*" {r set $key $val}
        set key [string repeat "a" [expr {32 * 1024 * 1024}]]
        assert_equal "OK" [r set $key $val]
        assert_equal $val [r get $key]
    } {} {needs:no_evicted}

    test {Very big payload in GET/SET} {
        r del "big_key"
        set buf [string repeat "a" 300000000]
        assert_error "*max object size limit of 256 MB exceeded*" {r set "big_key" $buf}
        assert_equal "" [r get "big_key"]
    } {} {needs:no_evicted}

    test {Very big payload in APPEND} {
        r del "big_key"
        set buf2 [string repeat "b" 300000000]

        assert_error "*max object size limit of 256 MB exceeded*" {r append "big_key" $buf2}
        assert_equal "" [r get "big_key"]

        set buf2 [string repeat "a" [expr {256 * 1024 * 1024 - 6}]]

        assert_equal "OK" [r set "big_key" $buf2]

        assert_equal $buf2 [r get "big_key"]

        assert_equal [expr {256 * 1024 * 1024 - 5}] [r append "big_key" "a"]

        assert_error "*max object size limit of 256 MB exceeded*" {r append "big_key" "a"}
    } {} {needs:no_evicted}

    test {SETRANGE with huge offset} {
        set offset [expr {256 * 1024 * 1024}]
        catch {r setrange K $offset A} res
        if {![string match "*ERR string exceeds maximum allowed size*" $res]} {
            assert_equal $res "expecting an error"
        }

        set offset [expr {256 * 1024 * 1024 - 6}]
        assert_equal [expr {256 * 1024 * 1024 - 5}] [r setrange K $offset A]

        set offset [expr {256 * 1024 * 1024 - 5}]
        assert_error "*max object size limit of 256 MB exceeded*" {r setrange K $offset A}
    } {} {needs:no_evicted}

    test {Very big object for RPUSH} {
        r del "l"
        set buf [string repeat "a" 300000000]
        assert_error "*max object size limit of 256 MB exceeded*" {r rpush "l" $buf}
        assert_equal "" [r lrange "l" 0 -1]
    } {} {needs:no_evicted}

    test {Very big object for ZADD} {
        set key "zset_key"
        r del $key

        set buf [string repeat "a" 100000000]
        assert_equal 1 [r zadd $key 10 $buf]

        set buf2 [string repeat "a" 200000000]
        assert_error "*max object size limit of 256 MB exceeded*" {r zadd $key 10 $buf2}

        set buf2 [string repeat "a" [expr {256 * 1024 * 1024 - 5 - 8 - 4 - 100000000 - 8 - 4}]]
        assert_equal 1 [r zadd $key 10 $buf2]

        set buf2 [string repeat "a" [expr {256 * 1024 * 1024 - 5 - 8 - 4 - 100000000 - 8 - 4 + 1}]]
        assert_error "*max object size limit of 256 MB exceeded*" {r zadd $key 10 $buf2}
    } {} {needs:no_evicted}

    test "SADD, SREM, SPOP serialized length" {
        r del myset
        r sadd myset a1234567 b12345678 c123456789
        assert_equal 49 [r memory usage myset]

        r sadd myset q1234567 w12345678 e123456789
        assert_equal 88 [r memory usage myset]
        
        r srem myset a1234567 w12345678 f123456789
        assert_equal 63 [r memory usage myset]
        
        set len [expr 59 - [string length [r spop myset]]]
        assert_equal $len [r memory usage myset]
    } {} {needs:no_evicted}

    test "SET OBJECT exceed serialized length" {
        set big_key [string repeat "a" 300000000]
        r del myset
        r sadd myset a1234567
        assert_equal 22 [r memory usage myset]
        assert_error {ERR max object size limit*} {r sadd myset $big_key}
        assert_equal "a1234567" [r smembers myset]
    } {} {needs:no_evicted}

     test "HASH OBJECT SET DEL HINCRBY HSETNX HINCRBYFLOAT serialized length" {
        r del myhash
        r hset myhash q1 qq1234567890 ww2 www1234567890
        assert_equal 57 [r memory usage myhash]
        
        r hset myhash q1 qq1234 ee3 eee1234567890
        assert_equal 75 [r memory usage myhash]
        
        r hdel myhash ww2
        assert_equal 51 [r memory usage myhash]
        
        r hdel myhash ww2
        assert_equal 51 [r memory usage myhash]
        
        r hincrby myhash ii1 1
        assert_equal 63 [r memory usage myhash]
        
        r hincrby myhash ii1 100
        assert_equal 65 [r memory usage myhash]
        
        r hincrby myhash ii1 -10
        assert_equal 64 [r memory usage myhash]
        
        r hsetnx myhash ll1 ll123456789
        assert_equal 86 [r memory usage myhash]
        
        r hsetnx myhash ll1 ll
        assert_equal 86 [r memory usage myhash]
        
        r hincrbyfloat myhash dd1 0.1
        assert_equal 100 [r memory usage myhash]
        
        r hincrbyfloat myhash dd1 0.00001
        assert_equal 104 [r memory usage myhash]
        
        r hincrbyfloat myhash dd1 -0.09001
        assert_equal 101 [r memory usage myhash]
    } {} {needs:no_evicted}

    test "HASH OBJECT exceed serialized length" {
        set big_val [string repeat "a" 268435432]
        r del myhash
        r hset myhash h1 $big_val
        assert_equal 268435453 [r memory usage myhash]
        assert_error {ERR max object size limit*} {r hset myhash h2 1234567890}
        assert_error {ERR max object size limit*} {r hincrby myhash i2 1234}
        assert_error {ERR max object size limit*} {r hsetnx myhash h3 qqqqqq}
        assert_error {ERR max object size limit*} {r hincrbyfloat myhash f2 0.001}
        assert_equal 268435453 [r memory usage myhash]
        assert_equal 268435435 [string length [r hgetall myhash]]
    } {} {needs:no_evicted}

    r flushdb
}
