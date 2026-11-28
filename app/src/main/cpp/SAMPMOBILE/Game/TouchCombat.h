#pragma once

#include <cstdint>

void RequestStuffUse();
bool ConsumeStuffUseRequest();
void RequestPassengerDriveByToggle();
bool ConsumePassengerDriveByToggle();
void ApplyLocalCombatSyncKeys(uint16_t& wKeys, uint16_t& lrAnalog, uint16_t& udAnalog);
void InstallTouchCombatHooks();
