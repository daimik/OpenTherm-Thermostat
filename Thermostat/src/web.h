// Read-only status page of the display, over the Wi-Fi it already uses.
//
// Built in only when secrets.h defines WEB_PASS; without it both functions do nothing and no server runs.
// The page shows; it does not control. No request changes the heating, the schedule or the settings. The server runs in its own task on the other processor core and reads a copy of the
// state; the main loop never waits for it.
#pragma once
void web_init();      // setup(), after Wi-Fi is configured
void web_publish();   // main task, once a second: hand over a copy of the state and take a chart sample
