/**
 * #region moduleContract
 * @modulecontract
 * @purpose Lets operators maintain SMTP delivery settings and verify them.
 * @scope /email page; NOT: shared runtime.
 * #endregion moduleContract
 */

import {
	apiFetch,
	fillFields,
	runAsyncOperation,
	submitSettingsForm,
} from "/js/main.js";

const smtpForm = document.getElementById("smtp-form");
const passwordInput = smtpForm.elements.password;

// #region FUNC_formFields
/**
 * @purpose Prevents SMTP field-name and secret-handling drift during settings writes.
 */
function formFields() {
	const elements = smtpForm.elements;
	return {
		host: elements.host.value.trim(),
		port: elements.port.value.trim(),
		security: elements.security.value,
		username: elements.username.value.trim(),
		password: elements.password.value,
		from: elements.from.value.trim(),
		recipient: elements.recipient.value.trim(),
	};
}
// #endregion FUNC_formFields

// #region FUNC_applyPasswordPlaceholder
/**
 * @purpose Signals when a saved secret can be retained without re-entry.
 */
function applyPasswordPlaceholder(passwordSet) {
	passwordInput.placeholder = passwordSet
		? "Saved — leave blank to keep"
		: "Enter password";
}
// #endregion FUNC_applyPasswordPlaceholder

// #region FUNC_loadConfig
/**
 * @purpose Prevents stale browser values from overwriting saved SMTP settings.
 */
async function loadConfig() {
	const { response, payload } = await apiFetch("/api/smtp");
	if (!response.ok || !payload) {
		return;
	}
	fillFields(smtpForm, {
		host: payload.host ?? "",
		port: payload.port ? String(payload.port) : "",
		security: payload.security ?? "starttls",
		username: payload.username ?? "",
		from: payload.from ?? "",
		recipient: payload.recipient ?? "",
	});
	applyPasswordPlaceholder(payload.password_set === true);
}
// #endregion FUNC_loadConfig

smtpForm.addEventListener("submit", (event) => {
	event.preventDefault();
	submitSettingsForm("/api/smtp", formFields(), () => {
		passwordInput.value = "";
		applyPasswordPlaceholder(true);
	});
});

document.getElementById("smtp-test").addEventListener("click", () => {
	runAsyncOperation("/api/smtp/test", formFields());
});

loadConfig();
