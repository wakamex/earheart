// Brings the app forward so a window it just showed has the keyboard. Needed on macOS, where an
// app without a Dock icon is not made active by showing a window; nothing elsewhere.
#pragma once
void eh_app_activate(void);
