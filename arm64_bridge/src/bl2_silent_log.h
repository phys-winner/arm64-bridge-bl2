#pragma once

// Final builds (HORN_SILENT_LOGCAT): from JNI_OnLoad on, the process drops log
// messages tagged BL2* or Arm32Bridge* (the bridge, the port's Java and the
// original game's Java). Needs Android 11 (__android_log_set_logger); older
// systems keep the Java messages, while the bridge itself is silent anywhere.
void Bl2SilenceProcessLogs();
