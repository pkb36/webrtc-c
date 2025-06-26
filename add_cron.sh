#!/bin/bash

function create_cron()			#this function does not allow duplication
{
	echo "Add CRON entry without duplication"
	new_entry=$1
	if ! crontab -l | fgrep -q "$new_entry"; then
		#write out current crontab
		crontab -l > allcrons
		#echo new cron into cron file
		echo "$new_entry" >> allcrons
		#install new cron file
		crontab allcrons
		rm allcrons
	fi
}

function create_cron2()			#this function allow duplication
{
	echo "Add CRON entry"
	new_entry=$1
	#write out current crontab
	crontab -l > allcrons
	#echo new cron into cron file
	echo "$new_entry" >> allcrons
	#install new cron file
	crontab allcrons
	rm allcrons
}

sleep 30

crontab -r
create_cron  "# indicating with different fields when the task will be run"
create_cron  "# and what command to run for the task"
create_cron2 "#"
create_cron  "# To define the time you can provide concrete values for"
create_cron  "# minute (m), hour (h), day of month (dom), month (mon),"
create_cron  "# and day of week (dow) or use '*' in these fields (for 'any')."
create_cron2 "#"
create_cron  "# Notice that tasks will be started based on the cron's system"
create_cron  "# daemon's notion of time and timezones."
create_cron2 "#"
create_cron  "# Output of the crontab jobs (including errors) is sent through"
create_cron  "# email to the user the crontab file belongs to (unless redirected)."
create_cron2 "#"
create_cron  "# For example, you can run a backup of all your user accounts"
create_cron  "# at 5 a.m every week with:"
create_cron  "# 0 5 * * 1 tar -zcf /var/backups/home.tgz /home/"
create_cron2 "#"
create_cron  "# For more information see the manual pages of crontab(5) and cron(8)"
create_cron2 "#"
create_cron  "# m h  dom mon dow   command"

create_cron  "@reboot /home/nvidia/webrtc/start.sh"
create_cron  "0 3 * * * /home/nvidia/webrtc/disk_check.sh"
