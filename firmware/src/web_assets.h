#pragma once
// Web UI, embedded in the binary as PROGMEM.
//
// Deliberately dependency-free: the setup page is served from an access point
// with no internet, so nothing may be fetched from a CDN. One stylesheet, one
// script, no framework.
//
// Look: dark navy base with a purple/violet accent, glass cards, and an
// animated aurora on the setup and login screens only - the dashboard stays
// calm and orderly once the device is configured.
//
// Layout rules, so the pages stay consistent:
//   - every card is <div class="card"> with an <h3> title; forms end in one
//     <div class="actions"> row, which spaces and sizes its buttons itself -
//     no inline spacers, no inline styles;
//   - two short fields side by side go in <div class="pair">;
//   - anything that only one role needs carries data-only="meter" or
//     data-only="gateway" and is hidden for the other role (dashboard), or
//     data-roles="1 2" on the setup page;
//   - rarely needed settings go behind <details class="more">.

#include <Arduino.h>

static const char kStyleCss[] PROGMEM = R"CSS(
*,*::before,*::after{box-sizing:border-box}
:root{
  color-scheme:dark;
  --bg:#070b18; --card:rgba(255,255,255,.045); --card2:rgba(255,255,255,.075);
  --line:rgba(255,255,255,.10); --line2:rgba(255,255,255,.055); --txt:#e9edfb; --muted:#98a3c4;
  --accent:#a259ff; --accent2:#5b8cff; --ok:#4ade80; --warn:#fbbf24; --err:#f87171;
  --radius:16px; --gap:14px; --ring:0 0 0 3px rgba(162,89,255,.22);
}
[hidden]{display:none!important}
html{-webkit-text-size-adjust:100%}
html,body{margin:0;padding:0;min-height:100%}
body{
  background:var(--bg); color:var(--txt);
  font:15px/1.5 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  -webkit-font-smoothing:antialiased; padding-bottom:max(48px,env(safe-area-inset-bottom));
}
a{color:var(--accent2)}
h1,h2,h3{margin:0;font-weight:650;letter-spacing:-.01em}
h1{font-size:1.35rem;line-height:1.25}
h2{font-size:1.05rem;margin-bottom:.35rem}
h3{font-size:.78rem;color:var(--muted);text-transform:uppercase;letter-spacing:.09em;margin-bottom:.75rem}
p{margin:.35rem 0}
b{font-weight:650}
.wrap{max-width:1040px;margin:0 auto;padding:20px 18px}
.narrow{max-width:440px;margin-top:10vh}
.muted{color:var(--muted)} .small{font-size:.85rem}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.92em}

