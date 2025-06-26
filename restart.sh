#!/bin/bash
./stop_all.sh
unset DISPLAY
echo nvidia | sudo -S python ./gstream_manage.py
