#!/bin/bash

mkdir -p ./output
g++ app.cc -o ./output/app
chmod +x ./output/app

taskset -c 0 ./output/app