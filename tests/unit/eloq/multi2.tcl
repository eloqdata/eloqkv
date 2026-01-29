start_server {tags {"multi-string"}} {
    
    test {string case 1: wrap write with multi/exec} {
        # Write Part
        r del mykey mykey2 mykey3 mykey4 mykey5 mykey6
        r multi
        r set mykey "hello"
        r append mykey " world"
        r setrange mykey 6 "Redis"
        r setbit mykey 0 1
        r setbit mykey 7 1
        r set mykey2 "10"
        r incr mykey2
        r incrby mykey2 5
        r decr mykey2
        r decrby mykey2 3
        r getset mykey2 "100"
        r incrbyfloat mykey2 1.5
        r mset mykey "new_hello" mykey3 "value3"
        r msetnx mykey4 "value4" mykey5 "value5"
        r exec

        # Read Part
        set v1 [r get mykey]
        set v2 [r getrange mykey 6 10]
        set v3 [r strlen mykey]
        set v4 [r getbit mykey 0]
        set v5 [r getbit mykey 7]
        set v6 [r bitcount mykey]
        set v7 [r bitpos mykey 1]
        set v8 [r get mykey2]
        set v9 [r mget mykey mykey3 mykey4 mykey5]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9
    } {new_hello llo 9 0 0 42 1 101.5 {new_hello value3 value4 value5}}
    
    test {string case 2: wrap read only} {
        # Write Part
        r del mykey mykey2 mykey3 mykey4 mykey5 mykey6
        r set mykey "hello"
        r append mykey " world"
        r setrange mykey 6 "Redis"
        r setbit mykey 0 1
        r setbit mykey 7 1
        r set mykey2 "10"
        r incr mykey2
        r incrby mykey2 5
        r decr mykey2
        r decrby mykey2 3
        r getset mykey2 "100"
        r incrbyfloat mykey2 1.5
        r mset mykey "new_hello" mykey3 "value3"
        r msetnx mykey4 "value4" mykey5 "value5"

        # Read Part
        r multi
        set v1 [r get mykey]
        set v2 [r getrange mykey 6 10]
        set v3 [r strlen mykey]
        set v4 [r getbit mykey 0]
        set v5 [r getbit mykey 7]
        set v6 [r bitcount mykey]
        set v7 [r bitpos mykey 1]
        set v8 [r get mykey2]
        set v9 [r mget mykey mykey3 mykey4 mykey5]
        set v10 [r exec]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {new_hello llo 9 0 0 42 1 101.5 {new_hello value3 value4 value5}}}
    
    test {string case 3: wrap both write and read} {
        # Write Part
        r del mykey mykey2 mykey3 mykey4 mykey5 mykey6
        r multi
        r set mykey "hello"
        r append mykey " world"
        r setrange mykey 6 "Redis"
        r setbit mykey 0 1
        r setbit mykey 7 1
        r set mykey2 "10"
        r incr mykey2
        r incrby mykey2 5
        r decr mykey2
        r decrby mykey2 3
        r getset mykey2 "100"
        r incrbyfloat mykey2 1.5
        r mset mykey "new_hello" mykey3 "value3"
        r msetnx mykey4 "value4" mykey5 "value5"
        r exec

        # Read Part
        r multi
        set v1 [r get mykey]
        set v2 [r getrange mykey 6 10]
        set v3 [r strlen mykey]
        set v4 [r getbit mykey 0]
        set v5 [r getbit mykey 7]
        set v6 [r bitcount mykey]
        set v7 [r bitpos mykey 1]
        set v8 [r get mykey2]
        set v9 [r mget mykey mykey3 mykey4 mykey5]
        set v10 [r exec]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {new_hello llo 9 0 0 42 1 101.5 {new_hello value3 value4 value5}}}
}

