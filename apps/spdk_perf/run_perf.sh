
#!/bin/bash

# Script to run perf tests
# June 2016

# Parameters:
# 	$1 = output_filename


if [ $# -ne 1 ]
  then
    echo "Usage: ./run_perf.sh [output_filename]"
fi

# Create output file and set permissions for root to write
touch $1
chmod o+w $1

printf "Workload; Read Ratio; Num cores; Max Qdepth; Req Size; Target IOPS; Rd IOPS; Wr IOPS; Rd Avg; Rd p95; Rd p99; Rd p99.9; Rd p99.99; Wr Avg; Wr p95; Wr p99; Wr p99.9; Wr p99.99; Total p99.9; #dropped \n" >> $1


# sweep request sizes
for s in 4096 # 1024 8192 16384 32768 65536
do
	# sweep read/write ratios
	for m in 0 50 100 #100 75 50 25 0 # 99 95 90 85 80 75 70 60 50 40 30 20 10 0 
	do
		# sweep target IOPS
		# note lambda is the target IOPS *per core*
		# total target IOPS is lambda times the num cores, specified via coremask parameter
		for lambda in `seq 20000 20000 700000`
		do
			printf "randrw-openloop-exp; %d; 1; 2122000; %d; %d;" "$m"  "$s" "$lambda" >> $1
			#sudo ./perf -t 120 -s 4096 -q 1024 -w randrw -M $m -c 1 -o $1 -L $lambda 
			# note: -c is the coremask in hex
		    #       -c 1 means use a single core 
		    # 	    -c 3 means use 2 cores
		    #       -c f means use 4 cores	
			#		keep in mind, to achieve high IOPS, may need more than 1 core
			sudo timeout 30 ./perf -t 10 -s $s -q 2122000 -w randrw -M $m -c 0x1 -o $1 -L $lambda 
		done
		printf "\n" >> $1
	done
done
