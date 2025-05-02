#!/bin/bash
# Check if a file exists and print its path

if [ -f $1 ]; then
    echo $1
fi
