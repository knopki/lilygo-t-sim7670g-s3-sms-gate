// #region MODULE_CONTRACT
// PURPOSE: Provides validated access to the isolated appcfg NVS partition for
// the Wi-Fi, web administrator, SMTP delivery, ZTE and onboard SIM7670G SMS
// source configurations. This header re-exports the split modules for
// backward compatibility so existing consumers keep a single include.
// SCOPE:
// - Re-exports network, SMTP, ZTE and modem-source stores and their runtime
//   profiles.
// - NOT: NVS access itself, network connections, protocol dialogs and HTTP
//   handling — each split module owns its domain.
// DEPRECATED: Prefer including the split headers (config_store_network.h,
// config_store_smtp.h, config_store_zte.h, config_store_modem.h) directly;
// this aggregator is kept only for backward compatibility and may be removed.
// #endregion MODULE_CONTRACT

#pragma once

#include "config_store_common.h"
#include "config_store_modem.h"
#include "config_store_network.h"
#include "config_store_smtp.h"
#include "config_store_zte.h"
