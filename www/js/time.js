/**
 * #region moduleContract
 * @modulecontract
 * @purpose Lets operators inspect synchronization health and control NTP input.
 * @scope /time page; NOT: shared runtime.
 * #endregion moduleContract
 */

import {
	apiFetch,
	bindFieldDependencies,
	fillFields,
	poll,
	setBanner,
	submitSettingsForm,
} from "/js/main.js";

const SOURCE_LABELS = {
	unsynced: "Not synchronized",
	sntp: "SNTP",
	nitz: "NITZ (modem)",
	gnss: "GNSS",
};

const ntpForm = document.getElementById("ntp-form");
const saveButton = ntpForm.querySelector('button[type="submit"]');
let configLoaded = false;

// The firmware starts SNTP and consumes the server addresses only while the
// enable flag is on (wifi_manager.cpp gates startSntp on ntpEnabled).
const syncForm = bindFieldDependencies(ntpForm, {
	ntp_server1: "ntp_enabled",
	ntp_server2: "ntp_enabled",
});

// #region FUNC_formatUtc
/**
 * @purpose Gives synchronization timestamps a stable operator-readable form.
 */
function formatUtc(epochMs) {
	if (!Number.isFinite(epochMs) || epochMs <= 0) {
		return "—";
	}
	return `${new Date(epochMs).toISOString().slice(0, 19).replace("T", " ")} UTC`;
}
// #endregion FUNC_formatUtc

// #region FUNC_loadTime
/**
 * @purpose Keeps synchronization health visible without stale readings.
 */
async function loadTime() {
	const { response, payload } = await apiFetch("/api/time");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(document.getElementById("time-status"), {
		source: SOURCE_LABELS[payload.source] ?? payload.source ?? "—",
		stratum: String(payload.stratum ?? "—"),
		dispersion: `${payload.dispersion_ms} ms`,
		now: formatUtc(payload.epoch_ms),
		last_sync: formatUtc(payload.last_sync_epoch_ms),
		quarantine: payload.quarantined
			? `active until ${formatUtc(payload.quarantined_until_epoch_ms)}`
			: "inactive",
	});
}
// #endregion FUNC_loadTime

// #region FUNC_loadConfig
/**
 * @purpose Restores NTP controls so changes start from device state.
 */
async function loadConfig() {
	try {
		const { response, payload } = await apiFetch("/api/ntp");
		if (!response.ok || !payload) {
			if (response.status !== 401) {
				setBanner(
					"error",
					"NTP settings could not be loaded. Saving is disabled.",
				);
			}
			return;
		}
		fillFields(ntpForm, {
			ntp_enabled: payload.ntp_enabled === true,
			ntp_server1: payload.ntp_server1 ?? "",
			ntp_server2: payload.ntp_server2 ?? "",
		});
		syncForm();
		configLoaded = true;
		saveButton.disabled = false;
	} catch (_error) {
		setBanner("error", "NTP settings could not be loaded. Saving is disabled.");
	}
}
// #endregion FUNC_loadConfig

ntpForm.addEventListener("submit", (event) => {
	event.preventDefault();
	if (!configLoaded) {
		return;
	}
	const elements = ntpForm.elements;
	submitSettingsForm("/api/ntp", {
		ntp_enabled: elements.ntp_enabled.checked ? "1" : "0",
		ntp_server1: elements.ntp_server1.value.trim(),
		ntp_server2: elements.ntp_server2.value.trim(),
	});
});

poll(loadTime, 5000).start();
loadConfig();
