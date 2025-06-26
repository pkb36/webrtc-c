#!/bin/bash

DATE=$(date "+%Y-%m-%d")
echo "tail -f ./logs/${DATE}.log"
tail -f ./logs/${DATE}.log

