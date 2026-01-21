start_server {tags {"introspection"}} {
    test {COMMAND COUNT get total number of Redis commands} {
        assert_morethan [r command count] 0
    }

    test {COMMAND INFO of invalid subcommands} {
        assert_equal {{}} [r command info get|key]
        assert_equal {{}} [r command info config|get|key]
    }

    foreach cmd {SET GET MSET BITFIELD LMOVE LPOP BLPOP PING MEMORY MEMORY|USAGE RENAME GEORADIUS_RO} {
        test "$cmd command will not be marked with movablekeys" {
            set info [lindex [r command info $cmd] 0]
            assert_no_match {*movablekeys*} [lindex $info 2]
        }
    }

#    foreach cmd {ZUNIONSTORE XREAD EVAL SORT SORT_RO MIGRATE GEORADIUS} {
#        test "$cmd command is marked with movablekeys" {
#            set info [lindex [r command info $cmd] 0]
#            assert_match {*movablekeys*} [lindex $info 2]
#        }
#    }
}