// #region MODULE_CONTRACT
// PURPOSE: Preserves one validated configuration include during store splits.
// SCOPE:
// - Re-exports network, SMTP, ZTE and modem-source stores and their runtime
//   profiles.
// - NOT: NVS access itself, network connections, protocol dialogs and HTTP
//   handling — each split module owns its domain.
// INVARIANTS:
// - Including this header exposes the same split-store declarations
//   without defining persistence or protocol behavior.
// DEPRECATED: Prefer including the split headers (config_store_network.h,
// config_store_smtp.h, config_store_zte.h, config_store_modem.h) directly;
// this aggregator is kept only for backward compatibility and may be removed.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef PERSISTENCE_CONFIG_STORE_H
#define PERSISTENCE_CONFIG_STORE_H

#include "persistence/config_store_common.h"
#include "persistence/config_store_gps.h"
#include "persistence/config_store_modem.h"
#include "persistence/config_store_network.h"
#include "persistence/config_store_smtp.h"
#include "persistence/config_store_zte.h"
#endif  // PERSISTENCE_CONFIG_STORE_H
