#!/bin/bash

IMAGE_ID=ohie:1.0
N=3
PORT=8000
FILE="_peers_docker"

./kill_docker_containers.sh
#./build_docker_image.sh

for i in $(seq 1 $N);
do
    IP=172.17.0.$((1 + $i))
    echo "Node $i with file $FILE, ip $IP and port $PORT"
    nohup docker run $IMAGE_ID $PORT $FILE $IP > outputnode$i.txt &
done