/* ── aurora: setup and login only ────────────────────────── */
.aurora{position:fixed;inset:0;overflow:hidden;z-index:-1;background:
  radial-gradient(1200px 600px at 20% -10%, #1a1440 0%, transparent 60%),
  radial-gradient(900px 500px at 90% 10%, #101a3a 0%, transparent 55%),
  linear-gradient(180deg,#070b18,#080d1f 60%,#070b18)}
.aurora span{position:absolute;display:block;border-radius:50%;filter:blur(70px);opacity:.45;will-change:transform}
.aurora .a{width:46vmax;height:46vmax;left:-10vmax;top:-12vmax;background:#7c3aed;animation:drift1 26s ease-in-out infinite}
.aurora .b{width:38vmax;height:38vmax;right:-8vmax;top:5vmax;background:#2563eb;animation:drift2 32s ease-in-out infinite}
.aurora .c{width:30vmax;height:30vmax;left:35vmax;bottom:-12vmax;background:#c026d3;animation:drift3 38s ease-in-out infinite}
@keyframes drift1{0%,100%{transform:translate3d(0,0,0) scale(1)}50%{transform:translate3d(8vmax,6vmax,0) scale(1.15)}}
@keyframes drift2{0%,100%{transform:translate3d(0,0,0) scale(1.05)}50%{transform:translate3d(-10vmax,8vmax,0) scale(.95)}}
@keyframes drift3{0%,100%{transform:translate3d(0,0,0) scale(.95)}50%{transform:translate3d(-6vmax,-6vmax,0) scale(1.2)}}
@media (prefers-reduced-motion:reduce){.aurora span{animation:none}}

/* ── header and tabs ─────────────────────────────────────── */
.top{display:flex;align-items:center;justify-content:space-between;gap:10px 16px;flex-wrap:wrap}
.top .id{display:flex;align-items:center;gap:8px 12px;flex-wrap:wrap;min-width:0}
.top h1{overflow-wrap:anywhere}
.badges{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.badge{font-size:.75rem;line-height:1.4;padding:3px 10px;border-radius:999px;border:1px solid var(--line);
  background:var(--card);color:var(--muted);white-space:nowrap}
.badge.ok{color:var(--ok);border-color:rgba(74,222,128,.35)}
.badge.err{color:var(--err);border-color:rgba(248,113,113,.35)}
.badge.warn{color:var(--warn);border-color:rgba(251,191,36,.35)}
/* One row, always: on a phone it scrolls sideways instead of wrapping a single
   tab onto a line of its own. Sticky, so switching tabs never needs a scroll. */
.tabs{position:sticky;top:0;z-index:4;display:flex;gap:6px;flex-wrap:nowrap;overflow-x:auto;
  margin:12px -18px 14px;padding:10px 18px;background:rgba(7,11,24,.88);backdrop-filter:blur(10px);
  -webkit-backdrop-filter:blur(10px);border-bottom:1px solid var(--line2);scrollbar-width:none}
.tabs::-webkit-scrollbar{display:none}
.tabs button{flex:0 0 auto;white-space:nowrap;background:transparent;border:1px solid transparent;color:var(--muted);
  padding:7px 14px;border-radius:999px;cursor:pointer;font:inherit;font-size:.9rem}
.tabs button:hover{color:var(--txt)}
.tabs button.on{background:var(--card2);border-color:var(--line);color:var(--txt)}
.panel{display:none} .panel.on{display:block}

/* ── cards ───────────────────────────────────────────────── */
.grid{display:grid;gap:var(--gap);grid-template-columns:repeat(auto-fill,minmax(min(100%,300px),1fr));align-items:start}
.grid.even{align-items:stretch}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:18px;
  backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);min-width:0}
.card.wide{grid-column:1/-1}
.steps>.card,.panel>.card{margin-bottom:var(--gap)}
.metric{font-size:2rem;font-weight:700;letter-spacing:-.02em;line-height:1.15;margin-bottom:6px;font-variant-numeric:tabular-nums}
.metric small{font-size:.9rem;font-weight:500;color:var(--muted);margin-left:5px}
.row{display:flex;justify-content:space-between;align-items:baseline;gap:12px;padding:8px 0;border-bottom:1px solid var(--line2)}
.row:last-child{border-bottom:0}
.row>span:first-child{color:var(--muted);flex:0 0 auto}
/* Long values (the REST URL, the link key) wrap instead of running out of the card. */
.row>span:last-child{min-width:0;overflow-wrap:anywhere;text-align:right;font-variant-numeric:tabular-nums}
.rows+label,.rows+.inline{margin-top:16px}
.spark{margin-top:10px}
.spark canvas{width:100%;height:64px;display:block}
.caption{font-size:.75rem;color:var(--muted);margin-top:4px}

/* ── forms ───────────────────────────────────────────────── */
label{display:block;font-size:.85rem;color:var(--muted);margin:14px 0 6px}
h3+label,.card>label:first-child{margin-top:0}
input,select{width:100%;min-height:44px;padding:10px 13px;border-radius:11px;border:1px solid var(--line);
  background:rgba(0,0,0,.25);color:var(--txt);font:inherit;font-size:16px;outline:none}
select{cursor:pointer}
input:focus,select:focus{border-color:var(--accent);box-shadow:var(--ring)}
input[type=file]{padding:8px;font-size:.9rem}
input[type=file]::file-selector-button{margin-right:10px;padding:6px 12px;border-radius:8px;border:1px solid var(--line);
  background:var(--card2);color:var(--txt);font:inherit;cursor:pointer}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:0 12px}
.pair>div{min-width:0}
.inline{display:flex;gap:10px;align-items:stretch}
.inline>input{flex:1;min-width:0}
.inline>button{flex:0 0 auto}

.primary,.ghost{display:inline-flex;align-items:center;justify-content:center;text-align:center;gap:6px;
  min-height:44px;padding:0 18px;border-radius:11px;font:inherit;font-size:.93rem;font-weight:600;
  text-decoration:none;cursor:pointer;line-height:1.2}
.primary{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;border:0}
.ghost{background:var(--card2);color:var(--txt);border:1px solid var(--line)}
.ghost:hover{border-color:rgba(255,255,255,.2)}
.primary:disabled,.ghost:disabled{opacity:.5;cursor:default}
.primary:focus-visible,.ghost:focus-visible{outline:none;box-shadow:var(--ring)}
.danger{border-color:rgba(248,113,113,.4);color:var(--err)}
.ghost.active{border-color:rgba(251,191,36,.5);color:var(--warn)}
/* A form ends in one row of buttons that share its width equally. */
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}
.actions>*{flex:1 1 140px}
h3+.actions,h3+.hint{margin-top:0}

.switch{position:relative;display:flex;align-items:flex-start;gap:12px;margin-top:16px;cursor:pointer;
  font-size:.86rem;color:var(--muted);line-height:1.4}
.switch input{position:absolute;opacity:0;width:0;height:0;min-height:0}
.switch .track{flex:0 0 auto;width:44px;height:25px;border-radius:999px;background:rgba(0,0,0,.35);
  border:1px solid var(--line);position:relative;transition:background .15s,border-color .15s;margin-top:1px}
.switch .track::after{content:"";position:absolute;top:2px;left:2px;width:19px;height:19px;border-radius:50%;
  background:var(--muted);transition:transform .15s,background .15s}
.switch input:checked+.track{background:linear-gradient(135deg,var(--accent),var(--accent2));border-color:transparent}
.switch input:checked+.track::after{transform:translateX(19px);background:#fff}
.switch input:focus-visible+.track{box-shadow:var(--ring)}
.switch b{color:var(--txt);display:block;margin-bottom:2px}

details.more{margin-top:16px;border-top:1px solid var(--line2);padding-top:12px}
details.more>summary{cursor:pointer;color:var(--muted);font-size:.88rem;list-style:none;display:flex;align-items:center;gap:8px}
details.more>summary::-webkit-details-marker{display:none}
details.more>summary::before{content:"";width:7px;height:7px;border-right:2px solid currentColor;border-bottom:2px solid currentColor;
  transform:rotate(-45deg);transition:transform .15s;margin:0 2px}
details.more[open]>summary::before{transform:rotate(45deg)}
details.more>summary:hover{color:var(--txt)}

/* ── setup ───────────────────────────────────────────────── */
.steps{counter-reset:step}
.step{counter-increment:step}
.step>h2::before{content:counter(step) " \00b7  ";color:var(--accent)}
.lead{color:var(--muted);font-size:.9rem;margin:0 0 4px}
.choice{display:grid;gap:12px;grid-template-columns:1fr 1fr;margin-top:14px}
.choice button{padding:16px 18px;border-radius:14px;border:1px solid var(--line);background:rgba(0,0,0,.18);
  color:var(--txt);text-align:left;cursor:pointer;font:inherit;line-height:1.45}
.choice button:hover{border-color:rgba(255,255,255,.2)}
.choice button.on{border-color:var(--accent);box-shadow:var(--ring);background:rgba(162,89,255,.08)}
.choice b{display:block;font-size:1rem}
.choice span{display:block;margin-top:6px;color:var(--muted);font-size:.85rem;line-height:1.4}
.wrap.setup{max-width:760px}
.wrap.setup>.top{margin-bottom:16px}
.savebar{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}
.savebar>*{flex:1 1 180px}

/* ── messages ────────────────────────────────────────────── */
.hint{font-size:.82rem;color:var(--muted);margin:8px 0 0;line-height:1.45}
.bad,.warnbox{border-radius:11px;padding:10px 12px;font-size:.88rem;line-height:1.45}
.bad{color:var(--err);border:1px solid rgba(248,113,113,.35);background:rgba(248,113,113,.08)}
.warnbox{color:var(--warn);border:1px solid rgba(251,191,36,.35);background:rgba(251,191,36,.07)}
.card .bad,.card .warnbox{margin:10px 0 0}
.banners{display:grid;gap:10px;margin-bottom:var(--gap)}
.banners:empty{display:none}
.wifilist{display:grid;gap:6px;margin-top:8px}
.wifilist:empty{display:none}
.wifi{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 12px;width:100%;
  border:1px solid var(--line);border-radius:11px;background:rgba(0,0,0,.2);cursor:pointer;color:var(--txt);font:inherit;text-align:left}
.wifi span:first-child{min-width:0;overflow-wrap:anywhere}
.wifi.on{border-color:var(--accent);box-shadow:var(--ring)}
.bars{display:inline-flex;gap:2px;align-items:flex-end;height:14px;flex:0 0 auto}
.bars i{width:3px;background:rgba(152,163,196,.35);border-radius:1px;display:block}
.bars i.on{background:var(--ok)}
.log{background:rgba(0,0,0,.35);border:1px solid var(--line);border-radius:12px;padding:10px 12px;
  font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.78rem;line-height:1.5;max-height:300px;overflow:auto;
  white-space:pre-wrap;overflow-wrap:anywhere;margin-top:10px}
h3+.log{margin-top:0}
.toast{position:fixed;left:50%;bottom:max(20px,env(safe-area-inset-bottom));transform:translate(-50%,8px);max-width:calc(100% - 32px);
  background:#111a33;border:1px solid var(--line);padding:11px 16px;border-radius:12px;opacity:0;
  transition:opacity .2s,transform .2s;pointer-events:none;z-index:10;box-shadow:0 10px 30px rgba(0,0,0,.4)}
.toast.on{opacity:1;transform:translate(-50%,0)}
.toast.bad{border-color:var(--err)}

@media (max-width:620px){
  .wrap{padding:14px 14px}
  .tabs{margin:10px -14px 12px;padding:8px 14px}
  .choice{grid-template-columns:1fr}
  .metric{font-size:1.7rem}
  .card{padding:16px}
}
@media (max-width:360px){.pair{grid-template-columns:1fr}}
)CSS";

static const char kLoginHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anmelden · Power Meter</title><link rel="stylesheet" href="/style.css"></head>
<body><div class="aurora"><span class="a"></span><span class="b"></span><span class="c"></span></div>
<div class="wrap narrow">
  <div class="card">
    <h1>Power Meter</h1>
    <p class="muted small">Melde dich mit dem Zugangscode an. Er steht auf dem Display des Geräts
      (Seite „Zugang“) und ist zugleich das Passwort des Geräte-WLANs.</p>
    <!--ERR-->
    <form method="POST" action="/login">
      <label for="pw">Zugangscode</label>
      <input id="pw" name="pw" type="password" autocomplete="current-password" autocapitalize="characters"
             spellcheck="false" autofocus required>
      <div class="actions"><button class="primary">Anmelden</button></div>
    </form>
    <p class="hint">Groß- und Kleinschreibung ist egal.</p>
  </div>
</div></body></html>)HTML";

static const char kSetupHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Einrichten · Power Meter</title><link rel="stylesheet" href="/style.css"></head>
<body><div class="aurora"><span class="a"></span><span class="b"></span><span class="c"></span></div>
<div class="wrap setup">
  <div class="top">
    <div class="id"><h1>Power Meter einrichten</h1></div>
    <div class="badges"><span class="badge mono" id="mac">–</span><span class="badge mono" id="fw">–</span></div>
  </div>

  <div class="steps">
    <div class="card step">
      <h2>Welches Gerät ist das?</h2>
      <p class="lead">Ein Board liest den Zähler, das andere empfängt die Werte. Die Wahl lässt sich später ändern.</p>
      <div class="choice">
        <button type="button" data-role="1"><b>Keller · Zähler</b>
          <span>Liest den Stromzähler per Infrarot-Lesekopf und sendet die Werte per LoRa. Bietet dauerhaft ein eigenes WLAN als Statusanzeige.</span></button>
        <button type="button" data-role="2"><b>Wohnung · Gateway</b>
          <span>Empfängt die Werte per LoRa, ist im Heim-WLAN und stellt sie Home Assistant bereit.</span></button>
      </div>
    </div>

    <div class="card step" data-roles="1 2" hidden>
      <h2>Gerätename</h2>
      <label for="name">Name</label>
      <input id="name" maxlength="27" placeholder="z. B. Keller">
      <p class="hint">Erscheint im Dashboard, im Netzwerk und in Home Assistant. Leer lassen für den Standardnamen.</p>
    </div>

    <div class="card step" data-roles="1 2" hidden>
      <h2>Heim-WLAN <span class="muted small" data-roles="1">(optional)</span></h2>
      <p class="lead" data-roles="2">Das Gateway meldet sich in diesem Netz an und ist danach unter seiner IP bzw.
        <span class="mono" id="hostname">lilygo-power-….local</span> erreichbar.</p>
      <p class="lead" data-roles="1">Der Keller-Knoten braucht kein WLAN – er sendet per LoRa. Mit WLAN ist er zusätzlich
        aus dem Heimnetz erreichbar, z. B. für Updates.</p>
      <label for="ssid">Netzwerk</label>
      <div class="inline">
        <input id="ssid" placeholder="WLAN-Name" autocapitalize="off" spellcheck="false">
        <button type="button" class="ghost" id="scanBtn">Suchen</button>
      </div>
      <div class="wifilist" id="wifilist"></div>
      <label for="pass">Passwort</label>
      <input id="pass" type="password" placeholder="WLAN-Passwort" autocomplete="new-password">
    </div>

    <div class="card step" data-roles="1" hidden>
      <h2>Stromzähler</h2>
      <label for="interval">Sendeintervall (Sekunden)</label>
      <input id="interval" type="number" min="2" max="600" step="1" value="15" inputmode="numeric">
      <p class="hint">Wie oft ein Messwert zum Gateway geht. Erlaubt ist 1&nbsp;% Sendezeit pro Stunde – das Gerät hält
        das selbst ein und lässt bei Bedarf einzelne Sendungen aus.</p>
      <details class="more">
        <summary>Lesekopf: Protokoll, Polarität, Pins</summary>
        <p class="hint">Normalerweise nicht nötig – das Gerät erkennt Protokoll und Polarität selbst.</p>
        <div class="pair">
          <div><label for="protocol">Protokoll</label>
            <select id="protocol">
              <option value="0">Automatisch</option>
              <option value="1">SML (sendet selbst)</option>
              <option value="2">D0 / IEC 62056-21</option>
            </select></div>
          <div><label for="polarity">Polarität</label>
            <select id="polarity">
              <option value="0">Automatisch</option>
              <option value="1">Normal</option>
              <option value="2">Invertiert</option>
            </select></div>
          <div><label for="meterRx">RX-Pin (Lesekopf → Gerät)</label>
            <input id="meterRx" type="number" min="1" max="48" placeholder="40" inputmode="numeric"></div>
          <div><label for="meterTx">TX-Pin (nur D0-Weckruf)</label>
            <input id="meterTx" type="number" min="1" max="48" placeholder="41" inputmode="numeric"></div>
        </div>
        <p class="hint">Standard: RX&nbsp;40, TX&nbsp;41. Frei sind 15 16 38–44 (Header) sowie 10 und 21 (QWIIC).
          Nicht 0, 45 oder 46 (Boot-Pins).</p>
      </details>
    </div>

    <div class="card step" data-roles="1" hidden>
      <h2>Geräte-WLAN</h2>
      <p class="lead">Der Keller-Knoten bietet dauerhaft ein eigenes WLAN an, über das diese Seite vor Ort erreichbar ist.</p>
      <div class="pair">
        <div><label for="apname">Name</label>
          <input id="apname" maxlength="32" placeholder="z. B. Stromzaehler-Keller"></div>
        <div><label for="apcode">Neuer Zugangscode</label>
          <input id="apcode" minlength="8" maxlength="15" placeholder="unverändert" autocomplete="off"></div>
      </div>
      <p class="hint">Der Code ist WLAN-Passwort und Anmeldecode zugleich – 8 bis 15 Buchstaben oder Ziffern.</p>
    </div>

    <div class="card step" data-roles="2" hidden>
      <h2>Home Assistant</h2>
      <p class="lead">Nichts einzutragen nötig: Home Assistant liest die Werte per REST. Die fertige YAML-Datei gibt es
        später im Dashboard unter „Home Assistant“.</p>
      <details class="more">
        <summary>Optional: MQTT-Broker für automatische Erkennung</summary>
        <p class="hint">Mit Broker (z. B. Mosquitto-Add-on) meldet sich das Gerät in Home Assistant von selbst an.</p>
        <label for="mqttHost">Broker</label>
        <input id="mqttHost" placeholder="homeassistant.local" autocapitalize="off" spellcheck="false">
        <div class="pair">
          <div><label for="mqttPort">Port</label><input id="mqttPort" type="number" placeholder="1883" inputmode="numeric"></div>
          <div><label for="mqttUser">Benutzer</label><input id="mqttUser" placeholder="optional" autocapitalize="off"></div>
        </div>
        <label for="mqttPass">Passwort</label>
        <input id="mqttPass" type="password" placeholder="optional" autocomplete="new-password">
      </details>
    </div>
  </div>

  <div class="card" id="doneCard" hidden>
    <h2 id="doneTitle">Gespeichert</h2>
    <p class="lead" id="doneLead"></p>
    <div class="rows" id="doneRows"></div>
    <div class="actions" id="doneActions"></div>
    <p class="hint" id="doneHint"></p>
  </div>

  <div class="savebar">
    <button class="primary" id="saveBtn" disabled>Speichern &amp; starten</button>
    <a class="ghost" id="backBtn" href="/" hidden>Abbrechen</a>
  </div>
  <p class="hint" id="saveHint">Bitte zuerst oben auswählen, welches Gerät das ist.</p>
</div>
<div class="toast" id="toast"></div>
<script src="/app.js"></script></body></html>)HTML";

static const char kAppHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Power Meter</title><link rel="stylesheet" href="/style.css"></head>
<body><div class="wrap">
  <div class="top">
    <div class="id">
      <h1 id="name">Power Meter</h1>
      <div class="badges"><span class="badge" id="role">…</span><span class="badge" id="link">…</span></div>
    </div>
    <div class="badges"><span class="badge" id="uptime" title="Laufzeit"></span></div>
  </div>

  <nav class="tabs">
    <button class="on" data-tab="status">Übersicht</button>
    <button data-tab="meter">Zähler</button>
    <button data-tab="link">Funk</button>
    <button data-tab="net">WLAN</button>
    <button data-tab="ha" data-only="gateway">Home Assistant</button>
    <button data-tab="system">System</button>
  </nav>

  <!-- ── Übersicht ─────────────────────────────────────────── -->
  <section class="panel on" id="tab-status">
    <div class="banners" id="banners"></div>
    <div class="grid even">
      <div class="card"><h3>Leistung</h3>
        <div class="metric" id="m-power">–<small>W</small></div>
        <div class="rows" id="m-phases"></div>
        <div class="spark"><canvas id="spark" height="64"></canvas><div class="caption" id="spark-cap">Verlauf seit dem Öffnen der Seite</div></div>
      </div>
      <div class="card"><h3>Zählerstand</h3>
        <div class="metric" id="m-import">–<small>kWh</small></div>
        <div class="rows">
          <div class="row"><span>Einspeisung</span><span id="m-export">–</span></div>
          <div class="row"><span>Letzte Messung</span><span id="m-age">–</span></div>
        </div>
      </div>
      <div class="card"><h3>Funkstrecke</h3>
        <div class="rows">
          <div class="row"><span>Gegenstelle</span><span id="l-peer">–</span></div>
          <div class="row"><span>Signal (RSSI / SNR)</span><span id="l-radio">–</span></div>
          <div class="row"><span>Sendezeit (1 h)</span><span id="l-duty">–</span></div>
          <div class="row"><span>Bestätigungen</span><span id="l-ack">–</span></div>
        </div>
      </div>
    </div>
  </section>

  <!-- ── Zähler ────────────────────────────────────────────── -->
  <section class="panel" id="tab-meter">
    <div class="grid">
      <div class="card"><h3>Messwerte</h3><div class="rows" id="values"></div></div>

      <div class="card" data-only="meter"><h3>Lesekopf</h3>
        <div class="rows">
          <div class="row"><span>Protokoll</span><span id="mt-proto">–</span></div>
          <div class="row"><span>Polarität</span><span id="mt-pol">–</span></div>
          <div class="row"><span>Telegramme</span><span id="mt-count">–</span></div>
          <div class="row"><span>Prüfsummenfehler</span><span id="mt-crc">–</span></div>
          <div class="row"><span>Bytes auf der Leitung</span><span id="mt-bytes">–</span></div>
          <div class="row"><span>Pins</span><span id="mt-pins" class="mono">–</span></div>
        </div>
        <div id="mt-wire-hint" hidden></div>
      </div>

      <div class="card" data-only="meter"><h3>Lesekopf einstellen</h3>
        <div class="pair">
          <div><label for="protocol">Protokoll</label>
            <select id="protocol" data-sync>
              <option value="0">Automatisch</option>
              <option value="1">SML</option>
              <option value="2">D0 / IEC 62056-21</option>
            </select></div>
          <div><label for="polarity">Polarität</label>
            <select id="polarity" data-sync>
              <option value="0">Automatisch</option>
              <option value="1">Normal</option>
              <option value="2">Invertiert</option>
            </select></div>
          <div><label for="meterRx">RX-Pin</label><input id="meterRx" data-sync type="number" min="1" max="48" inputmode="numeric"></div>
          <div><label for="meterTx">TX-Pin (D0)</label><input id="meterTx" data-sync type="number" min="1" max="48" inputmode="numeric"></div>
        </div>
        <p class="hint">„Automatisch“ passt fast immer. Frei sind die Pins 15 16 38–44 (Header) sowie 10 und 21 (QWIIC);
          nicht 0, 45 oder 46 – 46 blockiert sonst das Flashen.</p>
        <div class="actions"><button class="primary" id="saveMeter">Übernehmen</button></div>
      </div>

      <div class="card" data-only="meter"><h3>Diagnose</h3>
        <div class="actions">
          <button class="ghost" id="simBtn" data-toggle="sim">Simulator</button>
          <button class="ghost" id="loopBtn" data-toggle="loopback">Loopback</button>
        </div>
        <p class="hint" id="loopHint">Simulator: erzeugt Testwerte ohne Zähler. Loopback: RX- und TX-Pin mit einem Draht
          verbinden – Test-Telegramme laufen dann über die echte Leitung.</p>
        <div class="actions"><button class="ghost" id="scopeBtn">Leitung messen (3 s)</button></div>
        <div class="log" id="scope-out" hidden></div>
        <details class="more">
          <summary>So funktioniert der Test</summary>
          <p class="hint">Die Messung tastet den RX-Pin dreimal je eine Sekunde ab – unabhängig von Baudrate, Polarität
            und Protokoll. Eine TV-Fernbedienung frontal auf den Lesekopf halten, eine Taste gedrückt halten und messen:
            springen die Flanken/s, arbeitet der Empfänger, und es liegt am Zähler (Ausrichtung, PIN-Freischaltung).
            Bleibt alles bei 0&nbsp;%, kommt nichts an – Versorgung, GND und Datenleitung prüfen.</p>
        </details>
      </div>

      <div class="card wide"><h3>Letztes Telegramm</h3><div class="log" id="dump">–</div></div>
    </div>
  </section>

  <!-- ── Funk ──────────────────────────────────────────────── -->
  <section class="panel" id="tab-link">
    <div class="grid">
      <div class="card"><h3>Verbindung</h3>
        <div class="rows">
          <div class="row"><span>Status</span><span id="lk-online">–</span></div>
          <div class="row"><span>Kopplung</span><span id="lk-pair">–</span></div>
          <div class="row"><span>Letztes Paket</span><span id="lk-seen">–</span></div>
          <div class="row"><span>Gesendet / empfangen</span><span id="lk-counts">–</span></div>
          <div class="row"><span>Probleme</span><span id="lk-fail">–</span></div>
          <div class="row"><span>Netz-ID</span><span id="lk-net" class="mono">–</span></div>
        </div>
        <div id="lk-pair-msg" class="hint"></div>
        <div class="actions">
          <button class="primary" id="pairBtn" data-act="pair">Koppeln</button>
          <button class="ghost" data-act="unpair" data-confirm="Kopplung wirklich lösen?">Kopplung lösen</button>
        </div>
        <p class="hint" id="lk-timeout">Beide Geräte müssen in Reichweite sein; die Kopplung dauert bis zu einer Minute.</p>
      </div>

      <div class="card"><h3>Funkparameter</h3>
        <div class="rows"><div class="row"><span>In Betrieb</span><span id="radio-active">–</span></div></div>
        <label for="freq">Frequenz (MHz)</label>
        <input id="freq" data-sync type="number" step="0.0125" min="868.0625" max="868.5375" inputmode="decimal">
        <div class="pair">
          <div><label for="sf">Spreading Factor</label><select id="sf" data-sync>
            <option value="7">SF7 (Standard)</option>
            <option value="8">SF8</option>
            <option value="9">SF9</option>
            <option value="10">SF10</option>
            <option value="11">SF11</option>
            <option value="12">SF12</option>
          </select></div>
          <div><label for="power">Leistung (dBm)</label>
            <input id="power" data-sync type="number" min="2" max="14" inputmode="numeric"></div>
        </div>
        <div data-only="meter">
          <label for="interval">Sendeintervall (Sekunden)</label>
          <input id="interval" data-sync type="number" min="2" max="600" inputmode="numeric">
          <p class="hint" id="intervalFloor"></p>
        </div>
        <p class="warnbox"><b>Beide Geräte brauchen dieselbe Frequenz und denselben SF</b>, sonst hören sie sich nicht.</p>
        <p class="hint">Erlaubt: 868,0625–868,5375&nbsp;MHz, höchstens 14&nbsp;dBm und 1&nbsp;% Sendezeit pro Stunde.
          Ein höherer SF reicht weiter, kostet aber ein Vielfaches an Sendezeit.</p>
        <div class="actions"><button class="primary" id="saveRadio">Übernehmen</button></div>
      </div>

      <div class="card"><h3>Verschlüsselung</h3>
        <div class="rows">
          <div class="row"><span>Status</span><span id="lk-crypt">–</span></div>
          <div class="row"><span>Kontrollzahl</span><span id="lk-check" class="mono">–</span></div>
        </div>
        <p class="hint">Beim Koppeln vereinbaren die beiden Geräte selbst einen neuen Schlüssel (X25519-Schlüsseltausch);
          er geht nie über Funk und muss nirgends eingetippt werden. Die Kontrollzahl wird aus dem Schlüssel berechnet
          und steht auch auf dem Display: Ist sie auf beiden Geräten gleich, haben beide denselben Schlüssel.</p>
      </div>
    </div>
  </section>

  <!-- ── WLAN ──────────────────────────────────────────────── -->
  <section class="panel" id="tab-net">
    <div class="grid">
      <div class="card"><h3>Heim-WLAN</h3>
        <div class="rows">
          <div class="row"><span>Verbindung</span><span id="w-link">–</span></div>
          <div class="row"><span>Netz</span><span id="w-ssid" class="mono">–</span></div>
          <div class="row"><span>IP-Adresse</span><span id="w-ip" class="mono">–</span></div>
          <div class="row"><span>Signal</span><span id="w-rssi">–</span></div>
        </div>
        <div id="wifi-warn" class="warnbox" hidden></div>
        <label for="ssid">Netzwerk</label>
        <div class="inline">
          <input id="ssid" data-sync placeholder="WLAN-Name" autocapitalize="off" spellcheck="false">
          <button class="ghost" id="scanBtn">Suchen</button>
        </div>
        <div class="wifilist" id="wifilist"></div>
        <label for="pass">Passwort</label>
        <input id="pass" type="password" placeholder="unverändert lassen" autocomplete="new-password">
        <p class="hint" data-only="meter">Optional – der Keller-Knoten arbeitet auch ohne Heim-WLAN.</p>
        <div class="actions"><button class="primary" id="saveWifi">Verbinden</button></div>
        <p class="hint mono" id="w-diag"></p>
      </div>

      <div class="card"><h3>Geräte-WLAN</h3>
        <div class="rows">
          <div class="row"><span>Status</span><span id="w-ap">–</span></div>
        </div>
        <label class="switch" for="keepap" data-only="gateway">
          <input type="checkbox" id="keepap" data-sync><span class="track"></span>
          <span><b>Dauerhaft anbieten</b>Sonst schaltet es sich ab, sobald das Heim-WLAN steht. Dauerhaft an bleibt
            die Seite erreichbar, wenn der Router ausfällt.</span>
        </label>
        <div class="pair">
          <div><label for="apname">Name</label><input id="apname" data-sync maxlength="32"></div>
          <div><label for="apcode">Neuer Code</label>
            <input id="apcode" minlength="8" maxlength="15" placeholder="unverändert" autocomplete="off"></div>
        </div>
        <p class="hint">Der Code ist WLAN-Passwort und – solange kein eigenes Dashboard-Passwort gesetzt ist – auch der
          Anmeldecode. Nach dem Speichern trennt sich dein Gerät kurz vom Geräte-WLAN.</p>
        <div class="actions"><button class="primary" id="saveAp">Übernehmen</button></div>
      </div>
    </div>
  </section>

  <!-- ── Home Assistant (gateway) ──────────────────────────── -->
  <section class="panel" id="tab-ha">
    <div class="grid">
      <div class="card"><h3>REST (empfohlen)</h3>
        <div class="rows"><div class="row"><span>Endpunkt</span><span id="ha-url" class="mono">–</span></div></div>
        <div class="actions">
          <a class="primary" id="haYaml" href="/ha/power_meter.yaml" download>YAML-Paket</a>
          <a class="ghost" id="haCard" href="/ha/card.yaml" download>Dashboard-Karte</a>
        </div>
        <p class="hint">Das Paket nach <span class="mono">config/packages/</span> kopieren, in
          <span class="mono">configuration.yaml</span> unter <span class="mono">homeassistant:</span> die Zeile
          <span class="mono">packages: !include_dir_named packages</span> ergänzen und Home Assistant neu starten.</p>
      </div>

      <div class="card"><h3>MQTT (optional)</h3>
        <div class="rows"><div class="row"><span>Status</span><span id="mqtt-state">–</span></div></div>
        <label for="mqttHost">Broker</label>
        <input id="mqttHost" data-sync placeholder="homeassistant.local" autocapitalize="off" spellcheck="false">
        <div class="pair">
          <div><label for="mqttPort">Port</label><input id="mqttPort" data-sync type="number" placeholder="1883" inputmode="numeric"></div>
          <div><label for="mqttUser">Benutzer</label><input id="mqttUser" data-sync placeholder="optional" autocapitalize="off"></div>
        </div>
        <label for="mqttPass">Passwort</label>
        <input id="mqttPass" type="password" placeholder="unverändert lassen" autocomplete="new-password">
        <div class="actions">
          <button class="primary" id="saveMqtt">Speichern</button>
          <button class="ghost" id="mqttOff">Ausschalten</button>
        </div>
        <details class="more">
          <summary>Erkennung &amp; Vorschau</summary>
          <div class="actions">
            <button class="ghost" data-act="mqtt-announce">Jetzt ankündigen</button>
            <button class="ghost" id="mqttDumpBtn">Vorschau</button>
          </div>
          <div class="log" id="mqtt-dump" hidden></div>
        </details>
      </div>
    </div>
  </section>

  <!-- ── System ────────────────────────────────────────────── -->
  <section class="panel" id="tab-system">
    <div class="grid">
      <div class="card"><h3>Gerät</h3>
        <div class="rows">
          <div class="row"><span>Firmware</span><span id="s-fw" class="mono">–</span></div>
          <div class="row"><span>MAC</span><span id="s-mac" class="mono">–</span></div>
          <div class="row"><span>Freier Speicher</span><span id="s-heap">–</span></div>
          <div class="row"><span>Laufzeit</span><span id="s-up">–</span></div>
        </div>
        <label for="devname">Name</label><input id="devname" data-sync maxlength="27">
        <div class="actions">
          <button class="primary" id="saveDevice">Übernehmen</button>
          <a class="ghost" id="setupLink" href="/setup?edit=1">Rolle ändern …</a>
        </div>
      </div>

      <div class="card"><h3>Anmeldung</h3>
        <div class="rows"><div class="row"><span>Dashboard-Passwort</span><span id="pw-state">–</span></div></div>
        <label for="newpass">Eigenes Passwort</label>
        <input id="newpass" type="password" placeholder="mind. 8 Zeichen" minlength="8" autocomplete="new-password">
        <p class="hint">Ab dann gilt der Code vom Display nicht mehr als Anmeldung – nur noch als WLAN-Passwort.</p>
        <div class="actions">
          <button class="primary" id="saveAccess">Übernehmen</button>
          <button class="ghost" id="logoutBtn">Abmelden</button>
        </div>
      </div>

      <div class="card"><h3>Firmware-Update</h3>
        <form method="POST" action="/ota" enctype="multipart/form-data" id="otaForm">
          <label for="otaFile">Firmware-Datei</label>
          <input id="otaFile" type="file" name="firmware" accept=".bin" required>
          <p class="hint">Die Datei <span class="mono">…-update.bin</span> aus dem Release (nicht „factory“).
            Einstellungen und Kopplung bleiben erhalten; das Gerät startet danach neu. Schlägt das Update fehl,
            läuft die alte Firmware einfach weiter.</p>
          <div class="actions"><button class="primary">Hochladen</button></div>
        </form>
      </div>

      <div class="card"><h3>Display</h3>
        <div class="rows">
          <div class="row"><span>Seite</span><span id="oled-page">–</span></div>
          <div class="row"><span>BOOT-Taste</span><span id="oled-btn">–</span></div>
        </div>
        <p class="hint">Die Taste BOOT am Gerät blättert ebenfalls weiter.</p>
        <div class="actions"><button class="ghost" data-act="page-next">Nächste Seite</button></div>
      </div>

      <div class="card"><h3>Neustart &amp; Zurücksetzen</h3>
        <p class="hint">„Werkszustand“ löscht alle Einstellungen inklusive Kopplung und startet in die
          Einrichtung.</p>
        <div class="actions">
          <button class="ghost" data-act="reboot" data-confirm="Gerät neu starten?">Neu starten</button>
          <button class="ghost danger" data-act="factory"
                  data-confirm="Wirklich ALLE Einstellungen löschen? Das Gerät muss danach neu eingerichtet und gekoppelt werden.">Werkszustand</button>
        </div>
      </div>

      <div class="card wide"><h3>Ereignisprotokoll</h3><div class="log" id="log">–</div></div>
    </div>
  </section>
</div>
<div class="toast" id="toast"></div>
<script src="/app.js"></script></body></html>)HTML";

static const char kAppJs[] PROGMEM = R"JS(
'use strict';
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s === null || s === undefined ? '' : s)
  .replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const set = (id, v) => { const el = $(id); if (el) el.textContent = v; };
