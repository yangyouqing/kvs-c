#!/usr/bin/env bash
# Deploy TURN (coturn) on public server.
# Usage: export EXTERNAL_IP=1.2.3.4 && ./scripts/deploy-turnserver.sh
# Env: EXTERNAL_IP (required), TURN_PORT=3478, TURN_USER=user, TURN_PASS=pass
set -e
TURN_PORT="${TURN_PORT:-3478}"
TURN_USER="${TURN_USER:-user}"
TURN_PASS="${TURN_PASS:-pass}"
NO_SYSTEMD="${NO_SYSTEMD:-0}"
[ -z "${EXTERNAL_IP}" ] && echo "Set EXTERNAL_IP (e.g. export EXTERNAL_IP=1.2.3.4)" && exit 1
echo "=== Deploy TURN ==="
if ! command -v turnserver &>/dev/null; then
  command -v apt-get &>/dev/null && sudo apt-get update && sudo apt-get install -y coturn || { echo "Install coturn"; exit 1; }
fi
if [ "$NO_SYSTEMD" != "1" ] && [ -d /etc/turnserver.conf.d ]; then
  echo "listening-port=$TURN_PORT" | sudo tee /etc/turnserver.conf.d/port.conf
  echo "external-ip=$EXTERNAL_IP" | sudo tee /etc/turnserver.conf.d/external.conf
  echo "user=$TURN_USER:$TURN_PASS" | sudo tee /etc/turnserver.conf.d/user.conf
  [ -f /etc/default/coturn ] && sudo sed -i 's/#*TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
  sudo systemctl enable coturn 2>/dev/null || true
  sudo systemctl restart coturn 2>/dev/null || true
  echo "coturn running via systemd"; exit 0
fi
exec turnserver --listening-port="$TURN_PORT" --relay-ip="$EXTERNAL_IP" --external-ip="$EXTERNAL_IP" --user="$TURN_USER:$TURN_PASS" --realm=local --no-cli
