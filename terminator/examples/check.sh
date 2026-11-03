#!/bin/bash
# The script from the project slides, plus a nested if and exit status.
# Usage: ./check.sh myprog.c [run]
if [ -f "$1" ]; then
    echo "File exists, compiling..."
    gcc "$1" -o myprog
    if [ $? -eq 0 ]; then
        echo "Compiled $1 into ./myprog"
        if [ "$2" = "run" ]; then
            ./myprog
            echo "myprog exited with $?"
        fi
    else
        echo "gcc failed"
        exit 2
    fi
else
    echo "Source file missing!"
    exit 1
fi
echo "$# argument(s) given"