const show = (el, on) => { if (el) el.hidden = !on; };

// The session token arrives as ?t=… after a successful login and is kept for
// the rest of the session. It is passed explicitly so access never depends on
// the browser accepting our cookie.
let token = new URLSearchParams(location.search).get('t') || sessionStorage.getItem('sid') || '';
if (token) {
  sessionStorage.setItem('sid', token);
  // Keep the token out of the address bar, where it would end up in bookmarks.
  if (location.search) history.replaceState(null, '', location.pathname + location.hash);
}
function withToken(url) {
  if (!token) return url;
  return url + (url.indexOf('?') >= 0 ? '&' : '?') + 't=' + encodeURIComponent(token);
}

let state = null;
const powerHistory = [];

let toastTimer = 0;
function toast(msg, bad) {
  const el = $('toast'); if (!el) return;
  el.textContent = msg;
  el.className = 'toast on' + (bad ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = 'toast'; }, bad ? 4500 : 2600);
}

// Field names the server reports as refused, in words.
const FIELD_LABELS = {
  freq: 'Frequenz', sf: 'Spreading Factor', power: 'Sendeleistung', interval: 'Sendeintervall',
  protocol: 'Protokoll', polarity: 'Polarität', apname: 'WLAN-Name (mind. 4 Zeichen)',
  apcode: 'Zugangscode (8–15 Buchstaben/Ziffern)', newpass: 'Passwort (mind. 8 Zeichen)',
  mqttPort: 'Port'
};

