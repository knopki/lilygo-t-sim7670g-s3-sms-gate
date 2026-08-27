/**
 * #region moduleContract
 * @purpose Shared UI runtime: fetch wrapper with request timeout and
 *   centralized 401 handling, banner and busy state, reachability banner,
 *   visibility-aware polling, template DOM helpers and one-time session
 *   prefetch of sibling pages.
 * @scope imported by every page script; NOT: page-specific markup or logic.
 * #endregion moduleContract
 */

const banner = document.getElementById("banner");
const pollers = new Set();
const FETCH_TIMEOUT_MS = 8000;
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

export function setBusy(value) {
	busy = value;
	for (const button of document.querySelectorAll("button")) {
		button.disabled = value;
	}
}

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

export function postForm(path, fields) {
	return apiFetch(path, {
		method: "POST",
		body: new URLSearchParams(fields),
	});
}

// Creates a poller that pauses while the tab is hidden or the UI is busy
// and refreshes immediately when the tab becomes visible again. Long-running
// device operations pass { ignoreBusy: true } to keep polling while busy.
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

// Starts a long-running device operation (test or SMS send) and polls its
// status endpoint every 1.5 s until the result envelope arrives; the UI stays
// busy for the whole dialog.
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
	const controller = poll(
		async () => {
			const { response, payload } = await apiFetch(pollPath);
			if (response.status === 401) {
				controller.stop();
				setBusy(false);
				return;
			}
			if (!response.ok || !payload || payload.running || !payload.done) {
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

export function cloneTemplate(templateId) {
	const template = document.getElementById(templateId);
	return template.content.firstElementChild.cloneNode(true);
}

export function fillList(container, templateId, items, bind) {
	container.replaceChildren();
	for (const item of items) {
		const node = cloneTemplate(templateId);
		bind(node, item);
		container.append(node);
	}
}

// Fills elements marked with data-field="<name>" from a values map:
// checkboxes become checked, form controls get value, the rest textContent.
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
