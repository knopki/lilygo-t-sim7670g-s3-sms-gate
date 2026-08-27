/**
 * #region moduleContract
 * @purpose Time Sync page logic: TimeSync status polling and the NTP
 *   server form backed by GET/POST /api/ntp.
 * @scope /time only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import { apiFetch, fillFields, poll, submitSettingsForm } from "/js/main.js";

const SOURCE_LABELS = {
	unsynced: "Not synchronized",
	sntp: "SNTP",
	nitz: "NITZ (modem)",
	gnss: "GNSS",
};

const ntpForm = document.getElementById("ntp-form");

function formatUtc(epochMs) {
	if (!Number.isFinite(epochMs) || epochMs <= 0) {
		return "—";
	}
	return `${new Date(epochMs).toISOString().slice(0, 19).replace("T", " ")} UTC`;
}

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

async function loadConfig() {
	const { response, payload } = await apiFetch("/api/ntp");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(ntpForm, {
		ntp_enabled: payload.ntp_enabled === true,
		ntp_server1: payload.ntp_server1 ?? "",
		ntp_server2: payload.ntp_server2 ?? "",
	});
}

ntpForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = ntpForm.elements;
	submitSettingsForm("/api/ntp", {
		ntp_enabled: elements.ntp_enabled.checked ? "1" : "0",
		ntp_server1: elements.ntp_server1.value.trim(),
		ntp_server2: elements.ntp_server2.value.trim(),
	});
});

poll(loadTime, 5000).start();
loadConfig();
