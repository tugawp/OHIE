#!/bin/bash

# kill all nodes
fuser -k Node



if [ -z "$1" ]
then
    N=3
else
    N=$1
fi

FROM=8001
TO=$(($FROM + $N - 1))

rm _peers

echo -n "127.0.0.1:$FROM" >> _peers
for PORT in $(seq $(($FROM + 1)) $TO);
do
    echo -n -e "\n127.0.0.1:$PORT" >> _peers
done

# remove created folders (if any)
rm -rf _Blockchains
rm -rf _Blocks
rm -rf _Hashes
rm -rf _Pings
rm -rf _Sessions
rm -rf _Transactions

# remove previous outputs
rm outputnode*

for i in $(seq 1 $N);
do
    PORT=$(($FROM + $i - 1))
    echo "Node $i with port $PORT"
    nohup ./Node $PORT _peers 127.0.0.1 > outputnode$i.txt &
done
