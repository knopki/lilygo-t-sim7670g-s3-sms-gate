/**
 * #region moduleContract
 * @purpose Renders the SMS Gate UI including the SIM7670G modem status block
 * @scope polling /api/status and /api/modem/status, form handling, NOT: modem AT or SMTP logic
 * #endregion moduleContract
 */

(() => {
	const appRoot = document.getElementById("app");
	const banner = document.getElementById("banner");

	const MODE_LABELS = {
		initial: "Initial setup",
		connecting: "Connecting…",
		sta: "Station (connected)",
		fallback_ap: "Fallback access point",
	};

	const STATUS_PATH = "/api/status";
	const STATUS_INTERVAL_MS = 5000;
	const ASYNC_TEST_POLL_MS = 1500;
	const MODEM_STATUS_PATH = "/api/modem/status";
	const MODEM_INTERVAL_MS = 5000;
	const GPS_STATUS_PATH = "/api/gps/status";
	const GPS_INTERVAL_MS = 5000;

	let networks = null;
	let busy = false;
	let statusTimer = null;
	let asyncTestTimer = null;
	let modemTimer = null;
	let gpsTimer = null;

	const esc = (value) =>
		String(value).replace(
			/[&<>"']/g,
			(ch) =>
				({
					"&": "&amp;",
					"<": "&lt;",
					">": "&gt;",
					'"': "&quot;",
					"'": "&#39;",
				})[ch],
		);

	function setBusy(value) {
		busy = value;
		for (const button of document.querySelectorAll("button")) {
			button.disabled = value;
		}
	}

	function setBanner(kind, text) {
		if (!text) {
			banner.hidden = true;
			banner.textContent = "";
			return;
		}
		banner.className = `banner ${kind}`;
		banner.textContent = text;
		banner.hidden = false;
	}

	async function api(path, options) {
		const response = await fetch(path, options);
		let payload = null;
		try {
			payload = await response.json();
		} catch (_error) {
			payload = null;
		}
		return { response, payload };
	}

	function postForm(path, fields) {
		return api(path, { method: "POST", body: new URLSearchParams(fields) });
	}

	function statusHtml(status) {
		const rows = [
			["Mode", MODE_LABELS[status.mode] || status.mode],
			["Configured network", status.ssid || "—"],
			["Station IP", status.station_ip || "—"],
			["Device MAC", status.mac || "—"],
			["Signal", status.rssi_dbm == null ? "—" : `${status.rssi_dbm} dBm`],
			[
				"Address",
				status.mdns_hostname ? `http://${status.mdns_hostname}.local` : "—",
			],
		];
		const rowsHtml = rows
			.map(([label, value]) => `<dt>${label}</dt><dd>${esc(value)}</dd>`)
			.join("");
		return `<dl class="status">${rowsHtml}</dl>`;
	}

	function actName(act) {
		const map = {
			0: "GSM",
			1: "GSM Compact",
			2: "UTRAN",
			3: "GSM w/ EGPRS",
			4: "UTRAN w/ HSDPA",
			5: "UTRAN w/ HSUPA",
			6: "UTRAN w/ HSDPA+HSUPA",
			7: "E-UTRAN (LTE)",
			8: "EC-GSM-IoT",
			9: "E-UTRAN NB-S1",
		};
		return map[act] || "unknown";
	}

	function operatorLabel(oper) {
		const mccMncNames = {
			25001: "MTS",
			25002: "MegaFon",
			25011: "MTS",
			25012: "Tele2",
			25020: "Tele2",
			25028: "Beeline",
			25099: "Beeline",
		};
		if (!oper) return "—";
		const name = mccMncNames[oper];
		return name ? `${name} (${oper})` : oper;
	}

	function formatClock(raw) {
		if (!raw) return '— <span class="hint">not synced (no NITZ yet)</span>';
		// +CCLK: "yy/MM/dd,hh:mm:ss+zz" where zz is quarters of hour
		const m = raw.match(/^\d{2}\/\d{2}\/\d{2},\d{2}:\d{2}:\d{2}([+-]\d{1,2})$/);
		if (!m) return esc(raw);
		const q = parseInt(m[1], 10);
		const hours = q / 4;
		const sign = hours >= 0 ? "+" : "";
		const tz = `UTC${sign}${hours}`;
		return `${esc(raw)} <span class="hint">${esc(tz)}</span>`;
	}

	// #region FUNC_renderModemStatus
	// PURPOSE: Maps WebModemStatus JSON into an operator-readable badge table.
	function renderModemStatus(data) {
		if (!data) return '<p class="hint">Modem status unavailable.</p>';
		if (!data.present) return '<p class="hint">Modem not responding.</p>';
		const cpin = data.cpin || "—";
		const cereg = data.registration ? data.registration.cereg : data.cereg;
		const creg = data.registration ? data.registration.creg : data.creg;
		const attached = data.registration
			? data.registration.attached
			: data.attached;
		const rssi = data.signal ? data.signal.rssi_dbm : data.rssi_dbm;
		const ber = data.signal ? data.signal.ber : data.ber;
		const rsrp = data.signal ? data.signal.rsrp_dbm : data.rsrp_dbm;
		const rsrq = data.signal ? data.signal.rsrq_db : data.rsrq_db;
		const oper = data.operator ? data.operator.name : data.oper;
		const act = data.operator ? data.operator.act : data.act;
		const clock = data.clock || "";
		const storage = data.sms_storage || {};
		const usedMe = storage.used != null ? storage.used : data.smsUsedMe;
		const totalMe = storage.total != null ? storage.total : data.smsTotalMe;
		const usedSm = storage.used2 != null ? storage.used2 : data.smsUsedSm;
		const totalSm = storage.total2 != null ? storage.total2 : data.smsTotalSm;
		const imei = data.identity ? data.identity.imei : data.imei;
		const fw = data.identity ? data.identity.fw : data.fw;
		let badge = "ready";
		if (cpin !== "READY") badge = `No SIM (${esc(cpin)})`;
		else if (cereg !== 1 && cereg !== 5 && creg !== 1 && creg !== 5)
			badge = "No network";
		else if (!attached) badge = "Not attached";
		const operText =
			operatorLabel(oper) +
			" — " +
			actName(act) +
			" (act " +
			esc(String(act)) +
			")";
		const rows = [
			["Modem", badge],
			["SIM", esc(cpin)],
			[
				"Registration",
				"CEREG " +
					esc(String(cereg)) +
					" / CREG " +
					esc(String(creg)) +
					(attached ? " attached" : " detached"),
			],
			["Operator", operText],
			[
				"Signal",
				esc(String(rssi)) +
					" dBm BER " +
					esc(String(ber)) +
					" / RSRP " +
					esc(String(rsrp)) +
					" RSRQ " +
					esc(String(rsrq)),
			],
			[
				"Storage ME",
				esc(String(usedMe)) +
					"/" +
					esc(String(totalMe)) +
					' <span class="hint">modem flash (ME)</span>',
			],
			[
				"Storage SM",
				esc(String(usedSm)) +
					"/" +
					esc(String(totalSm)) +
					' <span class="hint">SIM card (SM)</span>',
			],
			["Clock", formatClock(clock)],
			["IMEI", esc(imei || "—")],
			["FW", esc(fw || "—")],
		];
		const rowsHtml = rows
			.map(([l, v]) => `<dt>${esc(l)}</dt><dd>${v}</dd>`)
			.join("");
		return `<dl class="status">${rowsHtml}</dl>`;
	}
	// #endregion FUNC_renderModemStatus

	// #region FUNC_loadModemStatus
	// PURPOSE: Polls /api/modem/status and renders the internal modem block.
	async function loadModemStatus() {
		const el = document.getElementById("modem-status");
		if (!el) return;
		try {
			const { response, payload } = await api(MODEM_STATUS_PATH);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok && payload) el.innerHTML = renderModemStatus(payload);
		} catch (_error) {
			// Transient; keep last rendered content.
		}
	}
	// #endregion FUNC_loadModemStatus

	// #region FUNC_renderGpsStatus
	// PURPOSE: Maps WebGpsStatus JSON into operator-readable block.
	function renderGpsStatus(data) {
		if (!data) return '<p class="hint">GPS unavailable.</p>';
		if (!data.present) return '<p class="hint">Modem not responding (GPS).</p>';
		if (!data.powered) return '<p class="hint">GNSS not powered yet.</p>';
		const fixBadge = data.fix ? "FIX" : "No fix";
		const coords = data.coords || {};
		const lat = coords.lat != null ? coords.lat : data.lat;
		const lon = coords.lon != null ? coords.lon : data.lon;
		const alt = coords.alt != null ? coords.alt : data.alt;
		const sats = data.sats || {};
		const used = sats.used != null ? sats.used : data.satsUsed;
		const vis = sats.visible != null ? sats.visible : data.satsVisible;
		const t = data.time || {};
		const iso = t.iso != null ? t.iso : data.isoTime;
		const date = t.date != null ? t.date : data.date;
		const utc = t.utc != null ? t.utc : data.utcTime;
		const timeText = iso || (date && utc ? `${date} ${utc}` : "—");
		const mapLink =
			data.fix && lat && lon
				? '<a href="https://www.openstreetmap.org/?mlat=' +
					esc(String(lat)) +
					"&mlon=" +
					esc(String(lon)) +
					"#map=16/" +
					esc(String(lat)) +
					"/" +
					esc(String(lon)) +
					'" target="_blank" rel="noopener">OpenStreetMap</a>'
				: "—";
		const rows = [
			["GNSS", fixBadge + (data.powered ? " (powered)" : "")],
			["Satellites", `${esc(String(used))} used / ${esc(String(vis))} visible`],
			["Latitude", data.fix ? esc(String(lat)) : "—"],
			["Longitude", data.fix ? esc(String(lon)) : "—"],
			["Altitude", data.fix ? `${esc(String(alt))} m` : "—"],
			["Speed", `${esc(String(data.speed != null ? data.speed : 0))} km/h`],
			["Course", `${esc(String(data.course != null ? data.course : 0))}°`],
			["UTC", esc(timeText) || "—"],
			["Map", mapLink],
		];
		const rowsHtml = rows
			.map(([l, v]) => `<dt>${esc(l)}</dt><dd>${v}</dd>`)
			.join("");
		return `<dl class="status">${rowsHtml}</dl>`;
	}
	// #endregion FUNC_renderGpsStatus

	// #region FUNC_loadGpsStatus
	// PURPOSE: Polls /api/gps/status and renders the GNSS block.
	async function loadGpsStatus() {
		const el = document.getElementById("gps-status");
		if (!el) return;
		try {
			const { response, payload } = await api(GPS_STATUS_PATH);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok && payload) el.innerHTML = renderGpsStatus(payload);
		} catch (_error) {
			// keep last
		}
	}
	// #endregion FUNC_loadGpsStatus

	function networkPickerHtml() {
		let html =
			'<button type="button" id="scan-button">Scan nearby networks</button>';
		if (networks) {
			const options = networks
				.map((network) => {
					const label = `${esc(network.ssid)} (${network.rssi_dbm} dBm)`;
					return `<option value="${esc(network.ssid)}">${label}</option>`;
				})
				.join("");
			html +=
				'<label>Detected network <select id="network-select">' +
				`<option value="">Enter SSID manually below</option>${options}</select></label>`;
		}
		return html;
	}

	function renderNetworkPicker() {
		const picker = document.getElementById("network-picker");
		if (!picker) {
			return;
		}
		picker.innerHTML = networkPickerHtml();
		document.getElementById("scan-button").addEventListener("click", runScan);
		const select = document.getElementById("network-select");
		if (select) {
			select.addEventListener("change", () => {
				const ssidInput = document.getElementById("ssid-input");
				if (select.value && ssidInput) {
					ssidInput.value = select.value;
				}
			});
		}
	}

	function setupHtml(status) {
		return `
      <p>Device Wi-Fi MAC: <strong>${esc(status.mac)}</strong></p>
      <p>Connect this device to a WPA2/WPA3-Personal Wi-Fi network.
      This setup page is open only until a successful configuration is saved.</p>
      <form id="setup-form">
        <fieldset>
          <legend>Wi-Fi network</legend>
          <div id="network-picker"></div>
          <label>SSID (manual or hidden network)
            <input maxlength="32" id="ssid-input" name="ssid" autocomplete="off">
          </label>
          <label>Wi-Fi password
            <input required minlength="8" maxlength="63" name="wifi_password" type="password" autocomplete="new-password">
          </label>
        </fieldset>
        <fieldset>
          <legend>Administrator password</legend>
          <p class="hint">8–63 printable ASCII characters. It also protects the fallback Wi-Fi AP.</p>
          <label>Password
            <input required minlength="8" maxlength="63" name="admin_password" type="password" autocomplete="new-password">
          </label>
          <label>Confirm password
            <input required minlength="8" maxlength="63" name="admin_password_confirm" type="password" autocomplete="new-password">
          </label>
        </fieldset>
        <button type="submit">Test and save</button>
      </form>`;
	}

	function configHtml() {
		return `
      <h2>Status</h2>
      <div id="status"></div>
      <h2>Internal modem (SIM7670G)</h2>
      <div id="modem-status" class="hint">Loading modem status…</div>
      <h2>GPS / GNSS (SIM7670G)</h2>
      <div id="gps-status" class="hint">Loading GNSS status…</div>
      <p id="gps-config-state" class="hint"></p>
      <form id="gps-form">
        <fieldset>
          <legend>GNSS polling</legend>
          <label class="checkbox">Enable GNSS polling
            <input type="checkbox" name="enabled">
          </label>
          <label>Poll interval (seconds)
            <input type="number" name="poll_interval" min="5" max="300" value="60">
          </label>
          <p class="hint">Active antenna etecl25t6a needs sky view. Default 60 s, 5–300 s. When fix is available the device clock is synced to GPS UTC.</p>
        </fieldset>
        <button type="submit">Save GPS settings</button>
      </form>
      <h2>SMS source: Internal modem (SIM7670G)</h2>
      <p id="modem-source-state" class="hint"></p>
      <form id="modem-source-form">
        <fieldset>
          <legend>Internal modem</legend>
          <label class="checkbox">Forward SMS to e-mail
            <input type="checkbox" name="enabled">
          </label>
          <label>Poll interval (seconds)
            <input type="number" name="poll_interval" min="5" max="300" value="15">
          </label>
          <label>Phone number or alias
            <input maxlength="31" name="label" placeholder="For example, +79990000000 (modem)" autocomplete="off">
          </label>
          <p class="hint">Shown in forwarded emails as the "Received on" line, optional. Poll interval 5–300 s, default 15 s. Storage ME (modem flash).</p>
        </fieldset>
        <button type="submit">Save settings</button>
      </form>
      <p id="modem-source-status" class="hint"></p>
      <h2>Change Wi-Fi network</h2>
      <form id="network-form">
        <fieldset>
          <legend>New profile</legend>
          <div id="network-picker"></div>
          <label>SSID (manual or hidden network)
            <input required maxlength="32" id="ssid-input" name="ssid" autocomplete="off">
          </label>
          <label>Wi-Fi password
            <input required minlength="8" maxlength="63" name="wifi_password" type="password" autocomplete="new-password">
          </label>
        </fieldset>
        <button type="submit">Test and save</button>
      </form>
      <h2>Email delivery (SMTP)</h2>
      <p id="smtp-config-state" class="hint"></p>
      <form id="smtp-form">
        <fieldset>
          <legend>Server</legend>
          <label>Host
            <input required maxlength="127" name="host" autocomplete="off">
          </label>
          <label>Port
            <input type="number" min="1" max="65535" name="port" value="587">
          </label>
          <label>Security
            <select name="security">
              <option value="starttls">STARTTLS (port 587)</option>
              <option value="implicit">Implicit TLS (port 465)</option>
            </select>
          </label>
          <label>Username
            <input required maxlength="127" name="username" autocomplete="off">
          </label>
          <label>SMTP password
            <input maxlength="95" name="password" type="password" autocomplete="new-password">
          </label>
        </fieldset>
        <fieldset>
          <legend>Message</legend>
          <label>From address
            <input required maxlength="127" name="from" type="email" autocomplete="off">
          </label>
          <label>Recipient address
            <input required maxlength="127" name="recipient" type="email" autocomplete="off">
          </label>
        </fieldset>
        <button type="submit">Save settings</button>
        <button type="button" id="smtp-test-button">Send test email</button>
      </form>
      <p id="smtp-test-status" class="hint"></p>
      <h2>SMS source: ZTE modem</h2>
      <p id="zte-config-state" class="hint"></p>
      <form id="zte-form">
        <fieldset>
          <legend>ZTE MF79RU (HiLink)</legend>
          <label class="checkbox">Enable polling
            <input type="checkbox" name="enabled">
          </label>
          <label>Host
            <input required maxlength="63" name="host" placeholder="192.168.0.1" autocomplete="off">
          </label>
          <label>Modem web password
            <input maxlength="63" name="password" type="password" autocomplete="new-password">
          </label>
          <label>Poll interval (seconds)
            <input type="number" name="poll_interval" min="5" max="300" value="15">
          </label>
          <label>Phone number or alias
            <input maxlength="31" name="label" placeholder="For example, +79990000000 (ZTE)" autocomplete="off">
          </label>
          <p class="hint">Shown in forwarded emails as the "Received on" line, so you can tell which SIM the message arrived on. Optional. Poll interval 5–300 s, default 15 s.</p>
        </fieldset>
        <button type="submit">Save settings</button>
        <button type="button" id="zte-test-button">Test connection</button>
      </form>
      <p id="zte-test-status" class="hint"></p>
      <h2>Send SMS via ZTE MF79RU</h2>
      <p class="hint">Sends through the ZTE modem. Up to 335 characters; the recipient is a phone number.</p>
      <form id="zte-send-form">
        <fieldset>
          <legend>ZTE message</legend>
          <label>To (phone number)
            <input required maxlength="20" name="to" inputmode="tel" placeholder="+79990000000" autocomplete="off">
          </label>
          <label>Message
            <textarea required maxlength="335" name="text" rows="4"></textarea>
          </label>
        </fieldset>
        <button type="submit">Send via ZTE</button>
      </form>
      <p id="zte-send-status" class="hint"></p>
      <h2>Send SMS via Internal modem (SIM7670G)</h2>
      <p class="hint">Sends through the internal SIM7670G via AT+CMGS/UCS2. Up to 335 characters.</p>
      <form id="modem-send-form">
        <fieldset>
          <legend>Internal modem message</legend>
          <label>To (phone number)
            <input required maxlength="20" name="to" inputmode="tel" placeholder="+79990000000" autocomplete="off">
          </label>
          <label>Message
            <textarea required maxlength="335" name="text" rows="4"></textarea>
          </label>
        </fieldset>
        <button type="submit">Send via SIM7670G</button>
      </form>
      <p id="modem-send-status" class="hint"></p>
      <h2>Change administrator password</h2>
      <form id="password-form">
        <label>Current password
          <input required name="current_password" type="password" autocomplete="current-password">
        </label>
        <label>New password
          <input required minlength="8" maxlength="63" name="new_password" type="password" autocomplete="new-password">
        </label>
        <label>Confirm new password
          <input required minlength="8" maxlength="63" name="new_password_confirm" type="password" autocomplete="new-password">
        </label>
        <button type="submit">Change password</button>
      </form>`;
	}

	function renderSetup(status) {
		stopStatusTimer();
		stopModemTimer();
		stopGpsTimer();
		appRoot.innerHTML = setupHtml(status);
		renderNetworkPicker();
		document
			.getElementById("setup-form")
			.addEventListener("submit", submitSetup);
	}

	function renderConfig(status) {
		appRoot.innerHTML = configHtml();
		document.getElementById("status").innerHTML = statusHtml(status);
		renderNetworkPicker();
		document
			.getElementById("network-form")
			.addEventListener("submit", submitNetworkChange);
		document
			.getElementById("smtp-form")
			.addEventListener("submit", submitSmtpSave);
		document
			.getElementById("smtp-test-button")
			.addEventListener("click", startSmtpTest);
		document
			.getElementById("smtp-form")
			.elements.security.addEventListener("change", onSmtpSecurityChange);
		document
			.getElementById("zte-form")
			.addEventListener("submit", submitZteSave);
		document
			.getElementById("zte-test-button")
			.addEventListener("click", startZteTest);
		document
			.getElementById("modem-source-form")
			.addEventListener("submit", submitModemSourceSave);
		document
			.getElementById("zte-send-form")
			.addEventListener("submit", submitZteSend);
		document
			.getElementById("modem-send-form")
			.addEventListener("submit", submitModemSend);
		document
			.getElementById("password-form")
			.addEventListener("submit", submitPasswordChange);
		document
			.getElementById("gps-form")
			.addEventListener("submit", submitGpsSave);
		loadSmtpSettings();
		loadZteSettings();
		loadModemSourceSettings();
		loadModemStatus();
		loadGpsSettings();
		loadGpsStatus();
		startStatusTimer();
		startModemTimer();
		startGpsTimer();
	}

	function renderAuthRequired() {
		stopStatusTimer();
		stopModemTimer();
		stopGpsTimer();
		stopAsyncTestTimer();
		appRoot.innerHTML = `
      <p>Authentication is required. Reload the page and sign in as <strong>admin</strong>.</p>
      <button type="button" id="reload-button">Reload</button>`;
		document
			.getElementById("reload-button")
			.addEventListener("click", () => window.location.reload());
	}

	async function runScan() {
		if (busy) {
			return;
		}
		setBusy(true);
		setBanner("ok", "Scanning nearby networks…");
		const scanButton = document.getElementById("scan-button");
		if (scanButton) {
			scanButton.textContent = "Scanning…";
		}
		try {
			const { response, payload } = await api("/api/scan");
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok && payload && Array.isArray(payload.networks)) {
				networks = payload.networks;
				setBanner("", "");
			} else {
				setBanner("error", "The network scan failed.");
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached for the scan.");
		} finally {
			renderNetworkPicker();
			setBusy(false);
		}
	}

	async function submitSetup(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		const form = event.target;
		const fields = {
			ssid: form.elements.ssid.value.trim(),
			wifi_password: form.elements.wifi_password.value,
			admin_password: form.elements.admin_password.value,
			admin_password_confirm: form.elements.admin_password_confirm.value,
		};
		if (fields.ssid.length === 0) {
			setBanner("error", "Enter an SSID.");
			return;
		}
		if (fields.admin_password !== fields.admin_password_confirm) {
			setBanner("error", "Administrator passwords do not match.");
			return;
		}
		setBusy(true);
		setBanner("ok", "Testing the Wi-Fi connection, this can take up to 30 s…");
		await submitCredentials("/api/setup", fields);
	}

	async function submitNetworkChange(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		const form = event.target;
		const fields = {
			ssid: form.elements.ssid.value.trim(),
			wifi_password: form.elements.wifi_password.value,
		};
		if (fields.ssid.length === 0) {
			setBanner("error", "Enter an SSID.");
			return;
		}
		setBusy(true);
		setBanner("ok", "Testing the Wi-Fi connection, this can take up to 30 s…");
		await submitCredentials("/api/network", fields);
	}

	async function submitCredentials(path, fields) {
		try {
			const { response, payload } = await postForm(path, fields);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "Configuration saved.");
				if (path === "/api/setup") {
					window.setTimeout(() => window.location.reload(), 3000);
				} else {
					await refreshStatusBlock();
				}
			} else {
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}

	async function submitPasswordChange(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		const form = event.target;
		const fields = {
			current_password: form.elements.current_password.value,
			new_password: form.elements.new_password.value,
			new_password_confirm: form.elements.new_password_confirm.value,
		};
		if (fields.new_password !== fields.new_password_confirm) {
			setBanner("error", "New passwords do not match.");
			return;
		}
		setBusy(true);
		try {
			const { response, payload } = await postForm("/api/password", fields);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "Password changed.");
				stopStatusTimer();
			} else {
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}

	function smtpFormFields(form) {
		return {
			host: form.elements.host.value.trim(),
			port: form.elements.port.value.trim(),
			security: form.elements.security.value,
			username: form.elements.username.value.trim(),
			password: form.elements.password.value,
			from: form.elements.from.value.trim(),
			recipient: form.elements.recipient.value.trim(),
		};
	}

	function onSmtpSecurityChange(event) {
		const defaults = { starttls: "587", implicit: "465" };
		const others = { starttls: "465", implicit: "587" };
		const mode = event.target.value;
		const portInput = document.getElementById("smtp-form").elements.port;
		if (!portInput.value || portInput.value === others[mode]) {
			portInput.value = defaults[mode];
		}
	}

	async function loadSmtpSettings() {
		try {
			const { response, payload } = await api("/api/smtp");
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			const form = document.getElementById("smtp-form");
			if (response.ok && payload && form) {
				form.elements.host.value = payload.host || "";
				if (payload.port) {
					form.elements.port.value = String(payload.port);
				}
				if (payload.security) {
					form.elements.security.value = payload.security;
				}
				form.elements.username.value = payload.username || "";
				form.elements.from.value = payload.from || "";
				form.elements.recipient.value = payload.recipient || "";
				form.elements.password.value = "";
				form.elements.password.placeholder = payload.password_set
					? "Unchanged (a password is saved)"
					: "";
			}
			const state = document.getElementById("smtp-config-state");
			if (state && payload) {
				state.textContent = payload.present
					? "SMTP delivery is configured."
					: "SMTP delivery is not configured yet.";
			}
		} catch (_error) {
			// Prefill is optional; the form stays usable with empty defaults.
		}
	}

	async function submitSmtpSave(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		setBusy(true);
		try {
			const { response, payload } = await postForm(
				"/api/smtp",
				smtpFormFields(event.target),
			);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "SMTP settings saved.");
				await loadSmtpSettings();
			} else {
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}

	function stopAsyncTestTimer() {
		if (asyncTestTimer !== null) {
			window.clearInterval(asyncTestTimer);
			asyncTestTimer = null;
		}
	}

	// Shared flow for the one-shot async routes (SMTP, ZTE, SMS send): POSTs
	// the form to startPath, then polls statusPath until done and reports the
	// result; onDone(succeeded) runs after the terminal sample.
	async function startAsyncTest(options) {
		const { startPath, statusPath, fields, statusEl, busyMessage, onDone } =
			options;
		if (busy) {
			return false;
		}
		setBusy(true);
		if (statusEl) {
			statusEl.textContent = busyMessage;
		}
		setBanner("ok", busyMessage);
		try {
			const { response, payload } = await postForm(startPath, fields);
			if (response.status === 401) {
				renderAuthRequired();
				return true; // The caller must not clear busy: a fresh page took over.
			}
			if (!response.ok) {
				setBanner(
					"error",
					payload?.error || "The operation could not be started.",
				);
				if (statusEl) {
					statusEl.textContent = "";
				}
				setBusy(false);
				return false;
			}
			pollAsyncTest(statusPath, statusEl, onDone);
			return true;
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
			if (statusEl) {
				statusEl.textContent = "";
			}
			setBusy(false);
			return false;
		}
	}

	function pollAsyncTest(path, statusEl, onDone) {
		stopAsyncTestTimer();
		asyncTestTimer = window.setInterval(async () => {
			try {
				const { response, payload } = await api(path);
				if (response.status === 401) {
					stopAsyncTestTimer();
					renderAuthRequired();
					setBusy(false);
					return;
				}
				if (!response.ok || !payload || payload.running || !payload.done) {
					return;
				}
				stopAsyncTestTimer();
				setBusy(false);
				const succeeded = payload.result === "success";
				if (statusEl) {
					statusEl.textContent = payload.message || "";
				}
				setBanner(succeeded ? "ok" : "error", payload.message || "");
				if (onDone) {
					onDone(succeeded);
				}
			} catch (_error) {
				// The device may be busy inside a network dialog; keep polling.
			}
		}, ASYNC_TEST_POLL_MS);
	}

	async function startSmtpTest() {
		await startAsyncTest({
			startPath: "/api/smtp/test",
			statusPath: "/api/smtp/test",
			fields: smtpFormFields(document.getElementById("smtp-form")),
			statusEl: document.getElementById("smtp-test-status"),
			busyMessage: "Sending the test email…",
		});
	}

	async function loadZteSettings() {
		try {
			const { response, payload } = await api("/api/zte");
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			const form = document.getElementById("zte-form");
			if (response.ok && payload && form) {
				form.elements.host.value = payload.host || "";
				form.elements.enabled.checked = !!payload.enabled;
				form.elements.poll_interval.value =
					payload.poll_interval != null ? String(payload.poll_interval) : "15";
				form.elements.label.value = payload.label || "";
				form.elements.password.value = "";
				form.elements.password.placeholder = payload.password_set
					? "Unchanged (a password is saved)"
					: "";
				const state = document.getElementById("zte-config-state");
				if (state) {
					if (!payload.present) {
						state.textContent = "The ZTE modem is not configured yet.";
					} else if (payload.enabled) {
						state.textContent =
							"Polling is enabled." +
							(payload.last_status ? ` Last poll: ${payload.last_status}` : "");
					} else {
						state.textContent = "Configured, polling disabled.";
					}
				}
			}
		} catch (_error) {
			// Prefill is optional; the form stays usable with empty defaults.
		}
	}

	async function submitZteSave(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		setBusy(true);
		try {
			const { response, payload } = await postForm(
				"/api/zte",
				zteFormFields(event.target),
			);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "ZTE settings saved.");
				await loadZteSettings();
			} else {
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}

	function zteFormFields(form) {
		return {
			enabled: form.elements.enabled.checked ? "1" : "0",
			host: form.elements.host.value.trim(),
			password: form.elements.password.value,
			poll_interval: form.elements.poll_interval.value.trim(),
			label: form.elements.label.value.trim(),
		};
	}

	async function startZteTest() {
		await startAsyncTest({
			startPath: "/api/zte/test",
			statusPath: "/api/zte/test",
			fields: zteFormFields(document.getElementById("zte-form")),
			statusEl: document.getElementById("zte-test-status"),
			busyMessage: "Testing the modem connection…",
		});
	}

	// #region FUNC_loadModemSourceSettings
	// PURPOSE: Loads /api/modem/source and prefills the internal modem form.
	async function loadModemSourceSettings() {
		try {
			const { response, payload } = await api("/api/modem/source");
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			const form = document.getElementById("modem-source-form");
			if (response.ok && payload && form) {
				form.elements.enabled.checked = !!payload.enabled;
				form.elements.poll_interval.value =
					payload.poll_interval != null ? String(payload.poll_interval) : "15";
				form.elements.label.value = payload.label || "";
				const state = document.getElementById("modem-source-state");
				if (state) {
					if (!payload.present) {
						state.textContent =
							"The internal modem source is not configured yet.";
					} else if (payload.enabled) {
						state.textContent =
							"Forwarding is enabled." +
							(payload.last_status ? ` Last poll: ${payload.last_status}` : "");
					} else {
						state.textContent = "Configured, forwarding disabled.";
					}
				}
			}
		} catch (_error) {
			// Prefill is optional; the form stays usable with empty defaults.
		}
	}
	// #endregion FUNC_loadModemSourceSettings

	// #region FUNC_submitModemSourceSave
	// PURPOSE: Validates and saves the internal modem source profile.
	async function submitModemSourceSave(event) {
		event.preventDefault();
		if (busy) {
			return;
		}
		setBusy(true);
		try {
			const { response, payload } = await postForm(
				"/api/modem/source",
				modemSourceFormFields(event.target),
			);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "Modem source settings saved.");
				await loadModemSourceSettings();
			} else {
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
			}
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}
	// #endregion FUNC_submitModemSourceSave

	// #region FUNC_loadGpsSettings
	// PURPOSE: Loads /api/gps and prefills the GNSS form.
	async function loadGpsSettings() {
		try {
			const { response, payload } = await api("/api/gps");
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			const form = document.getElementById("gps-form");
			if (response.ok && payload && form) {
				form.elements.enabled.checked = !!payload.enabled;
				form.elements.poll_interval.value =
					payload.poll_interval != null ? String(payload.poll_interval) : "60";
				const state = document.getElementById("gps-config-state");
				if (state) {
					if (!payload.present)
						state.textContent =
							"GNSS is not configured yet (default disabled, 60 s).";
					else if (payload.enabled)
						state.textContent =
							"Polling enabled." +
							(payload.last_status ? ` ${payload.last_status}` : "");
					else state.textContent = "Configured, polling disabled.";
				}
			}
		} catch (_error) {
			// optional
		}
	}
	// #endregion FUNC_loadGpsSettings

	// #region FUNC_submitGpsSave
	async function submitGpsSave(event) {
		event.preventDefault();
		if (busy) return;
		setBusy(true);
		try {
			const { response, payload } = await postForm(
				"/api/gps",
				gpsFormFields(event.target),
			);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			if (response.ok) {
				setBanner("ok", payload?.message || "GPS settings saved.");
				await loadGpsSettings();
			} else
				setBanner(
					"error",
					payload?.error || `Request failed (${response.status}).`,
				);
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
		} finally {
			setBusy(false);
		}
	}
	// #endregion FUNC_submitGpsSave

	function gpsFormFields(form) {
		return {
			enabled: form.elements.enabled.checked ? "1" : "0",
			poll_interval: form.elements.poll_interval.value.trim(),
		};
	}

	function modemSourceFormFields(form) {
		return {
			enabled: form.elements.enabled.checked ? "1" : "0",
			poll_interval: form.elements.poll_interval.value.trim(),
			label: form.elements.label.value.trim(),
		};
	}

	function zteSendFields(form) {
		return {
			to: form.elements.to.value.trim(),
			text: form.elements.text.value,
		};
	}
	function modemSendFields(form) {
		return {
			to: form.elements.to.value.trim(),
			text: form.elements.text.value,
		};
	}
	// #region FUNC_submitZteSend
	// PURPOSE: Validates and sends an SMS via the ZTE modem (separate form).
	async function submitZteSend(event) {
		event.preventDefault();
		if (busy) return;
		const fields = zteSendFields(event.target);
		if (!/^\+?\d{3,20}$/.test(fields.to)) {
			setBanner(
				"error",
				"Enter a phone number of 3–20 digits, optionally starting with +.",
			);
			return;
		}
		if (fields.text.length === 0) {
			setBanner("error", "Enter the message text.");
			return;
		}
		await startAsyncTest({
			startPath: "/api/zte/send",
			statusPath: "/api/zte/send",
			fields: fields,
			statusEl: document.getElementById("zte-send-status"),
			busyMessage: "Sending the SMS via ZTE…",
			onDone: (succeeded) => {
				if (!succeeded) return;
				const form = document.getElementById("zte-send-form");
				if (form) {
					form.elements.to.value = "";
					form.elements.text.value = "";
				}
			},
		});
	}
	// #endregion FUNC_submitZteSend
	// #region FUNC_submitModemSend
	// PURPOSE: Validates and sends an SMS via the internal SIM7670G (separate form).
	async function submitModemSend(event) {
		event.preventDefault();
		if (busy) return;
		const fields = modemSendFields(event.target);
		if (!/^\+?\d{3,20}$/.test(fields.to)) {
			setBanner(
				"error",
				"Enter a phone number of 3–20 digits, optionally starting with +.",
			);
			return;
		}
		if (fields.text.length === 0) {
			setBanner("error", "Enter the message text.");
			return;
		}
		await startAsyncTest({
			startPath: "/api/modem/send",
			statusPath: "/api/modem/send",
			fields: fields,
			statusEl: document.getElementById("modem-send-status"),
			busyMessage: "Sending the SMS via SIM7670G…",
			onDone: (succeeded) => {
				if (!succeeded) return;
				const form = document.getElementById("modem-send-form");
				if (form) {
					form.elements.to.value = "";
					form.elements.text.value = "";
				}
			},
		});
	}
	// #endregion FUNC_submitModemSend

	function startStatusTimer() {
		stopStatusTimer();
		statusTimer = window.setInterval(refreshStatusBlock, STATUS_INTERVAL_MS);
	}

	function stopStatusTimer() {
		if (statusTimer !== null) {
			window.clearInterval(statusTimer);
			statusTimer = null;
		}
	}

	function startModemTimer() {
		stopModemTimer();
		modemTimer = window.setInterval(loadModemStatus, MODEM_INTERVAL_MS);
	}

	function stopModemTimer() {
		if (modemTimer !== null) {
			window.clearInterval(modemTimer);
			modemTimer = null;
		}
	}

	function startGpsTimer() {
		stopGpsTimer();
		gpsTimer = window.setInterval(loadGpsStatus, GPS_INTERVAL_MS);
	}

	function stopGpsTimer() {
		if (gpsTimer !== null) {
			window.clearInterval(gpsTimer);
			gpsTimer = null;
		}
	}

	async function refreshStatusBlock() {
		if (busy || document.hidden) {
			return;
		}
		try {
			const { response, payload } = await api(STATUS_PATH);
			if (response.status === 401) {
				renderAuthRequired();
				return;
			}
			const statusBlock = document.getElementById("status");
			if (response.ok && payload && statusBlock) {
				statusBlock.innerHTML = statusHtml(payload);
			}
		} catch (_error) {
			// Transient reachability loss; the next interval retries.
		}
	}

	async function loadApp() {
		let result;
		try {
			result = await api(STATUS_PATH);
		} catch (_error) {
			setBanner("error", "The device could not be reached.");
			return;
		}
		const { response, payload } = result;
		if (response.status === 401) {
			renderAuthRequired();
			return;
		}
		if (!response.ok || !payload) {
			setBanner("error", "Device status is unavailable.");
			return;
		}
		if (payload.setup_required) {
			renderSetup(payload);
		} else {
			renderConfig(payload);
		}
	}

	loadApp();
})();
