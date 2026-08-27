/**
 * #region moduleContract
 * @purpose SMS Send page logic: channel discovery across enabled modems,
 *   client-side recipient validation mirroring firmware rules, the UCS-2
 *   unit/segment counter (335-unit limit, 67 units per segment) and the
 *   unified send operation.
 * @scope /sms only; NOT: shared helpers or other pages.
 * #endregion moduleContract
 */

import { apiFetch, fillList, runAsyncOperation, setBanner } from "/js/main.js";

const MAX_UNITS = 335;
const UNITS_PER_SEGMENT = 67;

const smsForm = document.getElementById("sms-form");
const viaSelect = document.getElementById("via-select");
const textInput = document.getElementById("sms-text");
const counter = document.getElementById("sms-counter");

function channelLabel(base, payload) {
	return payload.label ? `${base} — ${payload.label}` : base;
}

async function loadChannels() {
	const [modem, zte] = await Promise.all([
		apiFetch("/api/modem/source"),
		apiFetch("/api/zte"),
	]);
	const channels = [];
	if (modem.response.ok && modem.payload?.module_enabled) {
		channels.push({
			id: "modem",
			label: channelLabel("Internal modem", modem.payload),
		});
	}
	if (zte.response.ok && zte.payload?.module_enabled) {
		channels.push({
			id: "zte",
			label: channelLabel("ZTE MF79RU", zte.payload),
		});
	}
	if (channels.length === 0) {
		document.getElementById("sms-disabled-hint").hidden = false;
		return;
	}
	fillList(viaSelect, "via-option", channels, (node, channel) => {
		node.value = channel.id;
		node.textContent = channel.label;
	});
	smsForm.hidden = false;
}

// Mirrors firmware smsValidate: the recipient is 3-20 digits with an
// optional single leading plus.
function recipientValid(value) {
	const trimmed = value.trim();
	const digits = trimmed.startsWith("+") ? trimmed.slice(1) : trimmed;
	return trimmed.length >= 3 && trimmed.length <= 20 && /^\d+$/.test(digits);
}

// Mirrors firmware smsUtf16Units: JS string length already counts UTF-16
// code units, the unit the modem path bills for UCS-2 payloads.
function updateCounter() {
	const units = textInput.value.length;
	const segments = units === 0 ? 0 : Math.ceil(units / UNITS_PER_SEGMENT);
	counter.textContent =
		`${units} units · ${segments} ${segments === 1 ? "segment" : "segments"}` +
		` · UCS-2 · limit ${MAX_UNITS} units`;
	counter.classList.toggle("error", units > MAX_UNITS);
}

smsForm.addEventListener("submit", (event) => {
	event.preventDefault();
	const to = smsForm.elements.to.value.trim();
	const text = textInput.value;
	if (!recipientValid(to)) {
		setBanner(
			"error",
			"Recipient must be 3-20 digits with an optional leading +.",
		);
		return;
	}
	if (text.length === 0) {
		setBanner("error", "The message must not be empty.");
		return;
	}
	if (text.length > MAX_UNITS) {
		setBanner("error", `The message exceeds the ${MAX_UNITS}-unit limit.`);
		return;
	}
	const via = viaSelect.value;
	runAsyncOperation("/api/sms/send", { via, to, text }, `/api/${via}/send`);
});

textInput.addEventListener("input", updateCounter);

updateCounter();
loadChannels();
