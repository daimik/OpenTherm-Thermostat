// Thread-safe bridge between the main task (owner of st) and the other tasks.
#pragma once
#include "state.h"

enum CmdType : uint8_t { CMD_SET_TARGET = 1, CMD_SET_MODE = 2, CMD_SET_OUTSIDE = 3 };

void shared_init();
void shared_publish();                       // main task, 1 Hz: copy st into the snapshot
void shared_snapshot(ThermoState *out);      // any task: consistent copy of the state
bool shared_post(CmdType type, float value); // any task: ask the main task to change something
void shared_apply_commands();                // main task: drain the queue into st

extern volatile bool g_mqttConnected;        // written by the MQTT task, read by the main task
