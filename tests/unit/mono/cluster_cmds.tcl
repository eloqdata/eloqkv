start_server {tags {"cluster_cmds"}} {
    
    test {KEYSLOT} {
       assert_equal 15495 [ r CLUSTER KEYSLOT "a" ]
       assert_equal 15495 [ r CLUSTER KEYSLOT "{a}b" ]

       assert_equal 10276 [ r CLUSTER KEYSLOT "\{a" ]
       assert_equal 10276 [ r CLUSTER KEYSLOT "\{\{a\}b" ]

       assert_equal 7471 [ r CLUSTER KEYSLOT "\{b\{a\}c\}d" ]
       assert_equal 7471 [ r CLUSTER KEYSLOT "b\{a" ]
    } {} {nees:cluster_mode}

}