async function post(url, data) {
  const body = new URLSearchParams(data || {}).toString();
  let res;
  try {
    res = await fetch(withToken(url), { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
  } catch (e) {
    toast('Gerät nicht erreichbar', true);
    return { ok: false };
  }
  if (res.status === 401) {
    // Never leave the user staring at a save button that silently refuses.
    toast('Anmeldung nötig – weiter zur Anmeldung …', true);
    setTimeout(() => { location.href = '/login'; }, 1200);
    return { ok: false };
  }
  const text = await res.text();
  let json = null; try { json = JSON.parse(text); } catch (e) {}
  if (json && json.refused) {
    const names = json.refused.split(', ').map((k) => FIELD_LABELS[k] || k);
    toast('Nicht übernommen: ' + names.join(', '), true);
    return json;
  }
  if (json) { toast(json.message || (json.ok ? 'Gespeichert' : 'Fehler'), !json.ok); return json; }
  toast(res.ok ? 'Gespeichert' : 'Fehler', !res.ok);
  return { ok: res.ok };
}

function fmtDuration(s) {
  if (s === null || s === undefined) return '–';
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' min ' + (s % 60) + ' s';
  if (s < 86400) return Math.floor(s / 3600) + ' h ' + Math.floor((s % 3600) / 60) + ' min';
  return Math.floor(s / 86400) + ' d ' + Math.floor((s % 86400) / 3600) + ' h';
}
const ago = (s) => (s === null || s === undefined) ? '–' : (s < 2 ? 'gerade eben' : 'vor ' + fmtDuration(s));
const num = (v, d) => (v === null || v === undefined) ? '–'
  : Number(v).toLocaleString('de-DE', { minimumFractionDigits: d, maximumFractionDigits: d });

// Inputs marked data-sync show the device's current value. Once the owner edits
// one, the poll leaves it alone until it has been saved.
function fill(id, v) {
  const el = $(id);
  if (!el || el.dataset.touched || v === undefined || v === null) return;
  if (el.type === 'checkbox') el.checked = !!v;
  else if (document.activeElement !== el) el.value = v;
}

// ── dashboard ────────────────────────────────────────────────
let currentRole = '';
let wantedTab = 'status';
function applyRole(role) {
  if (role === currentRole) return;
  currentRole = role;
  document.querySelectorAll('[data-only]').forEach((el) => { el.hidden = el.dataset.only !== role; });
  // The tab from the address can only be honoured once its role is known; a
  // tab that does not exist for this role falls back to the overview.
  selectTab(wantedTab);
}

function render() {
  if (!state) return;
  const d = state.device, l = state.link, m = state.meter, w = state.wifi, r = state.radio || {};
  const isMeter = d.role === 'meter', isGateway = d.role === 'gateway';
  applyRole(d.role);

  // header
  set('name', d.name); document.title = d.name + ' · Power Meter';
  set('uptime', fmtDuration(d.uptime));
  set('role', isMeter ? 'Keller · Zähler' : isGateway ? 'Wohnung · Gateway' : 'nicht eingerichtet');
  const lb = $('link');
  if (lb) {
    lb.textContent = !l.radio ? 'Funkfehler' : l.peer ? 'Funk verbunden' : l.paired ? 'Gegenstelle fehlt' : 'nicht gekoppelt';
    lb.className = 'badge ' + (!l.radio ? 'err' : l.peer ? 'ok' : 'warn');
  }

  // banners on the overview: only things the owner has to act on
  const banners = [];
  if (m.simulating) banners.push(['bad', 'Simulation aktiv – die Werte kommen vom eingebauten Simulator, nicht vom Zähler. Ausschalten unter Zähler → Diagnose.']);
  if (!l.radio) banners.push(['bad', 'Das Funkmodul antwortet nicht. Gerät neu starten; hilft das nicht, ist die Antenne bzw. das Modul zu prüfen.']);
  else if (!l.paired) banners.push(['warnbox', 'Die beiden Geräte sind noch nicht gekoppelt – unter „Funk“ auf einem von beiden „Koppeln“ drücken.']);
  if (w.configured && !w.connected) banners.push(['warnbox', 'Das Heim-WLAN „' + (w.sta_ssid || '?') + '“ ist nicht erreichbar. Das Gerät bleibt über sein eigenes WLAN erreichbar.']);
  else if (!w.configured && isGateway) banners.push(['warnbox', 'Das Gateway ist noch in keinem Heim-WLAN – unter „WLAN“ ein Netz wählen, damit Home Assistant es erreicht.']);
  const bannerBox = $('banners');
  const bannerHtml = banners.map((b) => '<div class="' + b[0] + '">' + esc(b[1]) + '</div>').join('');
  if (bannerBox && bannerBox.innerHTML !== bannerHtml) bannerBox.innerHTML = bannerHtml;

  // overview
  $('m-power').innerHTML = num(m.power_w, 0) + '<small>W</small>';
  $('m-import').innerHTML = num(m.import_kwh, 2) + '<small>kWh</small>';
  const phases = [['L1', m.l1_w], ['L2', m.l2_w], ['L3', m.l3_w]].filter((p) => p[1] !== null && p[1] !== undefined);
  const phaseHtml = phases.map((p) => '<div class="row"><span>' + p[0] + '</span><span>' + num(p[1], 0) + ' W</span></div>').join('');
  if ($('m-phases').innerHTML !== phaseHtml) $('m-phases').innerHTML = phaseHtml;
  set('m-export', m.export_kwh === null ? '–' : num(m.export_kwh, 2) + ' kWh');
  set('m-age', ago(m.age));
  set('l-peer', l.peer ? ('online' + (l.peer_id && l.peer_id !== '0' ? ' · ' + l.peer_id.toUpperCase() : '')) : 'offline');
  set('l-radio', l.peer ? (l.rssi + ' dBm / ' + num(l.snr, 1) + ' dB') : '–');
  set('l-duty', num(l.duty_pct, 1) + ' % von ' + (l.budget_ms / 1000) + ' s');
  set('l-ack', l.acks + ' ok · ' + l.missed + ' verloren');

  // meter
  const vals = $('values');
  const valHtml = (m.values || []).map((v) =>
    '<div class="row"><span class="mono">' + esc(v.obis) + '</span><span>' + num(v.value, 3) + ' ' + esc(v.unit) + '</span></div>').join('')
    || '<p class="muted small">Noch keine Messwerte.</p>';
  if (vals && vals.innerHTML !== valHtml) vals.innerHTML = valHtml;
  const dump = $('dump'); if (dump) dump.textContent = m.dump || '–';
  set('mt-proto', m.protocol); set('mt-pol', m.polarity);
  set('mt-count', m.telegrams); set('mt-crc', m.crc_errors);
  set('mt-bytes', m.bytes === undefined ? '–' : m.bytes);
  set('mt-pins', 'RX ' + (m.rx_pin || '?') + ' · TX ' + (m.tx_pin || '?'));
  fill('protocol', m.config_protocol); fill('polarity', m.config_polarity);
  fill('meterRx', m.rx_pin); fill('meterTx', m.tx_pin);

  // What the wire tells, but only while nothing decodes: once telegrams arrive,
  // a byte count says nothing more.
  const wire = $('mt-wire-hint');
  if (wire) {
    const rx = 'GPIO' + (m.rx_pin || '?');
    if (m.simulating || m.telegrams > 0 || m.bytes === undefined) {
      show(wire, false);
    } else if (m.bytes < 50) {
      // At 9600 baud a meter delivers hundreds of bytes per second; a handful is noise.
      wire.className = 'bad'; show(wire, true);
      wire.textContent = (m.bytes ? 'Nur ' + m.bytes + ' Byte(s) auf ' + rx + ' – das ist Rauschen, kein Signal. ' : 'Auf ' + rx + ' kommt nichts an. ') +
        'Versorgung, GND und Datenleitung an ' + rx + ' prüfen und den Kopf genau auf die IR-Diode des Zählers setzen.';
    } else {
      wire.className = 'warnbox'; show(wire, true);
      wire.textContent = m.bytes + ' Bytes empfangen, aber noch kein gültiges Telegramm – meist die Polarität. ' +
        'Unter „Lesekopf einstellen“ die andere Polarität probieren.';
    }
  }
  const simBtn = $('simBtn'), loopBtn = $('loopBtn');
  if (simBtn) { simBtn.textContent = m.simulating ? 'Simulator ausschalten' : 'Simulator einschalten'; simBtn.classList.toggle('active', !!m.simulating); }
  if (loopBtn) { loopBtn.textContent = m.loopback ? 'Loopback ausschalten' : 'Loopback einschalten'; loopBtn.classList.toggle('active', !!m.loopback); }
  if (m.rx_pin && m.tx_pin) {
    set('loopHint', 'Simulator: erzeugt Testwerte ohne Zähler. Loopback: GPIO' + m.rx_pin + ' mit GPIO' + m.tx_pin +
      ' per Draht verbinden – Test-Telegramme laufen dann über die echte Leitung.');
  }

  // radio link
  const src = (l.traffic_src || '').toUpperCase();
  set('lk-net', (l.net || '').toUpperCase());
  set('lk-online', !l.radio ? 'Funkmodul-Fehler' : l.peer ? 'verbunden' : l.paired ? 'getrennt – keine Antwort' : 'nicht gekoppelt');
  // The age is the last packet from ANY device: readings keep arriving while the
  // two are not coupled, and a timestamp that never moves made such a link look dead.
  const seen = l.peer ? l.age : l.traffic_age;
  set('lk-seen', (seen === null || seen === undefined) ? 'noch keins'
    : ago(seen) + (l.peer ? '' : ' (von ' + (src || '?') + ')'));
  set('lk-counts', l.tx + ' / ' + l.rx);
  const problems = [];
  if (l.tx_failures) problems.push(l.tx_failures + ' unbestätigt');
  if (l.blocked) problems.push(l.blocked + ' wegen Sendezeit zurückgehalten');
  if (l.auth_failures) problems.push(l.auth_failures + ' mit falscher Signatur');
  if (l.plaintext_dropped) problems.push(l.plaintext_dropped + ' unverschlüsselt verworfen');
  set('lk-fail', problems.length ? problems.join(', ') : 'keine');

  let pairText = l.paired ? ('mit ' + (l.peer_id || '?').toUpperCase()) : 'nein';
  if (l.pair_state === 'running') {
    pairText = 'läuft … ' + (l.pair_elapsed_s || 0) + ' / ' + (l.pair_timeout_s || 60) + ' s';
  } else if (l.pair_state === 'timeout' && !l.paired) {
    pairText = 'fehlgeschlagen';
  }
  set('lk-pair', pairText);
  const pairMsg = $('lk-pair-msg');
  if (pairMsg) {
    let cls = 'hint', txt = '';
    if (l.pair_state === 'running') {
      txt = 'Kopplung läuft – sendet und wartet auf Antwort (' + (l.pair_attempts || 0) + ' Versuche).';
    } else if (l.pair_state === 'timeout' && l.paired) {
      // A re-pair that found nobody keeps the old link, which may well work.
      cls = 'hint';
      txt = 'Neu koppeln: keine Antwort – die bisherige Kopplung mit ' + (l.peer_id || '?').toUpperCase() + ' bleibt bestehen.';
    } else if (l.pair_state === 'timeout') {
      cls = 'bad';
      txt = 'Kopplung fehlgeschlagen: ' + (l.pair_result || 'keine Antwort') + '. Beide Geräte näher zusammenbringen (Antennen dran?) und erneut starten.';
    } else if (l.paired && !l.peer && l.auth_failures) {
      cls = 'warnbox';
      txt = 'Gekoppelt, aber die Pakete der Gegenstelle passen nicht zum Schlüssel – die beiden Geräte haben verschiedene Schlüssel. Einmal neu koppeln.';
    } else if (l.paired && !l.peer && l.plaintext_dropped) {
      cls = 'warnbox';
      txt = 'Gekoppelt, aber die Gegenstelle sendet unverschlüsselt – vermutlich läuft dort noch eine ältere Firmware. Beide Geräte aktualisieren und neu koppeln.';
    } else if (!l.paired && l.traffic_age !== null && l.traffic_age !== undefined) {
      cls = 'warnbox';
      txt = 'Es kommen Pakete von ' + (src || '?') + ' an, aber die Geräte sind nicht gekoppelt – auf einem der beiden „Koppeln“ drücken.';
    }
    pairMsg.className = cls; pairMsg.textContent = txt; show(pairMsg, !!txt);
    const btn = $('pairBtn');
    if (btn) { btn.disabled = l.pair_state === 'running'; btn.textContent = l.paired ? 'Neu koppeln' : 'Koppeln'; }
  }
  set('lk-timeout', 'Beide Geräte müssen in Reichweite sein; die Kopplung dauert bis zu ' + (l.pair_timeout_s || 60) +
    ' s. Die Verbindung gilt als getrennt, wenn ' + (l.peer_timeout_s || 90) + ' s lang nichts ankommt.');

  set('lk-crypt', l.encrypted ? 'an – verschlüsselt und signiert' : 'aus – entsteht beim Koppeln');
  set('lk-check', l.check_code || '–');

  fill('freq', r.freq_mhz); fill('sf', r.sf); fill('power', r.power_dbm);
  if (r.interval_ms) fill('interval', Math.round(r.interval_ms / 1000));
  if (r.min_interval_ms) {
    const floorS = Math.ceil(r.min_interval_ms / 1000);
    set('intervalFloor', r.interval_ms && r.interval_ms < r.min_interval_ms
      ? 'Bei SF' + r.sf + ' sendet das Gerät höchstens alle ' + floorS + ' s – sonst wäre die erlaubte Sendezeit vor Ablauf der Stunde verbraucht.'
      : 'Kürzestes Intervall bei SF' + r.sf + ': ' + floorS + ' s (1 % Sendezeit pro Stunde).');
  }
  const activeFreq = r.active_freq_mhz !== undefined ? r.active_freq_mhz : r.freq_mhz;
  set('radio-active', activeFreq !== undefined
    ? num(activeFreq, 4) + ' MHz · SF' + (r.active_sf || r.sf) + ' · ' + (r.active_power_dbm !== undefined ? r.active_power_dbm : r.power_dbm) + ' dBm'
    : '–');

  // WLAN
  set('w-link', w.connected ? 'verbunden' : (w.configured ? 'nicht verbunden' : 'nicht eingerichtet'));
  set('w-ssid', w.sta_ssid || '–');
  set('w-ip', w.connected ? w.ip : '–');
  set('w-rssi', (w.connected && w.rssi) ? (w.rssi + ' dBm') : '–');
  set('w-ap', w.ap_up ? ('an · ' + (w.ap_ssid || '?') + ' · ' + (w.clients || 0) + ' verbunden') : 'aus');
  set('w-diag', 'Modus ' + w.mode + ' · ' + w.state + (w.portal ? ' · Einrichtungsportal aktiv' : ''));
  const wifiWarn = $('wifi-warn');
  if (wifiWarn) {
    const msg = (w.configured && !w.connected)
      ? '„' + (w.sta_ssid || '?') + '“ ist nicht erreichbar. Netz und Passwort prüfen – das Gerät versucht es alle paar Minuten erneut.'
      : '';
    wifiWarn.textContent = msg; show(wifiWarn, !!msg);
  }
  fill('ssid', w.sta_ssid); fill('keepap', w.keep_ap); fill('apname', w.ap_ssid);

  // Home Assistant
  set('ha-url', state.ha.url);
  if (state.mqtt) {
    const q = state.mqtt;
    set('mqtt-state', q.connected ? ('verbunden' + (q.announced ? ' · Entitäten angemeldet' : ''))
      : (q.enabled && q.host ? 'nicht verbunden' : 'aus'));
    fill('mqttHost', q.host); fill('mqttPort', q.port); fill('mqttUser', q.user);
  }

  // System
  set('s-fw', d.fw); set('s-mac', d.mac); set('s-heap', Math.round(d.heap / 1024) + ' kB'); set('s-up', fmtDuration(d.uptime));
  fill('devname', d.name);
  set('pw-state', d.web_password_custom ? 'eigenes' : 'Code vom Display');
  if (state.oled) {
    set('oled-page', state.oled.page + ' von ' + state.oled.pages);
    set('oled-btn', state.oled.button ? 'gedrückt' : 'losgelassen');
  }
  const log = $('log');
  if (log) {
    const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
    log.textContent = (state.log || []).join('\n') || '–';
    if (atBottom) log.scrollTop = log.scrollHeight;
  }

  if (m.power_w !== null && m.power_w !== undefined) {
    powerHistory.push(m.power_w); if (powerHistory.length > 120) powerHistory.shift();
  }
  drawSpark();
}

function drawSpark() {
  const c = $('spark'); if (!c || !c.clientWidth) return;
  // Draw at the canvas' real size, so the line is not stretched on a phone.
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
  const ctx = c.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (powerHistory.length < 2) return;
  const min = Math.min(...powerHistory), max = Math.max(...powerHistory);
  const span = (max - min) || 1;
  const pts = powerHistory.map((v, i) => [(i / (powerHistory.length - 1)) * w, h - 4 - ((v - min) / span) * (h - 8)]);
  const fillGrad = ctx.createLinearGradient(0, 0, 0, h);
  fillGrad.addColorStop(0, 'rgba(162,89,255,.25)'); fillGrad.addColorStop(1, 'rgba(91,140,255,0)');
  ctx.beginPath(); ctx.moveTo(pts[0][0], h);
  pts.forEach((p) => ctx.lineTo(p[0], p[1]));
  ctx.lineTo(pts[pts.length - 1][0], h); ctx.closePath();
  ctx.fillStyle = fillGrad; ctx.fill();
  const line = ctx.createLinearGradient(0, 0, w, 0);
  line.addColorStop(0, '#a259ff'); line.addColorStop(1, '#5b8cff');
  ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])));
  ctx.strokeStyle = line; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.stroke();
  const secs = powerHistory.length * 3;
  set('spark-cap', 'Letzte ' + (secs < 120 ? secs + ' s' : Math.round(secs / 60) + ' min') + ' · ' + num(min, 0) + '–' + num(max, 0) + ' W');
}

