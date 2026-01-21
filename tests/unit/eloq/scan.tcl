start_server {tags {"scan network"}} {
    test "SCAN basic" {
        r flushdb
        populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN COUNT" {
        r flushdb
        populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur count 5]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            lappend hashs $cur
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN MATCH" {
        r flushdb
        populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur match "key:1??"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 100 [llength $keys]
    }

    test "SCAN TYPE" {
        r flushdb
        # populate only creates strings
        populate 1000

        # Check non-strings are excluded
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "list"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }
        
        assert_equal 0 [llength $keys]

        # Check strings are included
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]

        # Check all three args work together
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string" match "key:*" count 9]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]
    }

    foreach enc {intset listpack hashtable} {
        test "SSCAN with encoding $enc" {
            # Create the Set
            r del set
            if {$enc eq {intset}} {
                set prefix ""
            } else {
                set prefix "ele:"
            }
            set count [expr {$enc eq "hashtable" ? 200 : 100}]
            set elements {}
            for {set j 0} {$j < $count} {incr j} {
                lappend elements ${prefix}${j}
            }
            r sadd set {*}$elements

            # Verify that the encoding matches.
            assert_encoding $enc set

            # Test SSCAN
            set cur 0
            set keys {}
            while 1 {
                set res [r sscan set $cur]
                set cur [lindex $res 0]
                set k [lindex $res 1]
                lappend keys {*}$k
                if {$cur == 0} break
            }

            set keys [lsort -unique $keys]
            assert_equal $count [llength $keys]
        }
    }

    foreach enc {listpack hashtable} {
        test "HSCAN with encoding $enc" {
            # Create the Hash
            r del hash
            if {$enc eq {listpack}} {
                set count 30
            } else {
                set count 1000
            }
            set elements {}
            for {set j 0} {$j < $count} {incr j} {
                lappend elements key:$j $j
            }
            r hmset hash {*}$elements

            # Verify that the encoding matches.
            assert_encoding $enc hash

            # Test HSCAN
            set cur 0
            set keys {}
            while 1 {
                set res [r hscan hash $cur]
                set cur [lindex $res 0]
                set k [lindex $res 1]
                lappend keys {*}$k
                if {$cur == 0} break
            }

            set keys2 {}
            foreach {k v} $keys {
                assert {$k eq "key:$v"}
                lappend keys2 $k
            }

            set keys2 [lsort -unique $keys2]
            assert_equal $count [llength $keys2]
        }
    }

    test "SCAN guarantees check under write load" {
        r flushdb
        populate 100

        # We start scanning here, so keys from 0 to 99 should all be
        # reported at the end of the iteration.
        set keys {}
        while 1 {
            set res [r scan $cur count 5]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
            # Write 10 random keys at every SCAN iteration.
            for {set j 0} {$j < 10} {incr j} {
                r set key:[randomInt 1000]added foo
            }
        }

        set keys2 {}
        foreach k $keys {
            if {[string length $k] > 6} continue
            lappend keys2 $k
        }

        set keys2 [lsort -unique $keys2]
        assert_equal 100 [llength $keys2]
    }

test "del existing key during MULTI/EXEC does not crash" {
    r flushdb
    r set a a
    r multi
    r del a
    r keys *
    r exec
}

