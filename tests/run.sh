#!/bin/sh
# Host tests for the parts that are pure logic. Needs g++ and libmbedtls-dev (Linux or WSL):
#   sudo apt install g++ libmbedtls-dev && sh tests/run.sh
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -I../BoilerNode/include test_node.cpp  -o /tmp/test_node  && /tmp/test_node
g++ -std=c++17 -I../Thermostat/include test_auth.cpp  -o /tmp/test_auth -lmbedcrypto && /tmp/test_auth
g++ -std=c++17 -I../Thermostat/include test_logic.cpp -o /tmp/test_logic && /tmp/test_logic
cmp ../Thermostat/include/ble_protocol.h ../BoilerNode/include/ble_protocol.h
cmp ../Thermostat/include/ble_auth.h ../BoilerNode/include/ble_auth.h
echo "shared protocol headers identical in both projects"
