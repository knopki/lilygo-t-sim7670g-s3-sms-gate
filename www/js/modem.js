/**
 * #region moduleContract
 * @purpose Internal modem page logic: SIM7670G status polling and the
 *   module/polling/forwarding/NITZ settings form.
 * @scope /modem only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import { apiFetch, fillFields, poll, submitSettingsForm } from "/js/main.js";

const sourceForm = document.getElementById("source-form");

function text(value) {
	return value === null || value === undefined || value === ""
		? "—"
		: String(value);
}

async function loadStatus() {
	const { response, payload } = await apiFetch("/api/modem/status");
	if (!response.ok || !payload) {
		return;
	}
	const signal = payload.signal ?? {};
	const registration = payload.registration ?? {};
	const operator = payload.operator ?? {};
	const storage = payload.sms_storage ?? {};
	const identity = payload.identity ?? {};
	fillFields(document.getElementById("modem-status"), {
		present: payload.present ? "detected" : "not detected",
		cpin: text(payload.cpin),
		signal: `${text(signal.rssi_dbm)} dBm`,
		rsrp: `${text(signal.rsrp_dbm)} dBm`,
		rsrq: `${text(signal.rsrq_db)} dB`,
		ber: text(signal.ber),
		cereg: text(registration.cereg),
		attached: registration.attached ? "yes" : "no",
		operator: [operator.name, operator.act].filter(Boolean).join(" · ") || "—",
		clock: text(payload.clock),
		storage_device: `${text(storage.used)}/${text(storage.total)} (${text(storage.mem)})`,
		storage_sim: `${text(storage.used2)}/${text(storage.total2)} (${text(storage.mem2)})`,
		imei: text(identity.imei),
		firmware: text(identity.fw),
	});
}

function applyLastStatus(payload) {
	fillFields(document.getElementById("modem-config"), {
		last_status: payload.last_status || "—",
	});
}

function applyConfig(payload) {
	fillFields(sourceForm, {
		module_enabled: payload.module_enabled === true,
		poll_enabled: payload.poll_enabled === true,
		sms_poll: payload.sms_poll_enabled === true,
		nitz_time_sync: payload.nitz_time_sync_enabled === true,
		poll_interval: String(payload.poll_interval ?? ""),
		label: payload.label ?? "",
	});
	applyLastStatus(payload);
}

async function loadConfig() {
	const { response, payload } = await apiFetch("/api/modem/source");
	if (response.ok && payload) {
		applyConfig(payload);
	}
}

sourceForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = sourceForm.elements;
	submitSettingsForm("/api/modem/source", {
		module_enabled: elements.module_enabled.checked ? "1" : "0",
		poll_enabled: elements.poll_enabled.checked ? "1" : "0",
		sms_poll: elements.sms_poll.checked ? "1" : "0",
		nitz_time_sync: elements.nitz_time_sync.checked ? "1" : "0",
		poll_interval: elements.poll_interval.value.trim(),
		label: elements.label.value.trim(),
	});
});

poll(async () => {
	await loadStatus();
	const { response, payload } = await apiFetch("/api/modem/source");
	if (response.ok && payload) {
		applyLastStatus(payload);
	}
}, 5000).start();
loadConfig();
