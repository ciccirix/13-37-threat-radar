#pragma once

// Task manager: lists the power-hungry subsystems that are currently RUNNING
// (radios, scanners, detectors, portals…) and lets you kill them one-by-one or
// all at once to save battery. Opening it does nothing but read state.
void task_manager_show();
void task_manager_create();
bool task_manager_is_active();
