start_server {tags {"pubsub network"}} {
    test "PUBLISH with no subscribers returns 0" {
        assert_equal 0 [r publish nosubchan hello]
    }

    test "PUBLISH/SUBSCRIBE basics (single subscriber)" {
        set rd1 [redis_deferring_client]
        assert_equal {1} [subscribe $rd1 {smokechan}]
        assert_equal 1 [r publish smokechan hello]
        assert_equal {message smokechan hello} [$rd1 read]

        unsubscribe $rd1 {smokechan}
        assert_equal 0 [r publish smokechan hello]

        $rd1 close
    }

    test "PUBLISH/PSUBSCRIBE basics (single pattern subscriber)" {
        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 {smoke.*}]
        assert_equal 1 [r publish smoke.news hello]
        assert_equal {pmessage smoke.* smoke.news hello} [$rd1 read]

        punsubscribe $rd1 {smoke.*}
        assert_equal 0 [r publish smoke.news hello]

        $rd1 close
    }

    test "PUBLISH from Lua script counts subscribers" {
        assert_equal 0 [r eval {return redis.call('publish', ARGV[1], ARGV[2])} 0 luasmoke hello]

        set rd1 [redis_deferring_client]
        assert_equal {1} [subscribe $rd1 {luasmoke}]
        assert_equal 1 [r eval {return redis.call('publish', ARGV[1], ARGV[2])} 0 luasmoke hello]
        assert_equal {message luasmoke hello} [$rd1 read]

        $rd1 close
    }
}
