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

  let networks = null;
  let busy = false;
  let statusTimer = null;

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
    document.getElementById('password-form').addEventListener('submit', submitPasswordChange);
    startStatusTimer();
  }

  function renderAuthRequired() {
    stopStatusTimer();
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