async function poll() {
  try {
    const res = await fetch(withToken('/api/state'));
    if (!res.ok) throw new Error('http ' + res.status);
    state = await res.json();
    render();
  } catch (e) { /* keep the last good state */ }
}

function selectTab(name) {
  wantedTab = name;
  const btn = document.querySelector('.tabs button[data-tab="' + name + '"]');
  if (!btn || btn.hidden) name = 'status';
  document.querySelectorAll('.tabs button').forEach((b) => b.classList.toggle('on', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach((p) => p.classList.toggle('on', p.id === 'tab-' + name));
  if (location.hash !== '#' + name) history.replaceState(null, '', '#' + name);
  if (name === 'status') drawSpark();
}

// Collects the fields of a form, skipping those hidden for this role.
function collect(fields) {
  const data = {};
  fields.forEach((f) => {
    const el = $(f);
    if (!el || el.closest('[data-only][hidden]')) return;
    data[f] = el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value.trim();
  });
  return data;
}

function bindSave(id, fields, opts) {
  const el = $(id); if (!el) return;
  opts = opts || {};
  el.addEventListener('click', async () => {
    const data = collect(fields);
    if (opts.check) { const err = opts.check(data); if (err) { toast(err, true); return; } }
    el.disabled = true;
    const res = await post('/api/save', data);
    el.disabled = false;
    if (res.ok) {
      // Saved: let the poll show the device's values again, and clear secrets.
      fields.forEach((f) => { const i = $(f); if (!i) return; delete i.dataset.touched; if (i.type === 'password' || opts.clear && opts.clear.indexOf(f) >= 0) i.value = ''; });
    }
    setTimeout(poll, 600);
  });
}

function initApp() {
  // Role-specific parts stay hidden until the first state says which role this is.
  document.querySelectorAll('[data-only]').forEach((el) => { el.hidden = true; });
  // Once edited, a synced field is the owner's until saved.
  document.querySelectorAll('[data-sync]').forEach((el) => {
    const mark = () => { el.dataset.touched = '1'; };
    el.addEventListener('input', mark); el.addEventListener('change', mark);
  });
  document.querySelectorAll('.tabs button').forEach((btn) => btn.addEventListener('click', () => selectTab(btn.dataset.tab)));

  document.querySelectorAll('[data-act]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      if (btn.dataset.confirm && !confirm(btn.dataset.confirm)) return;
      const act = btn.dataset.act;
      btn.disabled = true;
      const res = await post('/api/action', { action: act });
      btn.disabled = false;
      if ((act === 'reboot' || act === 'factory') && res.ok) {
        toast('Gerät startet neu …');
        setTimeout(() => { location.href = act === 'factory' ? '/setup' : '/'; }, 9000);
        return;
      }
      setTimeout(poll, 700);
    });
  });
  document.querySelectorAll('[data-toggle]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const kind = btn.dataset.toggle;
      const on = state && state.meter && (kind === 'sim' ? state.meter.simulating : state.meter.loopback);
      btn.disabled = true;
      await post('/api/action', { action: kind + (on ? '-off' : '-on') });
      btn.disabled = false;
      setTimeout(poll, 500);
    });
  });

  bindSave('saveMeter', ['protocol', 'polarity', 'meterRx', 'meterTx']);
  bindSave('saveRadio', ['freq', 'sf', 'power', 'interval'], {
    check: (d) => (d.freq && (d.freq < 868.0625 || d.freq > 868.5375)) ? 'Frequenz muss zwischen 868,0625 und 868,5375 MHz liegen' : ''
  });
  bindSave('saveWifi', ['ssid', 'pass'], { check: (d) => d.ssid ? '' : 'Bitte ein Netzwerk wählen' });
  bindSave('saveAp', ['apname', 'apcode', 'keepap'], {
    clear: ['apcode'],
    check: (d) => (d.apcode && !/^[A-Za-z0-9]{8,15}$/.test(d.apcode)) ? 'Der Code braucht 8–15 Buchstaben oder Ziffern' : ''
  });
  bindSave('saveAccess', ['newpass'], { check: (d) => (d.newpass.length < 8) ? 'Das Passwort braucht mindestens 8 Zeichen' : '' });
  bindSave('saveDevice', ['devname']);
  bindSave('saveMqtt', ['mqttHost', 'mqttPort', 'mqttUser', 'mqttPass'], { check: (d) => d.mqttHost ? '' : 'Bitte einen Broker eintragen' });
  const mqttOff = $('mqttOff');
  if (mqttOff) mqttOff.addEventListener('click', async () => {
    await post('/api/save', { mqttOff: '1' });
    const host = $('mqttHost'); if (host) { delete host.dataset.touched; host.value = ''; }
    setTimeout(poll, 500);
  });

  const scopeBtn = $('scopeBtn');
  if (scopeBtn) scopeBtn.addEventListener('click', async () => {
    const box = $('scope-out');
    show(box, true); box.textContent = 'messe … (3 × 1 s)';
    scopeBtn.disabled = true;
    try {
      const res = await (await fetch(withToken('/api/action'), {
        method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ action: 'meter-scope' }).toString()
      })).json();
      box.textContent = res.message || 'keine Antwort';
    } catch (e) { box.textContent = 'Messung fehlgeschlagen'; }
    scopeBtn.disabled = false;
  });
  const dumpBtn = $('mqttDumpBtn');
  if (dumpBtn) dumpBtn.addEventListener('click', async () => {
    const box = $('mqtt-dump');
    show(box, true); box.textContent = 'lade …';
    try { box.textContent = await (await fetch(withToken('/api/mqtt'))).text(); }
    catch (e) { box.textContent = 'nicht erreichbar'; }
  });

  // Links and forms that leave the page carry the session token.
  const ota = $('otaForm'); if (ota) ota.action = withToken('/ota');
  ['haYaml', 'haCard', 'setupLink'].forEach((id) => { const a = $(id); if (a) a.href = withToken(a.getAttribute('href')); });
  const logout = $('logoutBtn');
  if (logout) logout.addEventListener('click', async () => {
    sessionStorage.removeItem('sid');
    try { await fetch(withToken('/logout'), { method: 'POST' }); } catch (e) {}
    location.href = '/login';
  });

  initWifiScanner();
  selectTab((location.hash || '#status').slice(1));
  window.addEventListener('resize', drawSpark);
  poll(); setInterval(poll, 3000);
}

