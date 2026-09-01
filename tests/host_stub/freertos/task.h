// #region MODULE_CONTRACT
// PURPOSE: Supplies the minimal FreeRTOS task-handle type required by host-only tests.
// SCOPE: Task handle declarations only; NOT task scheduling or lifecycle emulation.
// #endregion MODULE_CONTRACT

#pragma once

using TaskHandle_t = void*;
