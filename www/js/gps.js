/**
 * #region moduleContract
 * @modulecontract
 * @purpose Keeps GNSS availability and time-sync settings observable and controlled.
 * @scope /gps page; NOT: shared runtime.
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

const gpsForm = document.getElementById("gps-form");
const saveButton = gpsForm.querySelector('button[type="submit"]');
let configLoaded = false;

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

// #region FUNC_loadStatus
/**
 * @purpose Keeps GNSS diagnostics current while the page remains open.
 */
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
		satellites: [
			`${text(sats.gps)} GPS`,
			`${text(sats.glonass)} GLONASS`,
			`${text(sats.galileo)} Galileo`,
			`${text(sats.beidou)} BeiDou`,
			`total ${text(sats.visible)} visible / ${text(sats.used)} used`,
		].join(" · "),
		latitude: text(coords.lat),
		longitude: text(coords.lon),
		altitude: text(coords.alt),
		speed: text(payload.speed),
		course: text(payload.course),
		time: text(time.iso),
	});
}
// #endregion FUNC_loadStatus

// #region FUNC_loadConfig
/**
 * @purpose Prevents stale browser values from overwriting saved GNSS settings.
 */
async function loadConfig() {
	try {
		const { response, payload } = await apiFetch("/api/gps");
		if (!response.ok || !payload) {
			if (response.status !== 401) {
				setBanner(
					"error",
					"GPS settings could not be loaded. Saving is disabled.",
				);
			}
			return;
		}
		fillFields(gpsForm, {
			module_enabled: payload.module_enabled === true,
			poll_enabled: payload.poll_enabled === true,
			time_sync: payload.time_sync_enabled === true,
			poll_interval: String(payload.poll_interval ?? ""),
		});
		syncForm();
		configLoaded = true;
		saveButton.disabled = false;
	} catch (_error) {
		setBanner("error", "GPS settings could not be loaded. Saving is disabled.");
	}
}
// #endregion FUNC_loadConfig

gpsForm.addEventListener("submit", (event) => {
	event.preventDefault();
	if (!configLoaded) {
		return;
	}
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
