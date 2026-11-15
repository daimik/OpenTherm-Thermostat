#pragma once
#include <stdint.h>
#include "ble_protocol.h"

void ble_link_init();
void ble_link_loop();                       // call from loop(): scan/connect/reconnect, applies received status to st
void ble_link_send_control(const BoilerControl &c);
void ble_link_send_cmd(uint8_t cmd);
void ble_link_forget_node();                // drop the pinned node address (after replacing the boiler node)
