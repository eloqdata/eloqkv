# A freshly opened connection can already answer its own commands before the
# brpc Acceptor has registered its socket in the connection map that CLIENT
# LIST and CLIENT KILL enumerate. Under load (e.g. a 3-node cluster sharing
# core_number=2) that registration lag is observable: CLIENT KILL <addr> scans
# the map, finds no match and replies "No such client", failing the test.
# CLIENT KILL only accepts the old-style addr:port form, and it and CLIENT LIST
# read the exact same map, so wait until the target address is actually visible
# in CLIENT LIST before killing it. That makes the kill deterministic without
# reducing coverage.
proc kill_client_by_addr {conn addr} {
    wait_for_condition 50 20 {
        [string match "*addr=$addr *" [$conn client list]]
    } else {
        fail "client $addr never registered in CLIENT LIST"
    }
    $conn client kill $addr
}

start_server {tags {"client"}} {
   test "CLIENT SETNAME/GETNAME" {
      r client setname test-client
      # assert_match {*test-client*} [r client info]
      r client getname
   } {test-client}

   test "CLIENT LIST AND CLIENT KILL" {
      set c [redis_client]
      $c client setname "kill-client"
      assert_match {*kill-client*} [$c client getname]
      set c_info [$c client info]
      regexp addr=(127.0.0.1:\[0-9\]+) $c_info - addr
      kill_client_by_addr r $addr
      set client_list [r client list]
      regexp name=(kill-client) $client_list - name
      $c close
   } {0}

   test "CLIENT ID, CLIENT INFO, CLIENT LIST, CLIENT KILL" {
      set rr [redis_client]
      set rr2 [redis_client]
      set id1 [r client id]
      set id2 [$rr client id]
      set id3 [$rr2 client id]
      assert_not_equal $id1 $id2
      assert_not_equal $id1 $id3
      assert_not_equal $id2 $id3

      set info [r client info]
      regexp {id\=(\d+)} $info _ reg_id
      assert_equal $id1 $reg_id
      
      regexp {laddr\=([^\s]+)} $info _ laddr
      assert_equal "127.0.0.1:6379" $laddr
      
      assert_equal 1 [regexp $id1 [r client info]]
      assert_equal 1 [regexp $id2 [$rr client info]]
      assert_equal 1 [regexp $id3 [$rr2 client info]]

      regexp {addr\=([^\s]+)} [$rr client info]] _ addr2
      kill_client_by_addr r $addr2
      assert_equal 0 [regexp {$id2} [r client list]]

      regexp {addr\=([^\s]+)} [$rr2 client info]] _ addr3
      kill_client_by_addr r $addr3
      assert_equal 0 [regexp {$id3} [r client list]]
   }

   test "CLIENT SETNAME, CLIENT GETNAME" {
      set rr [redis_client]
      assert_equal 0 [regexp {name\=([^\s]+)} [$rr client info] name]
      $rr client setname test-name
      assert_equal "test-name" [ $rr client getname]

      # Now does not support client list(info) to show client name
      # assert_equal 1 [regexp {name\=([^\s]+)} [$rr client info] _ name]
      # assert_equal "test-name" $name
      # assert_equal 1 [regexp {name\=test-name} [r client list]]

      r client setname "name-test"
      # assert_equal 1 [regexp {name\=name-test} [$rr client list]]

      $rr close
   }
}
