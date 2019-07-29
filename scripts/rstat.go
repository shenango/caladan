package main

import (
	"net"
	"os"
	"time"
	"strconv"
	"fmt"
	"strings"
	"encoding/binary"
	"io"
)

func prettyPrint(m, lastm map[string]uint64, interval int) {
	// compute the actual interval in seconds
	measured_interval := (float64(m["tsc"] - lastm["tsc"])) / (float64(m["cycles_per_us"] * 1000 * 1000))

	dm := make(map[string]float64)
	for i, v := range m {
		dm[i] = float64(v - lastm[i]) / measured_interval
	}
	fmt.Printf("net: RX %.1f pkts, %.1f bytes | TX %.1f pkts, %.1f bytes | %.1f drops | %.2f%% rx out of order (%.2f%% reorder time)\n",
		   dm["rx_packets"], dm["rx_bytes"],
		   dm["tx_packets"], dm["tx_bytes"], dm["drops"],
		   dm["rx_tcp_out_of_order"] / (dm["rx_tcp_in_order"] + dm["rx_tcp_out_of_order"]) * 100,
		   dm["rx_tcp_text_cycles"] / (dm["sched_cycles"] + dm["program_cycles"]) * 100)
	fmt.Printf("sched: %.1f rescheds (%.1f%% sched time, %.1f%% local)," +
		   " %.1f softirqs (%.1f%% stolen), %.1f %%CPU, %.1f parks" +
		   " (%.1f%% migrated), %.1f preempts (%.1f stolen)," +
		   " thread_run_locality %.1f%%, thread_wake_locality %.1f%%, %.1f program_cycles, %.1f sched_cycles," +
		   " %.2f interval (s)\n",
		   dm["reschedules"],
		   dm["sched_cycles"] / (dm["sched_cycles"] + dm["program_cycles"]) * 100,
		   (1 - dm["threads_stolen"] / dm["reschedules"]) * 100,
		   dm["softirqs_local"] + dm["softirqs_stolen"],
		   (dm["softirqs_stolen"] / (dm["softirqs_local"] +
		    dm["softirqs_stolen"])) * 100,
		   (dm["sched_cycles"] + dm["program_cycles"]) * 100 /
		    (float64(m["cycles_per_us"]) * 1000000), dm["parks"],
		   dm["core_migrations"] * 100 / dm["parks"],
		   dm["preemptions"], dm["preemptions_stolen"],
		   (dm["local_runs"] / (dm["local_runs"] + dm["remote_runs"])) * 100,
		   (dm["local_wakes"] / (dm["local_wakes"] + dm["remote_wakes"])) * 100,
		   dm["program_cycles"], dm["sched_cycles"], measured_interval)
	fmt.Println("raw map:", dm)
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage:%s [host] [interval]", os.Args[0])
		os.Exit(1)
	}

	host := os.Args[1]
	uaddr, err := net.ResolveTCPAddr("tcp4", host + ":40")
	if err != nil {
		os.Exit(1)
	}

	c, err := net.DialTCP("tcp", nil, uaddr)
	if err != nil {
		os.Exit(1)
	}

	interval, err := strconv.Atoi(os.Args[2])
	if err != nil {
		os.Exit(1)
	}

	var buf [1500]byte
	lastm := make(map[string]uint64)

	for {
		last_write := time.Now()
		_, err = c.Write([]byte("stat"))
		if err != nil {
			os.Exit(1)
		}
		n, err := io.ReadFull(c, buf[0:8])
		if err != nil {
			os.Exit(1)
		}
		datalen := binary.LittleEndian.Uint64(buf[0:8])
		n, err = io.ReadFull(c, buf[0:datalen])
		if err != nil {
			os.Exit(1)
		}
		strs := strings.Split(string(buf[0:n-1]), ",")
		m := make(map[string]uint64)

		for _, v := range strs {

			fields := strings.Split(v, ":")
			if len(fields) != 2 {
				fmt.Println("can't parse", v)
				os.Exit(1)
			}

			value, err := strconv.ParseUint(fields[1], 10, 64)
			if err != nil {
				fmt.Println("can't parse uint64", fields[1])
				os.Exit(1)
			}

			m[fields[0]] = value
		}

		if len(lastm) > 0 {
			prettyPrint(m, lastm, interval)
		}
		lastm = m

		time.Sleep(time.Duration(interval) * time.Second - time.Since(last_write))
	}
}
