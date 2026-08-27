/**
 * #region moduleContract
 * @purpose GPS page logic: GNSS status polling and the module/polling/
 *   time-sync settings form.
 * @scope /gps only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import {
	apiFetch,
	bindFieldDependencies,
	fillFields,
	poll,
	submitSettingsForm,
} from "/js/main.js";

const gpsForm = document.getElementById("gps-form");

// Polling requires the GNSS module task; time sync and the poll interval
// only matter while polling runs.
const syncForm = bindFieldDependencies(gpsForm, {
	poll_enabled: "module_enabled",
	time_sync: "poll_enabled",
	poll_interval: "poll_enabled",
});

function text(value) {
	return value === null || value === undefined || value === ""
		? "—"
		: String(value);
}

async function loadStatus() {
	const { response, payload } = await apiFetch("/api/gps/status");
	if (!response.ok || !payload) {
		return;
	}
	const sats = payload.sats ?? {};
	const coords = payload.coords ?? {};
	const time = payload.time ?? {};
	fillFields(document.getElementById("gps-status"), {
		present: payload.present ? "detected" : "not detected",
		powered: payload.powered ? "on" : "off",
		fix: payload.fix ? "yes" : "no",
		mode: text(payload.mode),
		satellites: `${text(sats.used)} used / ${text(sats.visible)} visible`,
		latitude: text(coords.lat),
		longitude: text(coords.lon),
		altitude: text(coords.alt),
		speed: text(payload.speed),
		course: text(payload.course),
		time: text(time.iso),
	});
}

async function loadConfig() {
	const { response, payload } = await apiFetch("/api/gps");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(gpsForm, {
		module_enabled: payload.module_enabled === true,
		poll_enabled: payload.poll_enabled === true,
		time_sync: payload.time_sync_enabled === true,
		poll_interval: String(payload.poll_interval ?? ""),
	});
	syncForm();
}

gpsForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = gpsForm.elements;
	submitSettingsForm("/api/gps", {
		module_enabled: elements.module_enabled.checked ? "1" : "0",
		poll_enabled: elements.poll_enabled.checked ? "1" : "0",
		time_sync: elements.time_sync.checked ? "1" : "0",
		poll_interval: elements.poll_interval.value.trim(),
	});
});

poll(loadStatus, 5000).start();
loadConfig();
