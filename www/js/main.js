/**
 * #region moduleContract
 * @modulecontract
 * @purpose Keeps page interactions consistent and recoverable across the device UI.
 * @scope shared page runtime; NOT: page-specific markup or logic.
 * #endregion moduleContract
 */

const banner = document.getElementById("banner");
const pollers = new Set();
const FETCH_TIMEOUT_MS = 8000;
const MAX_STATUS_READ_RETRIES = 3;
let busy = false;
let reachabilityBanner = false;

// Raises a sticky "device not responding" banner on the first failed poll
// and clears only that banner when a request succeeds again.
function markReachability(reachable) {
	if (!reachable && !reachabilityBanner) {
		reachabilityBanner = true;
		setBanner("error", "The device is not responding. Retrying…");
		return;
	}
	if (reachable && reachabilityBanner) {
		reachabilityBanner = false;
		setBanner("", "");
	}
}

function stopAllPollers() {
	for (const controller of [...pollers]) {
		controller.stop();
	}
}

function handleUnauthorized() {
	stopAllPollers();
	setBanner("error", "Authentication required. Reload the page to sign in.");
}

// #region FUNC_setBusy
/**
 * @purpose Keeps controls consistent while an operation is in flight.
 */
export function setBusy(value) {
	busy = value;
	for (const button of document.querySelectorAll("button")) {
		button.disabled = value;
	}
}
// #endregion FUNC_setBusy

// #region FUNC_setBanner
/**
 * @purpose Gives every page one predictable way to report outcomes.
 */
export function setBanner(kind, text) {
	if (!text) {
		banner.hidden = true;
		banner.textContent = "";
		return;
	}
	banner.className = `banner ${kind}`;
	banner.textContent = text;
	banner.hidden = false;
}
// #endregion FUNC_setBanner

// #region FUNC_apiFetch
/**
 * @purpose Prevents stalled or unauthorized requests from trapping page state.
 */
export async function apiFetch(path, options) {
	// Bound every request so an unresponsive device cannot park fetches
	// in the browser queue indefinitely (e.g. right after a reset).
	const controller = new AbortController();
	const timeout = window.setTimeout(() => controller.abort(), FETCH_TIMEOUT_MS);
	let response;
	try {
		response = await fetch(path, { ...options, signal: controller.signal });
	} finally {
		window.clearTimeout(timeout);
	}
	let payload = null;
	try {
		payload = await response.json();
	} catch (_error) {
		// Non-JSON body; callers rely on response.ok instead.
	}
	if (response.status === 401) {
		handleUnauthorized();
	}
	return { response, payload };
}
// #endregion FUNC_apiFetch

// #region FUNC_postForm
/**
 * @purpose Gives settings endpoints the form encoding they expect.
 */
export function postForm(path, fields) {
	return apiFetch(path, {
		method: "POST",
		body: new URLSearchParams(fields),
	});
}
// #endregion FUNC_postForm

// Creates a poller that pauses while the tab is hidden or the UI is busy
// and refreshes immediately when the tab becomes visible again. Long-running
// device operations pass { ignoreBusy: true } to keep polling while busy.
// #region FUNC_poll
/**
 * @purpose Keeps periodic device updates efficient and recoverable in the browser.
 */
export function poll(task, intervalMs, { ignoreBusy = false } = {}) {
	let timer = null;
	let inFlight = false;
	async function tick() {
		if ((busy && !ignoreBusy) || document.hidden || inFlight) {
			return;
		}
		inFlight = true;
		try {
			await task();
			markReachability(true);
		} catch (_error) {
			// Reachability loss surfaces through the banner; the next interval retries.
			markReachability(false);
		} finally {
			inFlight = false;
		}
	}
	const controller = {
		start() {
			if (timer !== null) {
				return;
			}
			timer = window.setInterval(tick, intervalMs);
			pollers.add(controller);
			tick();
		},
		stop() {
			if (timer === null) {
				return;
			}
			window.clearInterval(timer);
			timer = null;
			pollers.delete(controller);
		},
		refresh: tick,
	};
	return controller;
}
// #endregion FUNC_poll

