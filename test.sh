#!/bin/bash

if [ ! -f test.c ]; then
        echo "test.c file not found!"
        exit 1
fi

if gcc test.c -o test; then
        ./test
        rm test
fi
