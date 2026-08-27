/**
 * #region moduleContract
 * @purpose E-mail page logic: SMTP settings form with a saved-password
 *   placeholder and the async test-email operation.
 * @scope /email only; NOT: shared helpers or other pages.
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

function applyPasswordPlaceholder(passwordSet) {
	passwordInput.placeholder = passwordSet
		? "Saved — leave blank to keep"
		: "Enter password";
}

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
