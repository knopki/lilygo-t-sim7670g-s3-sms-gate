/**
 * #region moduleContract
 * @purpose Wi-Fi page logic: station status polling, network change form
 *   with saved-SSID prefill, on-demand scan picker and the initial-setup
 *   combined Wi-Fi + administrator form.
 * @scope /wifi only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import {
	apiFetch,
	fillFields,
	fillList,
	poll,
	setBanner,
	setBusy,
	submitSettingsForm,
} from "/js/main.js";

const MODE_LABELS = {
	initial: "Initial setup (open access point)",
	connecting: "Connecting…",
	sta: "Connected to Wi-Fi",
	fallback_ap: "Fallback access point",
};

let ssidPrefilled = false;

const setupSection = document.getElementById("setup-section");
const wifiSection = document.getElementById("wifi-section");
const setupForm = document.getElementById("setup-form");
const networkForm = document.getElementById("network-form");

function applyStatus(status) {
	const setupRequired = status.setup_required === true;
	setupSection.hidden = !setupRequired;
	wifiSection.hidden = setupRequired;
	fillFields(document.getElementById("wifi-status"), {
		mode: MODE_LABELS[status.mode] ?? status.mode ?? "",
		ssid: status.ssid ?? "",
		station_ip: status.station_ip ?? "—",
		rssi:
			status.rssi_dbm === null || status.rssi_dbm === undefined
				? "—"
				: `${status.rssi_dbm} dBm`,
		mac: status.mac ?? "",
		mdns_hostname: status.mdns_hostname ?? "",
		last_error: status.last_error ?? "—",
	});
	// Bug fix: prefill the SSID once so a saved network is visible, without
	// clobbering user input on later status refreshes.
	if (!setupRequired && !ssidPrefilled) {
		ssidPrefilled = true;
		networkForm.elements.ssid.value = status.ssid ?? "";
	}
}

async function loadStatus() {
	const { response, payload } = await apiFetch("/api/status");
	if (response.ok && payload) {
		applyStatus(payload);
	}
}

function renderPicker(pickerId, form, networks) {
	const picker = document.getElementById(pickerId);
	fillList(picker, "network-row", networks, (node, network) => {
		node.querySelector(".picker-ssid").textContent =
			network.ssid || "Hidden network";
		node.querySelector(".picker-meta").textContent =
			`${network.rssi_dbm} dBm · ${network.security}`;
		node.addEventListener("click", () => {
			form.elements.ssid.value = network.ssid;
			form.elements.wifi_password.focus();
		});
	});
	picker.hidden = networks.length === 0;
}

async function runScan(pickerId, form) {
	setBusy(true);
	setBanner("ok", "Scanning nearby networks…");
	try {
		const { response, payload } = await apiFetch("/api/scan");
		if (response.status === 401) {
			return;
		}
		if (response.ok && payload && Array.isArray(payload.networks)) {
			setBanner("", "");
			renderPicker(pickerId, form, payload.networks);
		} else {
			setBanner("error", "The network scan failed.");
		}
	} catch (_error) {
		setBanner("error", "The device could not be reached for the scan.");
	} finally {
		setBusy(false);
	}
}

setupForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = setupForm.elements;
	submitSettingsForm(
		"/api/setup",
		{
			ssid: elements.ssid.value.trim(),
			wifi_password: elements.wifi_password.value,
			admin_password: elements.admin_password.value,
			admin_password_confirm: elements.admin_password_confirm.value,
		},
		() => {
			setupForm.reset();
		},
	);
});

networkForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const elements = networkForm.elements;
	submitSettingsForm(
		"/api/network",
		{
			ssid: elements.ssid.value.trim(),
			wifi_password: elements.wifi_password.value,
		},
		() => {
			networkForm.elements.wifi_password.value = "";
			ssidPrefilled = false;
			loadStatus();
		},
	);
});

document.getElementById("setup-scan-button").addEventListener("click", () => {
	runScan("setup-picker", setupForm);
});

document.getElementById("scan-button").addEventListener("click", () => {
	runScan("network-picker", networkForm);
});

poll(loadStatus, 5000).start();