test "MULTI/EXEC with various commands and checking KEYS * count" {
    r flushdb

    # Scenario 1: MULTI with SET and KEYS *
    r multi
    r set a b
    r keys *
    set result [r exec]

    # Check the number of keys returned by KEYS * (should be 1)
    set keys_in_multi_count [llength [lindex $result 1]] ; # The result of KEYS * is at index 1
    assert_equal 1 $keys_in_multi_count

    # Clean up database
    r flushdb

    # Scenario 2: MULTI with DEL, SET and KEYS *
    r set a b
    r multi
    r del a
    r set a b
    r keys *
    set result [r exec]

    # Check the number of keys returned by KEYS * (should be 1)
    set keys_in_multi_count [llength [lindex $result 2]] ; # The result of KEYS * is at index 2
    assert_equal 1 $keys_in_multi_count

    # Clean up database
    r flushdb

    # Scenario 3: MULTI with SET, INCR and KEYS *
    r set counter 0
    r multi
    r set a b
    r incr counter
    r keys *
    set result [r exec]

    # Check the number of keys returned by KEYS * (should be 2)
    set keys_in_multi_count [llength [lindex $result 2]] ; # The result of KEYS * is at index 2
    assert_equal 2 $keys_in_multi_count

    # Clean up database
    r flushdb

    # Scenario 4: MULTI with DEL, SET, INCR and KEYS *
    r set a b
    r set counter 0
    r multi
    r del a
    r set a b
    r incr counter
    r keys *
    set result [r exec]

    # Check the number of keys returned by KEYS * (should be 2)
    set keys_in_multi_count [llength [lindex $result 3]] ; # The result of KEYS * is at index 3
    assert_equal 2 $keys_in_multi_count
}

test "MULTI/EXEC with multiple KEYS * and DEL commands" {
    r flushdb

    # Scenario 1: MULTI with multiple KEYS *, SET, DEL, and INCR

    r multi
    # First KEYS * should return no keys
    r keys *

    # Set a new key 'a'
    r set a b
    # Second KEYS * should return key 'a'
    r keys *

    # Increment the counter
    r incr counter
    # Third KEYS * should return 'a' and 'counter'
    r keys *

    # Delete the key 'a'
    r del a
    # Fourth KEYS * should return only 'counter'
    r keys *

    # Final EXEC
    set result [r exec]

    # Check results for each KEYS * command
    set keys_first [llength [lindex $result 0]]   ; # First KEYS * (should return 0 keys)
    set keys_second [llength [lindex $result 2]]  ; # Second KEYS * (should return 1 key 'a')
    set keys_third [llength [lindex $result 4]]   ; # Third KEYS * (should return 2 keys 'a' and 'counter')
    set keys_fourth [llength [lindex $result 6]]  ; # Fourth KEYS * (should return 1 key 'counter')

    # Assertions
    assert_equal 0 $keys_first       ; # No keys in the first KEYS *
    assert_equal 1 $keys_second      ; # One key after SET 'a'
    assert_equal 2 $keys_third       ; # Two keys after INCR counter
    assert_equal 1 $keys_fourth      ; # One key after DEL 'a'

    # Clean up database
    r flushdb

    # Scenario 2: MULTI with multiple DEL, SET, INCR, and KEYS *
    r set a b
    r set counter 0

    r multi
    # First KEYS * should return 2 keys ('a' and 'counter')
    r keys *

    # Delete the key 'a'
    r del a
    # Second KEYS * should return only 'counter'
    r keys *

    # MSet key 'b' 'c'
    r mset b b c c
    # Third KEYS * should return 'b', 'c' and 'counter'
    r keys *

    # Increment the counter
    r incr counter
    # Fourth KEYS * should return 'b', 'c' and 'counter'
    r keys *

    # Final EXEC
    set result [r exec]

    # Check results for each KEYS * command
    set keys_first [llength [lindex $result 0]]   ; # First KEYS * (should return 2 keys 'a' and 'counter')
    set keys_second [llength [lindex $result 2]]  ; # Second KEYS * (should return 1 key 'counter')
    set keys_third [llength [lindex $result 4]]   ; # Third KEYS * (should return 3 keys 'b', 'c' and 'counter')
    set keys_fourth [llength [lindex $result 6]]  ; # Fourth KEYS * (should return 3 keys 'b', 'c' and 'counter')

    # Assertions
    assert_equal 2 $keys_first       ; # Two keys ('a' and 'counter') initially
    assert_equal 1 $keys_second      ; # One key ('counter') after DEL 'a'
    assert_equal 3 $keys_third       ; # Three keys after SET 'b' and 'c'
    assert_equal 3 $keys_fourth      ; # Three keys after INCR 'counter'

    # Final check: ensure that the database contains only 'b', 'c' and 'counter'
    assert_equal 3 [r dbsize]
}

}
