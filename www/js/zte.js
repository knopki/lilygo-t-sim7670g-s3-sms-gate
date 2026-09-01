/**
 * #region moduleContract
 * @modulecontract
 * @purpose Keeps the auxiliary modem configured and its connection testable.
 * @scope /zte page; NOT: shared runtime.
 * #endregion moduleContract
 */

import {
	apiFetch,
	bindFieldDependencies,
	fillFields,
	poll,
	runAsyncOperation,
	submitSettingsForm,
} from "/js/main.js";

const zteForm = document.getElementById("zte-form");
const passwordInput = zteForm.elements.password;

// The module task holds the connection profile (host, password); the poll
// interval is consumed only inside poll cycles, which run solely while SMS
// forwarding is on; the label only marks forwarded emails.
const syncForm = bindFieldDependencies(zteForm, {
	forward_enabled: "module_enabled",
	host: "module_enabled",
	password: "module_enabled",
	poll_interval: "forward_enabled",
	label: "forward_enabled",
});

// #region FUNC_formFields
/**
 * @purpose Prevents auxiliary-modem field drift during settings writes.
 */
function formFields() {
	const elements = zteForm.elements;
	return {
		module_enabled: elements.module_enabled.checked ? "1" : "0",
		forward_enabled: elements.forward_enabled.checked ? "1" : "0",
		host: elements.host.value.trim(),
		password: elements.password.value,
		poll_interval: elements.poll_interval.value.trim(),
		label: elements.label.value.trim(),
	};
}
// #endregion FUNC_formFields

// #region FUNC_applyPasswordPlaceholder
/**
 * @purpose Signals when a saved modem secret can be retained without re-entry.
 */
function applyPasswordPlaceholder(passwordSet) {
	passwordInput.placeholder = passwordSet
		? "Saved — leave blank to keep"
		: "Enter password";
}
// #endregion FUNC_applyPasswordPlaceholder

// #region FUNC_loadConfig
/**
 * @purpose Prevents stale browser values from overwriting the saved auxiliary profile.
 */
async function loadConfig() {
	const { response, payload } = await apiFetch("/api/zte");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(zteForm, {
		module_enabled: payload.module_enabled === true,
		forward_enabled: payload.forward_enabled === true,
		host: payload.host ?? "",
		poll_interval: String(payload.poll_interval ?? ""),
		label: payload.label ?? "",
	});
	syncForm();
	applyPasswordPlaceholder(payload.password_set === true);
}
// #endregion FUNC_loadConfig

// #region FUNC_refreshLastStatus
/**
 * @purpose Keeps the latest auxiliary modem outcome visible to operators.
 */
async function refreshLastStatus() {
	const { response, payload } = await apiFetch("/api/zte");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(document.getElementById("zte-config"), {
		last_status: payload.last_status || "—",
	});
}
// #endregion FUNC_refreshLastStatus

zteForm.addEventListener("submit", (event) => {
	event.preventDefault();
	submitSettingsForm("/api/zte", formFields(), () => {
		passwordInput.value = "";
		applyPasswordPlaceholder(true);
		refreshLastStatus();
	});
});

document.getElementById("zte-test").addEventListener("click", () => {
	runAsyncOperation("/api/zte/test", formFields());
});

poll(refreshLastStatus, 5000).start();
loadConfig();
