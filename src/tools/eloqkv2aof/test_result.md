
## Test with 2GB Rocksdb Data (aof 30GB)
key-count:14997962
(same count of "string" "hash" "set" "list" "zset" objects)

T1.1- One thread, only Scan items in Rocksdb. ("ssd" disk)

    Used time: 14sec

T1.2- One thread, Scan items in Rocksdb and Parse items to TxObject. ("ssd" disk)

    Used time: 1min

T1.3- One thread, Scan items in Rocksdb, Parse items to TxObject and Generate aof cmds. ("ssd" disk)

    Used time: 7min38sec

T1.4- One thread, Scan items in Rocksdb, Parse items to TxObject,Generate aof cmds and write to file.
("ssd" disk)

    Used time: 10min30sec

T2.1- One thread scan items in Rocksdb, 4 thread Parse items and write to file. ("ssd" disk)

    Used time: 2min40sec

T2.2- One thread scan items in Rocksdb, 10 thread Parse items and write to file.("ssd" disk)

    Used time: 1min18sec

T2.3- One thread scan items in Rocksdb, 20 thread Parse items and write to file.("ssd" disk)

    Used time: 38sec

T2.4- One thread scan items in Rocksdb, 20 thread Parse items and write to file. ("hdd" Disk)

    Used time: 2min57sec

T2.5- One thread scan items in Rocksdb, 10 thread Parse items and write to file.("hdd" Disk)

    Used time: 2min25sec


## Test with 13GB Rocksdb Data (aof 188GB)


T3.1- One thread scan items in Rocksdb, 20 thread Parse items and not write to file.("hdd" Disk)

    Used time: 3min40sec 

T3.2- One thread scan items in Rocksdb, 20 thread Parse items and write to file.("hdd" Disk)

    Used time: 18min40sec

## test disk write use io benchmark tools

T4.1- use `dd`. 
```
// run cmd on  ("hdd" disk)
 dd if=/dev/zero of=/mnt/disk_sda/output.out bs=1000k count=100000

// result
记录了100000+0 的读入
记录了100000+0 的写出
102400000000字节（102 GB，95 GiB）已复制，579.608 s，177 MB/s
```

```
// run cmd on  ("ssd" disk)
 dd if=/dev/zero of=~/output.out bs=1000k count=100000
记录了100000+0 的读入
记录了100000+0 的写出
102400000000字节（102 GB，95 GiB）已复制，201.49 s，508 MB/s
```



T4.2- use my test tool `src/tools/test_io_write.cpp`

```
 ./test_io_write --output_file_dir=/mnt/disk_sda/lzx_data/output4 --thread_count=20

```

Used time: 17min20sec  ("hdd" disk)


```
./test_io_write --output_file_dir=/mnt/disk_sda/lzx_data/output5 --thread_count=20 --write_block_size=1000000 --write_block_count=9400
I20241025 17:06:44.519734 1336674 test_io_write.cpp:123] Output file directory path:/mnt/disk_sda/lzx_data/output5
I20241025 17:06:44.519810 1336674 test_io_write.cpp:124] Parse thread count:20
I20241025 17:06:44.519825 1336674 test_io_write.cpp:125] Write block size:1000000
I20241025 17:06:44.519834 1336674 test_io_write.cpp:126] Write block count:9400
I20241025 17:06:44.519847 1336674 test_io_write.cpp:127] ====Begin====
I20241025 17:23:58.352327 1336681 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:58.612283 1336693 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:58.800384 1336694 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:59.024174 1336691 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:59.076081 1336684 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:59.388615 1336688 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:23:59.892405 1336676 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.100577 1336689 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.148265 1336690 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.388100 1336678 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.532356 1336679 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.584378 1336677 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:00.812306 1336680 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:02.276046 1336695 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:02.576001 1336685 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:02.620334 1336682 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:03.048290 1336692 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:03.080137 1336687 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:03.648242 1336686 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:04.492280 1336683 test_io_write.cpp:73] Thread terminated, write count:9400
I20241025 17:24:04.492944 1336674 test_io_write.cpp:143] ====Finished====
```