// Client-rendered UI for the SMS Gate configuration interface.
// The firmware serves this bundle plus a small JSON API; all markup is
// rendered in the browser.
'use strict';

(function () {
  const appRoot = document.getElementById('app');
  const banner = document.getElementById('banner');

  const MODE_LABELS = {
    initial: 'Initial setup',
    connecting: 'Connecting…',
    sta: 'Station (connected)',
    fallback_ap: 'Fallback access point',
  };

  const STATUS_PATH = '/api/status';
  const STATUS_INTERVAL_MS = 5000;
  const ASYNC_TEST_POLL_MS = 1500;

  let networks = null;
  let busy = false;
  let statusTimer = null;
  let asyncTestTimer = null;

  const esc = (value) =>
    String(value).replace(/[&<>"']/g, (ch) => ({
      '&': '&amp;',
      '<': '&lt;',
      '>': '&gt;',
      '"': '&quot;',
      "'": '&#39;',
    })[ch]);

  function setBusy(value) {
    busy = value;
    for (const button of document.querySelectorAll('button')) {
      button.disabled = value;
    }
  }

  function setBanner(kind, text) {
    if (!text) {
      banner.hidden = true;
      banner.textContent = '';
      return;
    }
    banner.className = 'banner ' + kind;
    banner.textContent = text;
    banner.hidden = false;
  }

  async function api(path, options) {
    const response = await fetch(path, options);
    let payload = null;
    try {
      payload = await response.json();
    } catch (error) {
      payload = null;
    }
    return { response, payload };
  }

  function postForm(path, fields) {
    return api(path, { method: 'POST', body: new URLSearchParams(fields) });
  }

  function statusHtml(status) {
    const rows = [
      ['Mode', MODE_LABELS[status.mode] || status.mode],
      ['Configured network', status.ssid || '—'],
      ['Station IP', status.station_ip || '—'],
      ['Device MAC', status.mac || '—'],
      ['Signal', status.rssi_dbm == null ? '—' : status.rssi_dbm + ' dBm'],
      ['Address', status.mdns_hostname ? 'http://' + status.mdns_hostname + '.local' : '—'],
    ];
    const rowsHtml = rows
      .map(([label, value]) => `<dt>${label}</dt><dd>${esc(value)}</dd>`)
      .join('');
    return `<dl class="status">${rowsHtml}</dl>`;
  }

  function networkPickerHtml() {
    let html = '<button type="button" id="scan-button">Scan nearby networks</button>';
    if (networks) {
      const options = networks
        .map((network) => {
          const label = `${esc(network.ssid)} (${network.rssi_dbm} dBm)`;
          return `<option value="${esc(network.ssid)}">${label}</option>`;
        })
        .join('');
      html += '<label>Detected network <select id="network-select">' +
        `<option value="">Enter SSID manually below</option>${options}</select></label>`;
    }
    return html;
  }

  function renderNetworkPicker() {
    const picker = document.getElementById('network-picker');
    if (!picker) {
      return;
    }
    picker.innerHTML = networkPickerHtml();
    document.getElementById('scan-button').addEventListener('click', runScan);
    const select = document.getElementById('network-select');
    if (select) {
      select.addEventListener('change', () => {
        const ssidInput = document.getElementById('ssid-input');
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
        </fieldset>
        <button type="submit">Save settings</button>
        <button type="button" id="zte-test-button">Test connection</button>
      </form>
      <p id="zte-test-status" class="hint"></p>
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
    appRoot.innerHTML = setupHtml(status);
    renderNetworkPicker();
    document.getElementById('setup-form').addEventListener('submit', submitSetup);
  }

  function renderConfig(status) {
    appRoot.innerHTML = configHtml();
    document.getElementById('status').innerHTML = statusHtml(status);
    renderNetworkPicker();
    document.getElementById('network-form').addEventListener('submit', submitNetworkChange);
    document.getElementById('smtp-form').addEventListener('submit', submitSmtpSave);
    document.getElementById('smtp-test-button').addEventListener('click', startSmtpTest);
    document.getElementById('smtp-form').elements.security.addEventListener('change', onSmtpSecurityChange);
    document.getElementById('zte-form').addEventListener('submit', submitZteSave);
    document.getElementById('zte-test-button').addEventListener('click', startZteTest);
    document.getElementById('password-form').addEventListener('submit', submitPasswordChange);
    loadSmtpSettings();
    loadZteSettings();
    startStatusTimer();
  }

  function renderAuthRequired() {
    stopStatusTimer();
    stopAsyncTestTimer();
    appRoot.innerHTML = `
      <p>Authentication is required. Reload the page and sign in as <strong>admin</strong>.</p>
      <button type="button" id="reload-button">Reload</button>`;
    document.getElementById('reload-button').addEventListener('click', () => window.location.reload());
  }

  async function runScan() {
    if (busy) {
      return;
    }
    setBusy(true);
    setBanner('ok', 'Scanning nearby networks…');
    const scanButton = document.getElementById('scan-button');
    if (scanButton) {
      scanButton.textContent = 'Scanning…';
    }
    try {
      const { response, payload } = await api('/api/scan');
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      if (response.ok && payload && Array.isArray(payload.networks)) {
        networks = payload.networks;
        setBanner('', '');
      } else {
        setBanner('error', 'The network scan failed.');
      }
    } catch (error) {
      setBanner('error', 'The device could not be reached for the scan.');
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
      setBanner('error', 'Enter an SSID.');
      return;
    }
    if (fields.admin_password !== fields.admin_password_confirm) {
      setBanner('error', 'Administrator passwords do not match.');
      return;
    }
    setBusy(true);
    setBanner('ok', 'Testing the Wi-Fi connection, this can take up to 30 s…');
    await submitCredentials('/api/setup', fields);
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
      setBanner('error', 'Enter an SSID.');
      return;
    }
    setBusy(true);
    setBanner('ok', 'Testing the Wi-Fi connection, this can take up to 30 s…');
    await submitCredentials('/api/network', fields);
  }

  async function submitCredentials(path, fields) {
    try {
      const { response, payload } = await postForm(path, fields);
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      if (response.ok) {
        setBanner('ok', (payload && payload.message) || 'Configuration saved.');
        if (path === '/api/setup') {
          window.setTimeout(() => window.location.reload(), 3000);
        } else {
          await refreshStatusBlock();
        }
      } else {
        setBanner('error', (payload && payload.error) || `Request failed (${response.status}).`);
      }
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
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
      setBanner('error', 'New passwords do not match.');
      return;
    }
    setBusy(true);
    try {
      const { response, payload } = await postForm('/api/password', fields);
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      if (response.ok) {
        setBanner('ok', (payload && payload.message) || 'Password changed.');
        stopStatusTimer();
      } else {
        setBanner('error', (payload && payload.error) || `Request failed (${response.status}).`);
      }
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
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
    const defaults = { starttls: '587', implicit: '465' };
    const others = { starttls: '465', implicit: '587' };
    const mode = event.target.value;
    const portInput = document.getElementById('smtp-form').elements.port;
    if (!portInput.value || portInput.value === others[mode]) {
      portInput.value = defaults[mode];
    }
  }

  async function loadSmtpSettings() {
    try {
      const { response, payload } = await api('/api/smtp');
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      const form = document.getElementById('smtp-form');
      if (response.ok && payload && form) {
        form.elements.host.value = payload.host || '';
        if (payload.port) {
          form.elements.port.value = String(payload.port);
        }
        if (payload.security) {
          form.elements.security.value = payload.security;
        }
        form.elements.username.value = payload.username || '';
        form.elements.from.value = payload.from || '';
        form.elements.recipient.value = payload.recipient || '';
        form.elements.password.value = '';
        form.elements.password.placeholder = payload.password_set
          ? 'Unchanged (a password is saved)'
          : '';
      }
      const state = document.getElementById('smtp-config-state');
      if (state && payload) {
        state.textContent = payload.present
          ? 'SMTP delivery is configured.'
          : 'SMTP delivery is not configured yet.';
      }
    } catch (error) {
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
      const { response, payload } = await postForm('/api/smtp', smtpFormFields(event.target));
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      if (response.ok) {
        setBanner('ok', (payload && payload.message) || 'SMTP settings saved.');
        await loadSmtpSettings();
      } else {
        setBanner('error', (payload && payload.error) || `Request failed (${response.status}).`);
      }
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
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

  // Shared flow for the one-shot async test routes (SMTP, ZTE): POSTs the
  // form, then polls the status endpoint until done and reports the result.
  async function startAsyncTest(path, fields, statusEl, busyMessage) {
    if (busy) {
      return false;
    }
    setBusy(true);
    if (statusEl) {
      statusEl.textContent = busyMessage;
    }
    setBanner('ok', busyMessage);
    try {
      const { response, payload } = await postForm(path + '/test', fields);
      if (response.status === 401) {
        renderAuthRequired();
        return true; // The caller must not clear busy: a fresh page took over.
      }
      if (!response.ok) {
        setBanner('error', (payload && payload.error) || 'The test could not be started.');
        if (statusEl) {
          statusEl.textContent = '';
        }
        setBusy(false);
        return false;
      }
      pollAsyncTest(path, statusEl);
      return true;
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
      if (statusEl) {
        statusEl.textContent = '';
      }
      setBusy(false);
      return false;
    }
  }

  function pollAsyncTest(path, statusEl) {
    stopAsyncTestTimer();
    asyncTestTimer = window.setInterval(async () => {
      try {
        const { response, payload } = await api(path + '/test');
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
        const succeeded = payload.result === 'success';
        if (statusEl) {
          statusEl.textContent = payload.message || '';
        }
        setBanner(succeeded ? 'ok' : 'error', payload.message || '');
      } catch (error) {
        // The device may be busy inside a network dialog; keep polling.
      }
    }, ASYNC_TEST_POLL_MS);
  }

  async function startSmtpTest() {
    await startAsyncTest('/api/smtp', smtpFormFields(document.getElementById('smtp-form')),
                         document.getElementById('smtp-test-status'),
                         'Sending the test email…');
  }

  async function loadZteSettings() {
    try {
      const { response, payload } = await api('/api/zte');
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      const form = document.getElementById('zte-form');
      if (response.ok && payload && form) {
        form.elements.host.value = payload.host || '';
        form.elements.enabled.checked = !!payload.enabled;
        form.elements.password.value = '';
        form.elements.password.placeholder = payload.password_set
          ? 'Unchanged (a password is saved)'
          : '';
        const state = document.getElementById('zte-config-state');
        if (state) {
          if (!payload.present) {
            state.textContent = 'The ZTE modem is not configured yet.';
          } else if (payload.enabled) {
            state.textContent = 'Polling is enabled.' +
              (payload.last_status ? ' Last poll: ' + payload.last_status : '');
          } else {
            state.textContent = 'Configured, polling disabled.';
          }
        }
      }
    } catch (error) {
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
      const { response, payload } = await postForm('/api/zte', zteFormFields(event.target));
      if (response.status === 401) {
        renderAuthRequired();
        return;
      }
      if (response.ok) {
        setBanner('ok', (payload && payload.message) || 'ZTE settings saved.');
        await loadZteSettings();
      } else {
        setBanner('error', (payload && payload.error) || `Request failed (${response.status}).`);
      }
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
    } finally {
      setBusy(false);
    }
  }

  function zteFormFields(form) {
    return {
      enabled: form.elements.enabled.checked ? '1' : '0',
      host: form.elements.host.value.trim(),
      password: form.elements.password.value,
    };
  }

  async function startZteTest() {
    const started = await startAsyncTest('/api/zte', zteFormFields(document.getElementById('zte-form')),
                                        document.getElementById('zte-test-status'),
                                        'Testing the modem connection…');
    if (!started) {
      return;
    }
  }

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
      const statusBlock = document.getElementById('status');
      if (response.ok && payload && statusBlock) {
        statusBlock.innerHTML = statusHtml(payload);
      }
    } catch (error) {
      // Transient reachability loss; the next interval retries.
    }
  }

  async function loadApp() {
    let result;
    try {
      result = await api(STATUS_PATH);
    } catch (error) {
      setBanner('error', 'The device could not be reached.');
      return;
    }
    const { response, payload } = result;
    if (response.status === 401) {
      renderAuthRequired();
      return;
    }
    if (!response.ok || !payload) {
      setBanner('error', 'Device status is unavailable.');
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