// One scanner for both pages: the setup screen and the WLAN tab use the same
// markup, so a running device switches networks the same way it was set up.
function initWifiScanner() {
  const scan = $('scanBtn');
  if (!scan) return;
  scan.addEventListener('click', async () => {
    scan.textContent = 'Suche …'; scan.disabled = true;
    const listEl = $('wifilist');
    try {
      const res = await fetch(withToken('/api/scan'));
      const raw = await res.json();
      // One entry per network name, strongest first: repeaters and mesh nodes
      // otherwise list the same SSID several times.
      const best = {};
      raw.forEach((n) => { if (!best[n.ssid] || best[n.ssid].rssi < n.rssi) best[n.ssid] = n; });
      const list = Object.keys(best).map((k) => best[k]).sort((a, b) => b.rssi - a.rssi);
      listEl.innerHTML = list.length ? list.map((n) =>
        '<button type="button" class="wifi" data-ssid="' + esc(n.ssid) + '"><span>' + esc(n.ssid) + '</span><span class="bars" title="' + n.rssi + ' dBm">' +
        [1, 2, 3, 4].map((i) => '<i class="' + (n.rssi > -50 - (i - 1) * 12 ? 'on' : '') + '" style="height:' + (i * 3 + 2) + 'px"></i>').join('') +
        '</span></button>').join('') : '<p class="muted small">Keine Netzwerke gefunden.</p>';
      listEl.querySelectorAll('.wifi').forEach((row) => row.addEventListener('click', () => {
        listEl.querySelectorAll('.wifi').forEach((x) => x.classList.remove('on'));
        row.classList.add('on');
        const ssid = $('ssid'); ssid.value = row.dataset.ssid; ssid.dataset.touched = '1';
        const pass = $('pass'); if (pass) { pass.value = ''; pass.focus(); }
      }));
    } catch (e) { toast('Suche fehlgeschlagen', true); }
    scan.textContent = 'Suchen'; scan.disabled = false;
  });
}

