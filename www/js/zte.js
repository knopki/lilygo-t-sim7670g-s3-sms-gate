/**
 * #region moduleContract
 * @purpose ZTE MF79RU page logic: module settings form with saved-password
 *   placeholder, last poll status refresh and the connection test.
 * @scope /zte only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import {
	apiFetch,
	fillFields,
	poll,
	runAsyncOperation,
	submitSettingsForm,
} from "/js/main.js";

const zteForm = document.getElementById("zte-form");
const passwordInput = zteForm.elements.password;

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

function applyPasswordPlaceholder(passwordSet) {
	passwordInput.placeholder = passwordSet
		? "Saved — leave blank to keep"
		: "Enter password";
}

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
	applyPasswordPlaceholder(payload.password_set === true);
}

async function refreshLastStatus() {
	const { response, payload } = await apiFetch("/api/zte");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(document.getElementById("zte-config"), {
		last_status: payload.last_status || "—",
	});
}

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
