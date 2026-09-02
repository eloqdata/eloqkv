start_server {tags {"maxclients network"}} {
    test {CONFIG GET and SET expose the live maxclients limit} {
        set original [lindex [r config get maxclients] 1]

        assert_equal {OK} [r config set maxclients 1]
        assert_equal {1} [lindex [r config get maxclients] 1]

        # The connection issuing CONFIG SET remains established when the new
        # limit is below the current connection count.
        assert_equal {PONG} [r ping]

        if {$::tls} {
            set expected_rejection {*I/O error*}
        } else {
            set expected_rejection {*ERR max*reached*}
        }
        set rejected [catch {redis_deferring_client} rejection]
        assert_equal {1} $rejected
        assert_match $expected_rejection $rejection

        assert_error {*argument must be between 1 and 4294967295 inclusive*} {
            r config set maxclients 0
        }
        assert_error {*argument couldn't be parsed into an integer*} {
            r config set maxclients invalid
        }
        assert_equal {1} [lindex [r config get maxclients] 1]

        r config set maxclients $original
    }
}