// ── setup ────────────────────────────────────────────────────
function initSetup() {
  let role = 0;
  initWifiScanner();
  const save = $('saveBtn');

  // Only the sections of the chosen role are shown, and the step numbers follow
  // (CSS counters skip hidden cards): the flat's gateway has no meter head and
  // no permanent access point to configure.
  function pickRole(picked) {
    role = String(picked);
    document.querySelectorAll('[data-role]').forEach((b) => b.classList.toggle('on', b.dataset.role === role));
    document.querySelectorAll('[data-roles]').forEach((el) => { el.hidden = el.dataset.roles.split(' ').indexOf(role) < 0; });
    save.disabled = false;
    set('saveHint', role === '2'
      ? 'Nach dem Speichern verbindet sich das Gateway mit dem Heim-WLAN. Die Seite ist dann unter seiner neuen Adresse erreichbar.'
      : 'Nach dem Speichern startet der Keller-Knoten mit dem Auslesen. Danach beide Geräte unter „Funk“ koppeln.');
  }
  document.querySelectorAll('[data-role]').forEach((btn) => btn.addEventListener('click', () => pickRole(btn.dataset.role)));

  // On a device that is already set up, the page opens with its current values,
  // so changing one thing does not reset the others to defaults.
  let prefilled = false;
  async function prefill() {
    if (prefilled) return;
    try {
      const s = await (await fetch(withToken('/api/state'))).json();
      prefilled = true;
      set('fw', s.device.fw); set('mac', s.device.mac);
      if (s.device.host) set('hostname', s.device.host);
      const val = (id, v) => { const el = $(id); if (el && !el.value && v !== undefined && v !== null && v !== '') el.value = v; };
      val('apname', s.wifi && s.wifi.ap_ssid);
      if (s.device.role === 'unset') return;
      show($('backBtn'), true);
      $('backBtn').href = withToken('/');
      pickRole(s.device.role === 'meter' ? 1 : 2);
      val('name', s.device.name); val('ssid', s.wifi.sta_ssid);
      if (s.radio && s.radio.interval_ms) $('interval').value = Math.round(s.radio.interval_ms / 1000);
      $('protocol').value = String(s.meter.config_protocol || 0);
      $('polarity').value = String(s.meter.config_polarity || 0);
      val('meterRx', s.meter.rx_pin); val('meterTx', s.meter.tx_pin);
      if (s.mqtt) { val('mqttHost', s.mqtt.host); if (s.mqtt.host) val('mqttPort', s.mqtt.port); val('mqttUser', s.mqtt.user); }
      if (s.wifi.sta_ssid) $('pass').placeholder = 'unverändert lassen';
    } catch (e) { setTimeout(prefill, 3000); }
  }
  prefill();

  save.addEventListener('click', async () => {
    if (!role) { toast('Bitte zuerst auswählen, welches Gerät das ist', true); return; }
    const v = (id) => $(id).value.trim();
    if (role === '2' && !v('ssid')) { toast('Das Gateway braucht ein Heim-WLAN', true); $('ssid').focus(); return; }
    if (v('apcode') && !/^[A-Za-z0-9]{8,15}$/.test(v('apcode'))) { toast('Der Zugangscode braucht 8–15 Buchstaben oder Ziffern', true); return; }
    save.disabled = true;
    const data = { role, name: v('name'), ssid: v('ssid'), pass: $('pass').value };
    if (role === '1') Object.assign(data, {
      interval: v('interval'), protocol: v('protocol'), polarity: v('polarity'),
      meterRx: v('meterRx'), meterTx: v('meterTx'), apname: v('apname'), apcode: v('apcode')
    });
    const res = await post('/api/setup', data);
    // MQTT is optional and goes through the normal settings path, which is
    // what turns it on.
    if (res.ok && role === '2' && v('mqttHost')) {
      await post('/api/save', { mqttHost: v('mqttHost'), mqttPort: v('mqttPort'), mqttUser: v('mqttUser'), mqttPass: $('mqttPass').value });
    }
    if (res.ok) finish(res, data);
    else save.disabled = false;
  });

  // After saving, the page says where the device will be found next instead of
  // jumping to an address that may already be gone. A gateway joins the home
  // network while its own access point stays up, so the page can watch the
  // join and show the new address; a renamed access point needs a reconnect.
  function finish(res, data) {
    document.querySelectorAll('.steps, .savebar, #saveHint').forEach((el) => { el.hidden = true; });
    const card = $('doneCard');
    card.hidden = false;
    window.scrollTo(0, 0);
    const rows = (list) => {
      $('doneRows').innerHTML = list.map(([k, v]) => '<div class="row"><span>' + esc(k) + '</span><span class="mono">' + esc(v) + '</span></div>').join('');
    };
    const actions = (list) => {
      $('doneActions').innerHTML = list.map(([href, label, cls]) => '<a class="' + (cls || 'primary') + '" href="' + esc(href) + '">' + esc(label) + '</a>').join('');
    };
    const say = (title, lead, hint) => { set('doneTitle', title); set('doneLead', lead || ''); set('doneHint', hint || ''); };
    const host = 'http://' + (res.host || '') + '/';

    if (role === '1' && res.ap_changed) {
      say('Gespeichert – bitte neu verbinden',
        'Das Geräte-WLAN startet gleich mit den neuen Daten neu. Das Handy mit diesem WLAN verbinden und dann die Adresse öffnen.',
        'Der Code ist WLAN-Passwort und Anmeldecode zugleich. Er steht auch auf dem Display (Seite „Zugang“).');
      rows([['WLAN', res.ap_ssid], ['Code', data.apcode || 'unverändert'], ['Adresse', 'http://' + (res.ap_ip || '192.168.4.1')]]);
      actions([['http://' + (res.ap_ip || '192.168.4.1') + '/', 'Zum Dashboard']]);
      return;
    }
    if (role !== '2' || !res.net_change) {
      say('Gespeichert', 'Weiter zum Dashboard …');
      actions([[withToken('/'), 'Zum Dashboard']]);
      setTimeout(() => { location.href = withToken('/'); }, 1500);
      return;
    }

    say('Verbinde mit „' + res.sta_ssid + '“ …',
      'Das Gateway meldet sich jetzt im Heim-WLAN an. Diese Seite bleibt mit dem Einrichtungs-WLAN verbunden und zeigt gleich die neue Adresse.');
    rows([['Heim-WLAN', res.sta_ssid]]);
    actions([]);
    const started = Date.now();
    let lastOk = Date.now();
    async function poll() {
      try {
        const s = await (await fetch(withToken('/api/state'), { cache: 'no-store' })).json();
        lastOk = Date.now();
        if (s.wifi && s.wifi.connected && s.wifi.ip && s.wifi.ip !== '-') {
          const ip = 'http://' + s.wifi.ip + '/';
          say('Verbunden', 'Jetzt das Handy wieder mit „' + res.sta_ssid + '“ verbinden und die neue Adresse öffnen.',
            'Das Einrichtungs-WLAN „' + res.ap_ssid + '“ schaltet sich in etwa zwei Minuten ab. Die Adresse steht auch auf dem Display.');
          rows([['Heim-WLAN', res.sta_ssid], ['Adresse', s.wifi.ip], ['Name', res.host]]);
          actions([[ip, 'Zur neuen Adresse'], [host, res.host, 'ghost']]);
          return;
        }
      } catch (e) { /* the phone may hop back onto the access point; keep trying */ }
      if (Date.now() - lastOk > 30000) {
        say('Verbindung zum Gerät verloren',
          'Wahrscheinlich ist das Gateway schon im Heim-WLAN. Das Handy mit „' + res.sta_ssid + '“ verbinden und den Namen öffnen – die IP-Adresse steht auch auf dem Display.');
        rows([['Heim-WLAN', res.sta_ssid], ['Name', res.host]]);
        actions([[host, 'Zum Gateway']]);
        return;
      }
      if (Date.now() - started > 60000) {
        say('Noch nicht verbunden',
          'Das Gateway kommt nicht ins WLAN „' + res.sta_ssid + '“. Meist stimmt das Passwort nicht, oder das Netz ist nur 5 GHz (das Gerät kann nur 2,4 GHz).',
          'Es versucht es weiter. Diese Seite bleibt über das Einrichtungs-WLAN erreichbar.');
        actions([[withToken('/setup?edit=1'), 'Einstellungen korrigieren', 'ghost']]);
      }
      setTimeout(poll, 2000);
    }
    setTimeout(poll, 4000);
  }
}

if ($('tab-status')) initApp();
else if ($('saveBtn')) initSetup();
)JS";
