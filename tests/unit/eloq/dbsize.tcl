start_server {tags {"dbsize"}} {
    test {Empty, Added 1000 keys, Deleted 500 keys} {
        r flushdb
        assert_equal 0 [r dbsize]

        for {set i 0} {$i < 1000} {incr i} {
            r set key_$i val_$i
        }

        assert_equal 1000 [r dbsize]

        for {set i 0} {$i < 1000} {incr i 2} {
            r del key_$i
        }

        assert_equal 500 [r dbsize]
    } {} {needs:no_evicted}
}