document.addEventListener("visibilitychange", () => {
	if (document.hidden) {
		return;
	}
	for (const controller of pollers) {
		controller.refresh();
	}
});

// Posts a settings form and reports the uniform ok/error envelope through
// the banner; onOk runs only after the device accepted the change. Returns
// true when the device accepted the submission.
// #region FUNC_submitSettingsForm
/**
 * @purpose Gives settings writes a uniform busy, success, and failure lifecycle.
 */
export async function submitSettingsForm(path, fields, onOk) {
	setBusy(true);
	setBanner("", "");
	try {
		const { response, payload } = await postForm(path, fields);
		if (response.status === 401) {
			return false;
		}
		if (response.ok && payload?.ok) {
			if (onOk) {
				onOk(payload);
			}
			setBanner("ok", payload.message || "Saved.");
			return true;
		}
		setBanner("error", payload?.error || "The device rejected the change.");
		return false;
	} catch (_error) {
		setBanner("error", "The device could not be reached.");
		return false;
	} finally {
		setBusy(false);
	}
}
// #endregion FUNC_submitSettingsForm

// Starts a long-running device operation (test or SMS send) and polls its
// status endpoint every 1.5 s. The UI stays busy while the operation reports
// running and across a bounded number of lost or malformed status reads.
// #region FUNC_runAsyncOperation
/**
 * @purpose Keeps long device operations observable without blocking the page.
 */
export async function runAsyncOperation(path, fields, pollPath = path) {
	setBusy(true);
	setBanner("", "");
	let started;
	try {
		started = await postForm(path, fields);
	} catch (_error) {
		setBanner("error", "The device could not be reached.");
		setBusy(false);
		return;
	}
	if (started.response.status === 401) {
		setBusy(false);
		return;
	}
	if (!started.response.ok || started.payload?.ok !== true) {
		setBanner(
			"error",
			started.payload?.error || "The operation could not be started.",
		);
		setBusy(false);
		return;
	}
	setBanner("ok", started.payload?.message || "The operation is running…");
	let controller;
	let statusReadRetries = 0;
	const stopPolling = (message) => {
		controller.stop();
		setBusy(false);
		if (message) {
			setBanner("error", message);
		}
	};
	const retryStatusRead = () => {
		statusReadRetries += 1;
		if (statusReadRetries > MAX_STATUS_READ_RETRIES) {
			stopPolling("The operation status could not be read. Try again.");
		}
	};
	controller = poll(
		async () => {
			let response;
			let payload;
			try {
				({ response, payload } = await apiFetch(pollPath));
			} catch (_error) {
				retryStatusRead();
				return;
			}
			if (response.status === 401) {
				stopPolling();
				return;
			}
			if (!response.ok || !payload) {
				retryStatusRead();
				return;
			}
			statusReadRetries = 0;
			if (payload.running === true) {
				return;
			}
			if (payload.done !== true) {
				stopPolling("The operation was interrupted. Try again.");
				return;
			}
			controller.stop();
			setBusy(false);
			const succeeded = payload.result === "success";
			setBanner(succeeded ? "ok" : "error", payload.message || "");
		},
		1500,
		{ ignoreBusy: true },
	);
	controller.start();
}
// #endregion FUNC_runAsyncOperation

// #region FUNC_cloneTemplate
/**
 * @purpose Creates isolated DOM instances for repeated page content.
 */
export function cloneTemplate(templateId) {
	const template = document.getElementById(templateId);
	return template.content.firstElementChild.cloneNode(true);
}
// #endregion FUNC_cloneTemplate

// #region FUNC_fillList
/**
 * @purpose Renders repeated records without duplicating page-specific markup.
 */
export function fillList(container, templateId, items, bind) {
	container.replaceChildren();
	for (const item of items) {
		const node = cloneTemplate(templateId);
		bind(node, item);
		container.append(node);
	}
}
// #endregion FUNC_fillList

// Fills elements marked with data-field="<name>" from a values map:
// checkboxes become checked, form controls get value, the rest textContent.
// #region FUNC_fillFields
/**
 * @purpose Synchronizes API values with the page controls that display them.
 */
