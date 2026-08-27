/**
 * #region moduleContract
 * @purpose Admin page logic: administrator password change plus the
 *   watchdog status block with its safe-mode exit action.
 * @scope /admin only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import {
	apiFetch,
	fillFields,
	poll,
	postForm,
	setBanner,
	setBusy,
	submitSettingsForm,
} from "/js/main.js";

const passwordForm = document.getElementById("password-form");
const clearButton = document.getElementById("watchdog-clear");

function formatDuration(ms) {
	if (!Number.isFinite(ms) || ms < 0) {
		return "—";
	}
	const totalSeconds = Math.floor(ms / 1000);
	const days = Math.floor(totalSeconds / 86400);
	const hours = Math.floor((totalSeconds % 86400) / 3600);
	const minutes = Math.floor((totalSeconds % 3600) / 60);
	const seconds = totalSeconds % 60;
	const hhmmss = `${String(hours).padStart(2, "0")}:${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
	return days > 0 ? `${days} d ${hhmmss}` : hhmmss;
}

// Mirrors esp_reset_reason_t (esp_system.h, Arduino-ESP32 3.3.11), 0-based.
const RESET_REASONS = [
	"Unknown",
	"Power-on reset",
	"External pin reset",
	"Software restart",
	"Exception/panic reset",
	"Interrupt watchdog reset",
	"Task watchdog reset",
	"Watchdog reset",
	"Deep sleep wake",
	"Brownout reset",
	"SDIO reset",
	"USB reset",
	"JTAG reset",
	"eFuse error reset",
	"Power glitch reset",
	"CPU lockup reset",
];

function describeResetReason(code) {
	if (!Number.isFinite(code)) {
		return "—";
	}
	const reason = RESET_REASONS[code];
	return reason ? `${reason} (${code})` : `Unknown (${code})`;
}

async function loadWatchdog() {
	const { response, payload } = await apiFetch("/api/watchdog");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(document.getElementById("watchdog-status"), {
		safe_mode: payload.safe_mode ? "active" : "inactive",
		boot_count: String(payload.boot_count ?? "—"),
		timeout: `${payload.timeout_sec} s`,
		last_reset_reason: describeResetReason(payload.last_reset_reason),
		uptime: formatDuration(payload.uptime_ms),
	});
	clearButton.hidden = payload.safe_mode !== true;
}

passwordForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = passwordForm.elements;
	submitSettingsForm("/api/password", {
		current_password: elements.current_password.value,
		new_password: elements.new_password.value,
		new_password_confirm: elements.new_password_confirm.value,
	}).then((accepted) => {
		if (accepted) {
			passwordForm.reset();
		}
	});
});

clearButton.addEventListener("click", async () => {
	setBusy(true);
	setBanner("", "");
	try {
		const { response, payload } = await postForm("/api/watchdog/clear", {});
		if (response.status === 401) {
			return;
		}
		if (response.ok && payload?.ok) {
			setBanner("ok", payload.message || "Safe mode cleared.");
		} else {
			setBanner("error", payload?.error || "The device rejected the request.");
		}
		await loadWatchdog();
	} catch (_error) {
		setBanner("error", "The device could not be reached.");
	} finally {
		setBusy(false);
	}
});

poll(loadWatchdog, 5000).start();
