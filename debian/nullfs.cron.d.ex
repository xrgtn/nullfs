#
# Regular cron jobs for the nullfs package
#
0 4	* * *	root	[ -x /usr/bin/nullfs_maintenance ] && /usr/bin/nullfs_maintenance
