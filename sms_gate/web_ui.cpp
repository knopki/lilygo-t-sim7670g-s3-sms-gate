// #region MODULE_CONTRACT
// PURPOSE: Renders the embedded, dependency-free configuration pages without
// mixing presentation markup into Wi-Fi and storage control flow.
// INVARIANTS: Dynamic values are escaped before entering HTML; passwords are
// never rendered after submission.
// #endregion MODULE_CONTRACT

#include "web_ui.h"

namespace {

String renderDocument(const String& title, const String& body) {
  String page;
  page.reserve(title.length() + body.length() + 900);
  page += F("<!doctype html><html lang='en'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>");
  page += escapeHtml(title);
  page +=
      F("</title><style>body{font:16px "
        "system-ui,sans-serif;max-width:42rem;margin:2rem auto;padding:0 "
        "1rem;line-height:1.45}");
  page +=
      F("fieldset{margin:1.25rem 0;padding:1rem}label{display:block;margin:.7rem "
        "0}.hint{color:#555}.error{color:#9b1c1c;font-weight:600}");
  page +=
      F("input{display:block;box-sizing:border-box;width:100%;padding:."
        "5rem;margin-top:.2rem}button{padding:.55rem "
        ".9rem}</style></head><body><h1>SMS Gate</h1>");
  page += body;
  page += F("</body></html>");
  return page;
}

void appendError(String& body, const String& error) {
  if (error.length() == 0) {
    return;
  }
  body += F("<p class='error'>");
  body += escapeHtml(error);
  body += F("</p>");
}

}  // namespace

String escapeHtml(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t index = 0; index < value.length(); ++index) {
    switch (value[index]) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      case '\'':
        escaped += F("&#39;");
        break;
      default:
        escaped += value[index];
        break;
    }
  }
  return escaped;
}

String renderSetupPage(const String& networkOptions, const String& error,
                       const String& stationMacAddress) {
  String body;
  appendError(body, error);
  body += F("<p>Device Wi-Fi MAC: <strong>");
  body += escapeHtml(stationMacAddress);
  body +=
      F("</strong></p><p>Connect this device to a WPA2/WPA3-Personal Wi-Fi "
        "network. This setup page is open only until a successful "
        "configuration is saved.</p>");
  body +=
      F("<form method='post' action='/setup'><fieldset><legend>Wi-Fi "
        "network</legend>");
  if (networkOptions.length() > 0) {
    body +=
        F("<label>Detected network <select name='scanned_ssid'><option "
          "value=''>Enter SSID manually below</option>");
    body += networkOptions;
    body += F("</select></label>");
  } else {
    body += F("<p><a href='/setup?scan=1'>Scan nearby networks</a></p>");
  }
  body +=
      F("<label>SSID (manual or hidden network) <input maxlength='32' "
        "name='ssid' autocomplete='off'></label><label>Wi-Fi password "
        "<input required minlength='8' maxlength='63' name='wifi_password' "
        "type='password' autocomplete='new-password'></label>");
  body +=
      F("</fieldset><fieldset><legend>Administrator password</legend><p "
        "class='hint'>8–63 printable ASCII characters. It also protects "
        "the fallback Wi-Fi AP.</p>");
  body +=
      F("<label>Password <input required minlength='8' maxlength='63' "
        "name='admin_password' type='password' "
        "autocomplete='new-password'></label>");
  body +=
      F("<label>Confirm password <input required minlength='8' "
        "maxlength='63' name='admin_password_confirm' type='password' "
        "autocomplete='new-password'></label>");
  body += F("</fieldset><button type='submit'>Test and save</button></form>");
  return renderDocument(F("Initial setup"), body);
}

String renderConfigPage(const RuntimeConfig& config, const WebStatus& status,
                        const String& networkOptions, const String& message, const String& error) {
  String body;
  if (message.length() > 0) {
    body += F("<p>");
    body += escapeHtml(message);
    body += F("</p>");
  }
  appendError(body, error.length() > 0 ? error : status.lastError);
  body += F("<h2>Status</h2><dl><dt>Mode</dt><dd>");
  body += escapeHtml(status.mode);
  body += F("</dd><dt>Configured SSID</dt><dd>");
  body += escapeHtml(config.ssid);
  body += F("</dd><dt>Station IP</dt><dd>");
  body += escapeHtml(status.stationIp);
  body += F("</dd><dt>Device MAC</dt><dd>");
  body += escapeHtml(status.macAddress);
  body += F("</dd><dt>RSSI</dt><dd>");
  body += escapeHtml(status.rssi);
  body += F("</dd><dt>mDNS</dt><dd>http://");
  body += escapeHtml(status.mdnsHostname);
  body += F(".local</dd></dl>");
  body += F("<h2>Change Wi-Fi network</h2><form method='post' action='/network'>");
  if (networkOptions.length() > 0) {
    body +=
        F("<label>Detected network <select name='scanned_ssid'><option "
          "value=''>Keep or enter SSID manually below</option>");
    body += networkOptions;
    body += F("</select></label>");
  } else {
    body += F("<p><a href='/?scan=1'>Scan nearby networks</a></p>");
  }
  body +=
      F("<label>SSID (manual or hidden network) <input required "
        "maxlength='32' name='ssid' value='");
  body += escapeHtml(config.ssid);
  body +=
      F("' autocomplete='off'></label><label>Wi-Fi password <input "
        "required minlength='8' maxlength='63' name='wifi_password' "
        "type='password' autocomplete='new-password'></label><button "
        "type='submit'>Test and save</button></form>");
  body +=
      F("<h2>Change administrator password</h2><form method='post' "
        "action='/password'><label>Current password <input required "
        "name='current_password' type='password' "
        "autocomplete='current-password'></label><label>New password <input "
        "required minlength='8' maxlength='63' name='new_password' "
        "type='password' autocomplete='new-password'></label><label>Confirm "
        "new password <input required minlength='8' maxlength='63' "
        "name='new_password_confirm' type='password' "
        "autocomplete='new-password'></label><button type='submit'>Change "
        "password</button></form>");
  return renderDocument(F("Configuration"), body);
}
