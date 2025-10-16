package main

import (
	"crypto/rand"
	"flag"
	"fmt"
	"log"
	"math/big"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/redis/go-redis/v9"
	"golang.org/x/net/context"
)

var (
	server    = flag.String("server", "127.0.0.1", "Redis server address")
	port      = flag.Int("port", 6379, "Redis server port")
	dataSize  = flag.Int("data_size", 512, "Value size in bytes")
	totalKeys = flag.Int("total_keys", 0, "Total number of keys to insert")
	startKey  = flag.Int("start_key", 1, "Starting key index")
	threads   = flag.Int("threads", 1, "Number of worker threads (GOMAXPROCS)")
	conns     = flag.Int("conns", 1, "Total number of parallel connections/jobs")
	randomVal = flag.Bool("random", false, "Generate random value data for each request")
)

var uploaded int64

func main() {
	flag.Parse()
	if *totalKeys == 0 {
		log.Fatal("Must provide -total_keys")
	}
	runtime.GOMAXPROCS(*threads)

	fmt.Printf("Launching %d connections for keys %d ~ %d (stride mode)\n", *conns, *startKey, *totalKeys)

	done := make(chan struct{})
	go func() {
		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				count := atomic.LoadInt64(&uploaded)
				percent := float64(count) * 1000.0 / float64(*totalKeys-*startKey+1)
				fmt.Printf("Progress: %d / %d (≈ %.1f‰)\n",
					count, *totalKeys-*startKey+1, percent)
			case <-done:
				return
			}
		}
	}()

	var wg sync.WaitGroup
	for i := 0; i < *conns; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			doJob(id)
		}(i)
	}
	wg.Wait()

	close(done)
	fmt.Println("All jobs finished")
}

func doJob(id int) {
	ctx := context.Background()
	addr := fmt.Sprintf("%s:%d", *server, *port)
	rdb := redis.NewClient(&redis.Options{Addr: addr})

	value := make([]byte, *dataSize)
	if !*randomVal {
		for i := range value {
			value[i] = 'x'
		}
	}

	fmt.Printf("Job %d: inserting keys kv_%d, kv_%d, ... (stride %d)\n",
		id, *startKey+id, *startKey+id+*conns, *conns)

	for k := *startKey + id; k <= *totalKeys; k += *conns {
		if *randomVal {
			generateRandom(value)
		}

		key := "kv_" + strconv.Itoa(k)
		for {
			err := rdb.Set(ctx, key, value, 0).Err()
			if err != nil {
				log.Printf("Job %d: failed to set %s, retrying in 30s: %v", id, key, err)
				time.Sleep(30 * time.Second)
				continue
			}
			atomic.AddInt64(&uploaded, 1)
			break
		}
	}
}

func generateRandom(buf []byte) {
	const letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	for i := range buf {
		nBig, _ := rand.Int(rand.Reader, big.NewInt(int64(len(letters))))
		buf[i] = letters[nBig.Int64()]
	}
}
