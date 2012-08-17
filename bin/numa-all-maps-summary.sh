#!/bin/bash

for i in `ls /proc/[0-9]*/numa_maps`
do
    if [ -e $i ]
    then
        echo $i
        sudo ./numa-maps-summary.pl < $i; 
        echo ================================================================================
    fi
done
