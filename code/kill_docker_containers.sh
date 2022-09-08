#!/bin/bash

IMAGE_ID=ohie:1.0

docker kill $(docker ps -q --filter ancestor=$IMAGE_ID) 2>/dev/null
