#!/bin/sh

### BEGIN INIT INFO
# Provides:          aesdsocket
# Required-Start:    $network $local_fs
# Required-Stop:     $network $local_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start/stop the aesdsocket daemon
### END INIT INFO

### Configuration
DAEMON_PATH="$(dirname "$(readlink -f "$0")")"
DAEMON="/usr/bin/aesdsocket"
DAEMON_OPTS="-d"
PIDFILE="/var/run/aesdsocket.pid"
LOGFILE="/var/log/aesdsocket.log"

case "$1" in
    start)
        echo "Starting aesdsocket daemon..."
        if [ ! -x "$DAEMON" ]; then
            echo "ERROR: $DAEMON not found or not executable"
            exit 1
        fi
        start-stop-daemon --start --background --make-pidfile --pidfile "$PIDFILE" --exec "$DAEMON" -- $DAEMON_OPTS >> "$LOGFILE" 2>&1
        echo "Started."
        ;;
    stop)
        echo "Stopping aesdsocket daemon..."
        start-stop-daemon --stop --pidfile "$PIDFILE" --retry 5
        echo "Stopped."
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    status)
        if [ -f "$PIDFILE" ] && kill -0 $(cat "$PIDFILE") 2>/dev/null; then
            echo "aesdsocket is running (PID: $(cat "$PIDFILE"))"
            exit 0
        else
            echo "aesdsocket is not running"
            exit 1
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac

exit 0
