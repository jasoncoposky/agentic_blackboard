#!/bin/bash
set -e

# If config file is missing in mounted volume, copy default config from distribution backup
if [ ! -f /etc/agentic-blackboard/blackboard.conf ]; then
    cp -r /etc/agentic-blackboard.dist/. /etc/agentic-blackboard/ 2>/dev/null || true
fi

# If bootstrap admin token or database is missing in mounted data volume, initialize
if [ ! -f /var/lib/agentic-blackboard/admin.token ] && [ ! -f /var/lib/agentic-blackboard/credentials.db ]; then
    /usr/bin/ab-ctl init --bootstrap --data-dir=/var/lib/agentic-blackboard
    chmod 0644 /var/lib/agentic-blackboard/admin.token 2>/dev/null || true
fi

# If the first argument is an option (starts with -) or empty, default to executing agentic-blackboardd
if [ $# -eq 0 ]; then
    exec /usr/bin/agentic-blackboardd --config=/etc/agentic-blackboard/blackboard.conf
elif [ "${1:0:1}" = '-' ]; then
    exec /usr/bin/agentic-blackboardd "$@"
else
    exec "$@"
fi
