while true; do ps aux | grep hts_skipper.py | grep /usr/bin/python| awk '{print $5, $12}' | sort -n; sleep 1; done;