start_server {tags {"multi-list"}} {
    
    test {list case 1: wrap write with multi/exec} {
        # Write Part
        r del mylist mylist2
        r multi
        r lpush mylist a
        r lpush mylist b
        r lpush mylist c
        r rpush mylist d
        r lpushx mylist2 e
        r rpushx mylist2 f
        r linsert mylist BEFORE b x
        r lset mylist 1 y
        r lrem mylist 1 a
        r ltrim mylist 1 -1
        r rpoplpush mylist mylist2
        r exec

        # Read Part
        set v1 [r lindex mylist 0]
        set v2 [r llen mylist]
        set v3 [r lrange mylist 0 -1]
        set v4 [r lpop mylist]
        set v5 [r rpop mylist]
        set v6 [r lpos mylist x]
        set v7 [r llen mylist2]
        set v8 [r lrange mylist2 0 -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8
    } {y 2 {y b} y b {} 1 d}
    
    test {list case 2: wrap read only} {
        # Write Part
        r del mylist mylist2
        r lpush mylist a
        r lpush mylist b
        r lpush mylist c
        r rpush mylist d
        r lpushx mylist2 e
        r rpushx mylist2 f
        r linsert mylist BEFORE b x
        r lset mylist 1 y
        r lrem mylist 1 a
        r ltrim mylist 1 -1
        r rpoplpush mylist mylist2

        # Read Part
        r multi
        set v1 [r lindex mylist 0]
        set v2 [r llen mylist]
        set v3 [r lrange mylist 0 -1]
        set v4 [r lpop mylist]
        set v5 [r rpop mylist]
        set v6 [r lpos mylist x]
        set v7 [r llen mylist2]
        set v8 [r lrange mylist2 0 -1]
        set v9 [r exec]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {y 2 {y b} y b {} 1 d}}
    
    test {list case 3: wrap both write and read} {
        # Write Part
        r del mylist mylist2
        r multi
        r lpush mylist a
        r lpush mylist b
        r lpush mylist c
        r rpush mylist d
        r lpushx mylist2 e
        r rpushx mylist2 f
        r linsert mylist BEFORE b x
        r lset mylist 1 y
        r lrem mylist 1 a
        r ltrim mylist 1 -1
        r rpoplpush mylist mylist2
        r exec

        # Read Part
        r multi
        set v1 [r lindex mylist 0]
        set v2 [r llen mylist]
        set v3 [r lrange mylist 0 -1]
        set v4 [r lpop mylist]
        set v5 [r rpop mylist]
        set v6 [r lpos mylist x]
        set v7 [r llen mylist2]
        set v8 [r lrange mylist2 0 -1]
        set v9 [r exec]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {y 2 {y b} y b {} 1 d}}
}

proc list_equal_unordered {list1 list2 {step 1}} {
    if {[llength $list1] != [llength $list2]} {
        return 0
    }
    if {[llength $list1] % $step != 0} {
        return 0
    }
    set sublists1 {}
    set sublists2 {}
    for {set i 0} {$i < [llength $list1]} {incr i $step} {
        lappend sublists1 [lrange $list1 $i [expr {$i + $step - 1}]]
        lappend sublists2 [lrange $list2 $i [expr {$i + $step - 1}]]
    }
    return [expr {[lsort $sublists1] == [lsort $sublists2]}]
}


start_server {tags {"multi-set"}} {

    test {set case 1: wrap write with multi/exec} {
		r flushdb
        # Write Part
        r multi
        r sadd myset a b c d
        r sadd myset2 c d e f
        r sadd myset3 g h
        r srem myset a
        r sdiffstore resultset1 myset myset2
        r sinterstore resultset2 myset myset2
        r sunionstore resultset3 myset myset2
        r smove myset myset2 b
        r spop myset
        r exec

        # Read Part
        set v1 [r scard myset]
        set v2 [r sdiff myset myset2]
        set v3 [r sinter myset myset2]
        set v4 [r sintercard 2 myset myset2]
        set v5 [r sismember myset c]
        set v6 [r smembers myset]
        set v7 [r smismember myset c d e]
        set v8 [r srandmember myset]
        set v9 [r scard resultset1]
        set v10 [r scard resultset2]
        set v11 [r scard resultset3]
        set v12 [r sunion myset myset2]
		set v12 [lsort $v12]
		set ans [list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12]
		set pass [expr {
				  [list_equal_unordered $ans [list 1 {} d 1 0 d {0 1 0} d 1 2 5 [lsort [list b c d e f]]]] ||
				  [list_equal_unordered $ans [list 1 {} c 1 1 c {1 0 0} c 1 2 5 [lsort [list b c d e f]]]]}]
		list $pass
    } {1}

    test {set case 2: wrap read only} {
		r flushdb
        # Write Part
        r sadd myset a b c d
        r sadd myset2 c d e f
        r sadd myset3 g h
        r srem myset a
        r sdiffstore resultset1 myset myset2
        r sinterstore resultset2 myset myset2
        r sunionstore resultset3 myset myset2
        r smove myset myset2 b
        r spop myset

        # Read Part
        r multi
        set v1 [r scard myset]
        set v2 [r sdiff myset myset2]
        set v3 [r sinter myset myset2]
        set v4 [r sintercard 2 myset myset2]
        set v5 [r sismember myset c]
        set v6 [r smembers myset]
        set v7 [r smismember myset c d e]
        set v8 [r srandmember myset]
        set v9 [r scard resultset1]
        set v10 [r scard resultset2]
        set v11 [r scard resultset3]
        set v12 [r sunion myset myset2]
        set v13 [r exec]
		lset v13 11 [lsort [lindex $v13 11]]
		set pass [expr {
				  [list_equal_unordered $ans [list 1 {} d 1 0 d {0 1 0} d 1 2 5 [lsort [list b c d e f]]]] ||
				  [list_equal_unordered $ans [list 1 {} c 1 1 c {1 0 0} c 1 2 5 [lsort [list b c d e f]]]]}]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12 $pass
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED 1}

    test {set case 3: wrap both write and read} {
		r flushdb
        # Write Part
        r multi
        r sadd myset a b c d
        r sadd myset2 c d e f
        r sadd myset3 g h
        r srem myset a
        r sdiffstore resultset1 myset myset2
        r sinterstore resultset2 myset myset2
        r sunionstore resultset3 myset myset2
        r smove myset myset2 b
        r spop myset
        r exec

        # Read Part
        r multi
        set v1 [r scard myset]
        set v2 [r sdiff myset myset2]
        set v3 [r sinter myset myset2]
        set v4 [r sintercard 2 myset myset2]
        set v5 [r sismember myset c]
        set v6 [r smembers myset]
        set v7 [r smismember myset c d e]
        set v8 [r srandmember myset]
        set v9 [r scard resultset1]
        set v10 [r scard resultset2]
        set v11 [r scard resultset3]
        set v12 [r sunion myset myset2]
        set v13 [r exec]
		lset v13 11 [lsort [lindex $v13 11]]
		set pass [expr {
				  [list_equal_unordered $ans [list 1 {} d 1 0 d {0 1 0} d 1 2 5 [lsort [list b c d e f]]]] ||
				  [list_equal_unordered $ans [list 1 {} c 1 1 c {1 0 0} c 1 2 5 [lsort [list b c d e f]]]]}]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12 $pass
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED 1}
}

start_server {tags {"multi-hash"}} {
    
    test {hash case 1: wrap write with multi/exec} {
        # Write Part
        r del myhash
        r multi
        r hset myhash field1 "value1"
        r hset myhash field2 "value2"
        r hset myhash field3 5
        r hdel myhash field1
        r hexists myhash field2
        r hget myhash field2
        r hincrby myhash field3 5
        r hincrbyfloat myhash field3 1.5
        r hsetnx myhash field4 "value4"
        r hmset myhash field5 "value5" field6 "value6"
        r exec

        # Read Part
        set v1 [r hlen myhash]
        set v2 [r hget myhash field2]
        set v3 [r hget myhash field3]
        set v4 [expr {[list_equal_unordered [r hkeys myhash] [list field2 field3 field4 field5 field6]]}]
        set v5 [expr {[list_equal_unordered [r hvals myhash] [list value2 11.5 value4 value5 value6]]}]
        set v6 [expr {[list_equal_unordered [r hgetall myhash] [list field2 value2 field3 11.5 field4 value4 field5 value5 field6 value6]]}]
        set v7 [r hexists myhash field1]
        set v8 [r hmget myhash field2 field3 field4]
        set v9 [r hstrlen myhash field2]
		set all [r hkeys myhash]
        set v10 [r hrandfield myhash]
		set v10 [expr {[lsearch -exact $all $v10]} != -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10
    } {5 value2 11.5 1 1 1 0 {value2 11.5 value4} 6 1}
    
    test {hash case 2: wrap read only} {
        # Write Part
        r del myhash
        r hset myhash field1 "value1"
        r hset myhash field2 "value2"
        r hset myhash field3 5
        r hdel myhash field1
        r hexists myhash field2
        r hget myhash field2
        r hincrby myhash field3 5
        r hincrbyfloat myhash field3 1.5
        r hsetnx myhash field4 "value4"
        r hmset myhash field5 "value5" field6 "value6"

        # Read Part
        r multi
        set v1 [r hlen myhash]
        set v2 [r hget myhash field2]
        set v3 [r hget myhash field3]
        set v4 [r hkeys myhash]
        set v5 [r hvals myhash]
        set v6 [r hgetall myhash]
        set v7 [r hexists myhash field1]
        set v8 [r hmget myhash field2 field3 field4]
        set v9 [r hstrlen myhash field2]
        set v10 [r hrandfield myhash]
        set v11 [r exec]
        lset v11 3 [expr {[list_equal_unordered [lindex $v11 3] [list field2 field3 field4 field5 field6]]}]
        lset v11 4 [expr {[list_equal_unordered [lindex $v11 4] [list value2 11.5 value4 value5 value6]]}]
        lset v11 5 [expr {[list_equal_unordered [lindex $v11 5] [list field2 value2 field3 11.5 field4 value4 field5 value5 field6 value6]]}]
		set all [r hkeys myhash]
		lset v11 9 [expr {[lsearch -exact $all [lindex $v11 9]]} != -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {5 value2 11.5 1 1 1 0 {value2 11.5 value4} 6 1}}
    
    test {hash case 3: wrap both write and read} {
        # Write Part
        r del myhash
        r multi
        r hset myhash field1 "value1"
        r hset myhash field2 "value2"
        r hset myhash field3 5
        r hdel myhash field1
        r hexists myhash field2
        r hget myhash field2
        r hincrby myhash field3 5
        r hincrbyfloat myhash field3 1.5
        r hsetnx myhash field4 "value4"
        r hmset myhash field5 "value5" field6 "value6"
        r exec

        # Read Part
        r multi
        set v1 [r hlen myhash]
        set v2 [r hget myhash field2]
        set v3 [r hget myhash field3]
        set v4 [r hkeys myhash]
        set v5 [r hvals myhash]
        set v6 [r hgetall myhash]
        set v7 [r hexists myhash field1]
        set v8 [r hmget myhash field2 field3 field4]
        set v9 [r hstrlen myhash field2]
        set v10 [r hrandfield myhash]
        set v11 [r exec]
        lset v11 3 [expr {[list_equal_unordered [lindex $v11 3] [list field2 field3 field4 field5 field6]]}]
        lset v11 4 [expr {[list_equal_unordered [lindex $v11 4] [list value2 11.5 value4 value5 value6]]}]
        lset v11 5 [expr {[list_equal_unordered [lindex $v11 5] [list field2 value2 field3 11.5 field4 value4 field5 value5 field6 value6]]}]
		set all [r hkeys myhash]
		lset v11 9 [expr {[lsearch -exact $all [lindex $v11 9]]} != -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {5 value2 11.5 1 1 1 0 {value2 11.5 value4} 6 1}}
}

start_server {tags {"multi-zset"}} {
    
    test {zset case 1: wrap write with multi/exec} {
        # Write Part
        r del myzset myzset2 myzset3 resultzset interzset diffzset storezset
        r multi
        r zadd myzset -2 "a" -1 "b" 3 "c" 8 "g" -10 "z" 7 "k" 14.2 "l" 13.8 "j"
        r zadd myzset2 3 "c" 4 "d" 5 "e"
        r zadd myzset3 1 "a" 5 "e" 7 "f"
        r zincrby myzset 2 "a"
        r zrem myzset "b"
        r zremrangebyscore myzset 3 5
        r zremrangebyrank myzset 0 0
        r zremrangebylex myzset \[c \[e
        r zunionstore resultzset 2 myzset myzset2
        r zinterstore interzset 2 myzset myzset2
        r zdiffstore diffzset 2 myzset myzset2
        r exec

        # Read Part
        set v1 [r zcard myzset]
        set v2 [r zcount myzset 0 5]
        set v3 [r zrange myzset 0 -1 withscores]
        set v4 [r zrank myzset "a"]
        set v5 [r zscore myzset "a"]
        set v6 [r zmscore myzset "a" "b"]
        set v7 [r zrangebyscore myzset 0 5]
        set v8 [r zrevrange myzset 0 -1 withscores]
        set v9 [r zunion 2 myzset myzset2]
        set v10 [r zinter 2 myzset myzset2]
        set v11 [r zdiff 2 myzset myzset2]
        set v12 [r zintercard 2 myzset myzset2]
        set v13 [r zlexcount myzset "-" "+"]
        set v14 [expr {[lsearch -exact [r zrange myzset 0 -1] [r zrandmember myzset]]} != -1]
        set v15 [r zmpop 2 myzset myzset2 min]
        set v16 [r zpopmax myzset]
        set v17 [r zpopmin myzset]
        set v18 [r zrangebylex myzset "-" "+"]
        set v19 [r zrangestore storezset myzset 0 -1]
        set v20 [r zrevrangebylex myzset "+" "-"]
        set v21 [r zrevrangebyscore myzset 5 0]
        set v22 [r zrevrank myzset "a"]
        set v23 [r zscan myzset 0]
        set v24 [r zcard resultzset]
        set v25 [r zcard interzset]
        set v26 [r zcard diffzset]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12 $v13 $v14 $v15 $v16 $v17 $v18 $v19 $v20 $v21 $v22 $v23 $v24 $v25 $v26
    } {5 1 {a 0 k 7 g 8 j 13.8 l 14.2} 0 0 {0 {}} a {l 14.2 j 13.8 g 8 k 7 a 0} {a c d e k g j l} {} {a k g j l} 0 5 1 {myzset {{a 0}}} {l 14.2} {k 7} {g j} 2 {j g} {} {} {0 {g 8 j 13.8}} 8 0 5}
    
    test {zset case 2: wrap read only} {
        # Write Part
        r del myzset myzset2 myzset3 resultzset interzset diffzset storezset
        r zadd myzset -2 "a" -1 "b" 3 "c" 8 "g" -10 "z" 7 "k" 14.2 "l" 13.8 "j"
        r zadd myzset2 3 "c" 4 "d" 5 "e"
        r zadd myzset3 1 "a" 5 "e" 7 "f"
        r zincrby myzset 2 "a"
        r zrem myzset "b"
        r zremrangebyscore myzset 3 5
        r zremrangebyrank myzset 0 0
        r zremrangebylex myzset \[c \[e
        r zunionstore resultzset 2 myzset myzset2
        r zinterstore interzset 2 myzset myzset2
        r zdiffstore diffzset 2 myzset myzset2

		set all [r zrange myzset 0 -1]

        # Read Part
        r multi
        set v1 [r zcard myzset]
        set v2 [r zcount myzset 0 5]
        set v3 [r zrange myzset 0 -1 withscores]
        set v4 [r zrank myzset "a"]
        set v5 [r zscore myzset "a"]
        set v6 [r zmscore myzset "a" "b"]
        set v7 [r zrangebyscore myzset 0 5]
        set v8 [r zrevrange myzset 0 -1 withscores]
        set v9 [r zunion 2 myzset myzset2]
        set v10 [r zinter 2 myzset myzset2]
        set v11 [r zdiff 2 myzset myzset2]
        set v12 [r zintercard 2 myzset myzset2]
        set v13 [r zlexcount myzset "-" "+"]
        set v14 [r zrandmember myzset]
        set v15 [r zmpop 2 myzset myzset2 min]
        set v16 [r zpopmax myzset]
        set v17 [r zpopmin myzset]
        set v18 [r zrangebylex myzset "-" "+"]
        set v19 [r zrangestore storezset myzset 0 -1]
        set v20 [r zrevrangebylex myzset "+" "-"]
        set v21 [r zrevrangebyscore myzset 5 0]
        set v22 [r zrevrank myzset "a"]
        set v23 [r zscan myzset 0]
        set v24 [r zcard resultzset]
        set v25 [r zcard interzset]
        set v26 [r zcard diffzset]
        set v27 [r exec]
        lset v27 13 [expr {[lsearch -exact $all [lindex $v27 13]]} != -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12 $v13 $v14 $v15 $v16 $v17 $v18 $v19 $v20 $v21 $v22 $v23 $v24 $v25 $v26 $v27
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {5 1 {a 0 k 7 g 8 j 13.8 l 14.2} 0 0 {0 {}} a {l 14.2 j 13.8 g 8 k 7 a 0} {a c d e k g j l} {} {a k g j l} 0 5 1 {myzset {{a 0}}} {l 14.2} {k 7} {g j} 2 {j g} {} {} {0 {g 8 j 13.8}} 8 0 5}}
    
    test {zset case 3: wrap both write and read} {
        # Write Part
        r del myzset myzset2 myzset3 resultzset interzset diffzset storezset
        r del myzset myzset2 myzset3 resultzset interzset diffzset storezset
        r multi
        r zadd myzset -2 "a" -1 "b" 3 "c" 8 "g" -10 "z" 7 "k" 14.2 "l" 13.8 "j"
        r zadd myzset2 3 "c" 4 "d" 5 "e"
        r zadd myzset3 1 "a" 5 "e" 7 "f"
        r zincrby myzset 2 "a"
        r zrem myzset "b"
        r zremrangebyscore myzset 3 5
        r zremrangebyrank myzset 0 0
        r zremrangebylex myzset \[c \[e
        r zunionstore resultzset 2 myzset myzset2
        r zinterstore interzset 2 myzset myzset2
        r zdiffstore diffzset 2 myzset myzset2
        r exec

		set all [r zrange myzset 0 -1]

        # Read Part
        r multi
        set v1 [r zcard myzset]
        set v2 [r zcount myzset 0 5]
        set v3 [r zrange myzset 0 -1 withscores]
        set v4 [r zrank myzset "a"]
        set v5 [r zscore myzset "a"]
        set v6 [r zmscore myzset "a" "b"]
        set v7 [r zrangebyscore myzset 0 5]
        set v8 [r zrevrange myzset 0 -1 withscores]
        set v9 [r zunion 2 myzset myzset2]
        set v10 [r zinter 2 myzset myzset2]
        set v11 [r zdiff 2 myzset myzset2]
        set v12 [r zintercard 2 myzset myzset2]
        set v13 [r zlexcount myzset "-" "+"]
        set v14 [r zrandmember myzset]
        set v15 [r zmpop 2 myzset myzset2 min]
        set v16 [r zpopmax myzset]
        set v17 [r zpopmin myzset]
        set v18 [r zrangebylex myzset "-" "+"]
        set v19 [r zrangestore storezset myzset 0 -1]
        set v20 [r zrevrangebylex myzset "+" "-"]
        set v21 [r zrevrangebyscore myzset 5 0]
        set v22 [r zrevrank myzset "a"]
        set v23 [r zscan myzset 0]
        set v24 [r zcard resultzset]
        set v25 [r zcard interzset]
        set v26 [r zcard diffzset]
        set v27 [r exec]
        lset v27 13 [expr {[lsearch -exact $all [lindex $v27 13]]} != -1]
        list $v1 $v2 $v3 $v4 $v5 $v6 $v7 $v8 $v9 $v10 $v11 $v12 $v13 $v14 $v15 $v16 $v17 $v18 $v19 $v20 $v21 $v22 $v23 $v24 $v25 $v26 $v27
    } {QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED QUEUED {5 1 {a 0 k 7 g 8 j 13.8 l 14.2} 0 0 {0 {}} a {l 14.2 j 13.8 g 8 k 7 a 0} {a c d e k g j l} {} {a k g j l} 0 5 1 {myzset {{a 0}}} {l 14.2} {k 7} {g j} 2 {j g} {} {} {0 {g 8 j 13.8}} 8 0 5}}
}

proc sort_by_key {mylist} {
    for {set i 0} {$i < [llength $mylist] - 2} {incr i 2} {
        for {set j 0} {$j < [llength $mylist] - 2 - $i} {incr j 2} {
            if {[lindex $mylist $j] > [lindex $mylist [expr {$j + 2}]]} {
                set tmp1 [lindex $mylist $j]
                set tmp2 [lindex $mylist [expr {$j + 1}]]
                lset mylist $j [lindex $mylist [expr {$j + 2}]]
                lset mylist [expr {$j + 1}] [lindex $mylist [expr {$j + 3}]]
                lset mylist [expr {$j + 2}] $tmp1
                lset mylist [expr {$j + 3}] $tmp2
            }
        }
    }
    return $mylist
}

start_server {tags {"dump-restore"}} {

    test {dump-restore case 1: dump and restore multiple string keys} {
        # Part 1: Use MULTI to execute dump
        r del mykey1 mykey2 mykey3
        r multi
        r set mykey1 "Hello"
        r set mykey2 "World"
        r set mykey3 "!"
        set dump_cmds [list [r dump mykey1] [r dump mykey2] [r dump mykey3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del mykey1 mykey2 mykey3

        # Part 2: Use MULTI to execute restore
        r multi
        r restore mykey1 0 $dumped_value1
        r restore mykey2 0 $dumped_value2
        r restore mykey3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r get mykey1]
        set v2 [r get mykey2]
        set v3 [r get mykey3]
        set part2_result [list $v1 $v2 $v3]

        # Part 3: Use two MULTI to execute dump and restore separately
        r del mykey1 mykey2 mykey3
        r multi
        r set mykey1 "Hello"
        r set mykey2 "World"
        r set mykey3 "!"
        set dump_cmds [list [r dump mykey1] [r dump mykey2] [r dump mykey3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del mykey1 mykey2 mykey3

        r multi
        r restore mykey1 0 $dumped_value1
        r restore mykey2 0 $dumped_value2
        r restore mykey3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r get mykey1]
        set v2 [r get mykey2]
        set v3 [r get mykey3]
        set part3_result [list $v1 $v2 $v3]

        list $part2_result $part3_result
    } {{Hello World !} {Hello World !}}

    test {dump-restore case 2: dump and restore multiple hash keys} {
        # Part 1: Use MULTI to execute dump
        r del myhash1 myhash2 myhash3
        r multi
        r hmset myhash1 field1 "value1" field2 "value2"
        r hmset myhash2 field1 "foo" field2 "bar"
        r hmset myhash3 field1 "123" field2 "456"
        set dump_cmds [list [r dump myhash1] [r dump myhash2] [r dump myhash3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myhash1 myhash2 myhash3

        # Part 2: Use MULTI to execute restore
        r multi
        r restore myhash1 0 $dumped_value1
        r restore myhash2 0 $dumped_value2
        r restore myhash3 0 $dumped_value3
        r exec

        # Verify
        set v1 [sort_by_key [r hgetall myhash1]]
        set v2 [sort_by_key [r hgetall myhash2]]
        set v3 [sort_by_key [r hgetall myhash3]]
        set part2_result [list $v1 $v2 $v3]

        # Part 3: Use two MULTI to execute dump and restore separately
        r del myhash1 myhash2 myhash3
        r multi
        r hmset myhash1 field1 "value1" field2 "value2"
        r hmset myhash2 field1 "foo" field2 "bar"
        r hmset myhash3 field1 "123" field2 "456"
        set dump_cmds [list [r dump myhash1] [r dump myhash2] [r dump myhash3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myhash1 myhash2 myhash3

        r multi
        r restore myhash1 0 $dumped_value1
        r restore myhash2 0 $dumped_value2
        r restore myhash3 0 $dumped_value3
        r exec

        # Verify
        set v1 [sort_by_key [r hgetall myhash1]]
        set v2 [sort_by_key [r hgetall myhash2]]
        set v3 [sort_by_key [r hgetall myhash3]]
        set part3_result [list $v1 $v2 $v3]

        list $part2_result $part3_result
    } {{{field1 value1 field2 value2} {field1 foo field2 bar} {field1 123 field2 456}} {{field1 value1 field2 value2} {field1 foo field2 bar} {field1 123 field2 456}}}

    test {dump-restore case 3: dump and restore multiple zset keys} {
        # Part 1: Use MULTI to execute dump
        r del myzset1 myzset2 myzset3
        r multi
        r zadd myzset1 1 "a" 2 "b"
        r zadd myzset2 2 "b" 3 "c"
        r zadd myzset3 3 "c" 4 "d"
        set dump_cmds [list [r dump myzset1] [r dump myzset2] [r dump myzset3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myzset1 myzset2 myzset3

        # Part 2: Use MULTI to execute restore
        r multi
        r restore myzset1 0 $dumped_value1
        r restore myzset2 0 $dumped_value2
        r restore myzset3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r zrange myzset1 0 -1 withscores]
        set v2 [r zrange myzset2 0 -1 withscores]
        set v3 [r zrange myzset3 0 -1 withscores]
        set part2_result [list $v1 $v2 $v3]

        # Part 3: Use two MULTI to execute dump and restore separately
        r del myzset1 myzset2 myzset3
        r multi
        r zadd myzset1 1 "a" 2 "b"
        r zadd myzset2 2 "b" 3 "c"
        r zadd myzset3 3 "c" 4 "d"
        set dump_cmds [list [r dump myzset1] [r dump myzset2] [r dump myzset3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myzset1 myzset2 myzset3

        r multi
        r restore myzset1 0 $dumped_value1
        r restore myzset2 0 $dumped_value2
        r restore myzset3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r zrange myzset1 0 -1 withscores]
        set v2 [r zrange myzset2 0 -1 withscores]
        set v3 [r zrange myzset3 0 -1 withscores]
        set part3_result [list $v1 $v2 $v3]

        list $part2_result $part3_result
    } {{{a 1 b 2} {b 2 c 3} {c 3 d 4}} {{a 1 b 2} {b 2 c 3} {c 3 d 4}}}

    test {dump-restore case 4: dump and restore multiple set keys} {
        # Part 1: Use MULTI to execute dump
        r del myset1 myset2 myset3
        r multi
        r sadd myset1 "one" "two"
        r sadd myset2 "three" "four"
        r sadd myset3 "five" "six"
        set dump_cmds [list [r dump myset1] [r dump myset2] [r dump myset3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myset1 myset2 myset3

        # Part 2: Use MULTI to execute restore
        r multi
        r restore myset1 0 $dumped_value1
        r restore myset2 0 $dumped_value2
        r restore myset3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r smembers myset1]
        set v2 [r smembers myset2]
        set v3 [r smembers myset3]
        set part2_result [list [lsort $v1] [lsort $v2] [lsort $v3]]

        # Part 3: Use two MULTI to execute dump and restore separately
        r del myset1 myset2 myset3
        r multi
        r sadd myset1 "one" "two"
        r sadd myset2 "three" "four"
        r sadd myset3 "five" "six"
        set dump_cmds [list [r dump myset1] [r dump myset2] [r dump myset3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del myset1 myset2 myset3

        r multi
        r restore myset1 0 $dumped_value1
        r restore myset2 0 $dumped_value2
        r restore myset3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r smembers myset1]
        set v2 [r smembers myset2]
        set v3 [r smembers myset3]
        set part3_result [list [lsort $v1] [lsort $v2] [lsort $v3]]

        list $part2_result $part3_result
    } {{{one two} {four three} {five six}} {{one two} {four three} {five six}}}

    test {dump-restore case 5: dump and restore multiple list keys} {
        # Part 1: Use MULTI to execute dump
        r del mylist1 mylist2 mylist3
        r multi
        r rpush mylist1 "a" "b"
        r rpush mylist2 "c" "d"
        r rpush mylist3 "e" "f"
        set dump_cmds [list [r dump mylist1] [r dump mylist2] [r dump mylist3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del mylist1 mylist2 mylist3

        # Part 2: Use MULTI to execute restore
        r multi
        r restore mylist1 0 $dumped_value1
        r restore mylist2 0 $dumped_value2
        r restore mylist3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r lrange mylist1 0 -1]
        set v2 [r lrange mylist2 0 -1]
        set v3 [r lrange mylist3 0 -1]
        set part2_result [list $v1 $v2 $v3]

        # Part 3: Use two MULTI to execute dump and restore separately
        r del mylist1 mylist2 mylist3
        r multi
        r rpush mylist1 "a" "b"
        r rpush mylist2 "c" "d"
        r rpush mylist3 "e" "f"
        set dump_cmds [list [r dump mylist1] [r dump mylist2] [r dump mylist3]]
        set dump_results [r exec]

        # Extract dumped values
        set dumped_value1 [lindex $dump_results 3]
        set dumped_value2 [lindex $dump_results 4]
        set dumped_value3 [lindex $dump_results 5]
        r del mylist1 mylist2 mylist3

        r multi
        r restore mylist1 0 $dumped_value1
        r restore mylist2 0 $dumped_value2
        r restore mylist3 0 $dumped_value3
        r exec

        # Verify
        set v1 [r lrange mylist1 0 -1]
        set v2 [r lrange mylist2 0 -1]
        set v3 [r lrange mylist3 0 -1]
        set part3_result [list $v1 $v2 $v3]

        list $part2_result $part3_result
    } {{{a b} {c d} {e f}} {{a b} {c d} {e f}}}

    test {hsetnx fail case} {
        r del myset1
        r hsetnx myset1 foo bar
        r dump myset1
    }
}