export function fillFields(root, values) {
	for (const element of root.querySelectorAll("[data-field]")) {
		const value = values[element.dataset.field];
		if (element instanceof HTMLInputElement && element.type === "checkbox") {
			element.checked = Boolean(value);
			continue;
		}
		if (
			element instanceof HTMLInputElement ||
			element instanceof HTMLTextAreaElement ||
			element instanceof HTMLSelectElement
		) {
			if (document.activeElement !== element) {
				element.value = value ?? "";
			}
			continue;
		}
		element.textContent = value ?? "";
	}
}
// #endregion FUNC_fillFields

// Keeps form controls disabled while the checkbox chain they depend on is
// off: a field stays editable only while every master checkbox is checked
// AND its own masters are active, recursively ("<dependent>: <master|masters>").
// The rules table must be acyclic. Applies immediately, on every master
// change, and returns a sync function to re-run after fillFields() replaced
// the loaded values (checked changes programmatically fire no event).
// Values are preserved on purpose: disabling never clears stored settings,
// and submit handlers still read .checked/.value from disabled controls.
// #region FUNC_bindFieldDependencies
/**
 * @purpose Prevents dependent settings from being edited when their prerequisites are off.
 */
export function bindFieldDependencies(form, rules) {
	const elements = form.elements;
	const mastersOf = new Map(
		Object.entries(rules).map(([name, requires]) => [
			name,
			Array.isArray(requires) ? requires : [requires],
		]),
	);
	const masters = new Set();
	for (const names of mastersOf.values()) {
		masters.add(...names);
	}
	function sync() {
		// Memoized per run, so evaluation order never matters: a stale
		// checked-but-disabled master must not keep its own dependents alive.
		const active = new Map();
		const isChainOn = (name) => {
			if (active.has(name)) {
				return active.get(name);
			}
			const state =
				Boolean(elements[name]?.checked) &&
				(mastersOf.get(name) ?? []).every(isChainOn);
			active.set(name, state);
			return state;
		};
		for (const [name, requires] of mastersOf) {
			const control = elements[name];
			if (!control) {
				continue;
			}
			const enabled = requires.every(isChainOn);
			control.disabled = !enabled;
			control.closest("label")?.classList.toggle("is-disabled", !enabled);
		}
	}
	for (const name of masters) {
		elements[name]?.addEventListener("change", sync);
	}
	sync();
	return sync;
}
// #endregion FUNC_bindFieldDependencies

// Warms the browser cache with all pages and scripts once per session
// (staggered, silent) so menu navigation renders from cache instead of
// waiting on the device. Plain fetch on purpose: a 401 or a timeout here
// must never raise the authentication or reachability banners nor stop
// the page pollers.
const PREFETCH_ASSETS = [
	"/wifi",
	"/admin",
	"/email",
	"/time",
	"/modem",
	"/zte",
	"/gps",
	"/sms",
	"/style.css",
	"/js/main.js",
	"/js/wifi.js",
	"/js/admin.js",
	"/js/email.js",
	"/js/time.js",
	"/js/modem.js",
	"/js/zte.js",
	"/js/gps.js",
	"/js/sms.js",
];
const PREFETCH_FLAG = "sms-gate-prefetched";

function sessionFlagSet(name) {
	try {
		return window.sessionStorage.getItem(name) !== null;
	} catch (_error) {
		return false;
	}
}

function markSessionFlag(name) {
	try {
		window.sessionStorage.setItem(name, "1");
	} catch (_error) {
		// Storage unavailable: prefetch again next time, which is harmless.
	}
}

function prefetchAssets() {
	let index = 0;
	function next() {
		if (index >= PREFETCH_ASSETS.length) {
			markSessionFlag(PREFETCH_FLAG);
			return;
		}
		const path = PREFETCH_ASSETS[index];
		index += 1;
		fetch(path).catch(() => {});
		window.setTimeout(next, 250);
	}
	next();
}

window.setTimeout(() => {
	if (!sessionFlagSet(PREFETCH_FLAG)) {
		prefetchAssets();
	}
}, 1500);
