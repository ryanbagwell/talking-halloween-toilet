#pragma once

// Remote-control page, served at / over HTTP and HTTPS
const char INDEX_HTML[] = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Burping Toilet</title>
<style>
:root{--bg:#f4f6f3;--card:#fff;--text:#1d2420;--muted:#5b665f;--accent:#2f8f4e;--accent-text:#fff;--line:#d9dfda}
@media (prefers-color-scheme:dark){:root{--bg:#121614;--card:#1c221f;--text:#e8eee9;--muted:#9aa69e;--accent:#5fd27f;--accent-text:#0d1a12;--line:#2c3530}}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}
main{max-width:420px;margin:0 auto;padding:20px 16px}
h1{font-size:1.4rem;margin:0 0 4px}
#info{color:var(--muted);margin:0 0 20px;min-height:1.4em}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
button{font:inherit;font-weight:600;padding:18px 12px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--text)}
button:active{transform:scale(.97)}
#burp{grid-column:span 2;background:var(--accent);color:var(--accent-text);border-color:var(--accent);font-size:1.25rem;padding:26px}
label{display:block;margin:24px 0 8px;color:var(--muted)}
input[type=range]{width:100%}
a{color:var(--muted)}
footer{margin-top:28px;font-size:.9rem}
</style></head><body><main>
<h1>Burping Toilet</h1>
<p id="info">Connecting...</p>
<div class="grid">
<button id="burp" onclick="send('b')">Burp!</button>
<button onclick="send('o')">Open lid</button>
<button onclick="send('c')">Close lid</button>
<button onclick="send('s')">Sound only</button>
<button id="arm" onclick="send('p')">Sensor</button>
</div>
<label for="angle">Servo angle: <span id="angleVal">-</span>&deg;</label>
<input id="angle" type="range" min="0" max="180" step="1">
<footer><a href="/talk">Talk through the toilet</a> &middot; <a href="/mictest">Microphone test</a></footer>
</main><script>
const $ = id => document.getElementById(id);
function send(c) {
  fetch('/cmd', {method: 'POST', body: new URLSearchParams({c})}).then(() => setTimeout(poll, 300));
}
function poll() {
  fetch('/status').then(r => r.json()).then(s => {
    const state = !s.armed ? 'Sensor paused'
      : s.waiting ? 'Sensor on - waiting for the area to clear'
      : 'Sensor on - waiting for a victim';
    $('info').textContent = state + ' (' + (s.dist >= 80 ? '80+' : s.dist) + ' cm)';
    $('arm').textContent = s.armed ? 'Pause sensor' : 'Resume sensor';
    if (document.activeElement !== $('angle')) {
      $('angle').value = s.lid;
      $('angleVal').textContent = s.lid;
    }
  }).catch(() => $('info').textContent = 'Not connected');
}
$('angle').oninput = e => $('angleVal').textContent = e.target.value;
$('angle').onchange = e => send('a' + e.target.value);
poll();
setInterval(poll, 1000);
</script></body></html>
)rawliteral";


// Talk page (HTTPS only): the phone's browser hears the guest, the local LLM writes a
// reply, and ElevenLabs or Kokoro gives the toilet its voice.
const char TALK_HTML[] = R"rawliteral(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Talking Toilet</title>
<style>
:root{--bg:#f4f6f3;--card:#fff;--text:#1d2420;--muted:#5b665f;--accent:#2f8f4e;--accent-text:#fff;--line:#d9dfda;--rec:#c2412d;--rec-text:#fff;--guest:#e7ece8}
@media (prefers-color-scheme:dark){:root{--bg:#121614;--card:#1c221f;--text:#e8eee9;--muted:#9aa69e;--accent:#5fd27f;--accent-text:#0d1a12;--line:#2c3530;--rec:#ff7a63;--rec-text:#1a0d0a;--guest:#252c28}}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}
main{max-width:460px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:1.4rem;margin:0 0 4px}
.muted{color:var(--muted);margin:0}
#state{min-height:1.4em;margin:12px 0 16px;font-weight:600}
button{font:inherit;font-weight:600;padding:14px 12px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--text)}
button:active{transform:scale(.97)}
button:disabled{opacity:.5}
#talk{display:block;width:100%;padding:44px 12px;font-size:1.3rem;background:var(--accent);color:var(--accent-text);border-color:var(--accent);
  touch-action:none;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
#talk.rec{background:var(--rec);border-color:var(--rec);color:var(--rec-text)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}
#log{margin:20px 0 8px;display:flex;flex-direction:column;gap:8px}
.line{padding:10px 12px;border-radius:12px;max-width:85%}
.line b{display:block;font-size:.8rem;color:var(--muted);font-weight:600}
.guest{background:var(--guest);align-self:flex-start}
.toilet{background:var(--card);border:1px solid var(--line);align-self:flex-end}
#timing{font-size:.85rem}
details{margin-top:24px;border-top:1px solid var(--line);padding-top:12px}
summary{font-weight:600;cursor:pointer}
label{display:block;margin:14px 0 6px;color:var(--muted);font-size:.9rem}
input,textarea,select{box-sizing:border-box;width:100%;font:inherit;padding:10px;border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--text)}
textarea{min-height:110px;resize:vertical}
#save{margin-top:16px;width:100%}
a{color:var(--muted)}
#talkDots{display:none}
/* The root page (/): just the talk button, filling the screen, with dots while it works */
body.simple main{max-width:none;height:100vh;height:100dvh;box-sizing:border-box;padding:16px;display:flex}
body.simple main>:not(#talk){display:none}
body.simple #talk{flex:1;font-size:2rem;border-radius:24px}
body.simple.working #talkDots{display:flex;justify-content:center;gap:10px;margin-top:20px}
#talkDots i{width:12px;height:12px;border-radius:50%;background:currentColor;animation:dot 1.2s infinite ease-in-out}
#talkDots i:nth-child(2){animation-delay:.2s}
#talkDots i:nth-child(3){animation-delay:.4s}
@keyframes dot{0%,80%,100%{opacity:.25;transform:scale(.8)}40%{opacity:1;transform:scale(1)}}
@media (prefers-reduced-motion:reduce){#talkDots i{animation:none;opacity:.7}}
</style></head><body><main>
<h1>Talking Toilet</h1>
<p class="muted" id="toilet">Connecting to the toilet...</p>
<p id="state">Hold the button while the guest talks.</p>
<button id="talk"><span id="talkLabel">Hold to talk</span><span id="talkDots"><i></i><i></i><i></i></span></button>
<div class="row">
<button id="burp">Burp!</button>
<button id="reset">New conversation</button>
</div>
<div id="log"></div>
<p class="muted" id="timing"></p>

<details id="settings">
<summary>Settings</summary>
<label for="prompt">Personality</label>
<textarea id="prompt"></textarea>
<p class="muted" id="localInfo"></p>
<label for="localModel">Local model name (optional)</label>
<input id="localModel" autocomplete="off" spellcheck="false">
<label for="ttsProvider">Toilet voice from</label>
<select id="ttsProvider">
<option value="elevenlabs">ElevenLabs</option>
<option value="kokoro">Local Kokoro</option>
</select>
<div id="elevenFields">
<label for="elevenVoice">ElevenLabs voice ID (from the voice's menu in ElevenLabs)</label>
<input id="elevenVoice" list="elevenVoices" autocomplete="off" spellcheck="false">
<datalist id="elevenVoices"><option value="aA3082oYudyOqwtlJRIh">The toilet's voice</option><option value="JBFqnCBsd6RMkjVDRZzb">George</option><option value="N2lVS1w4EtoT3dr4eOWO">Callum</option><option value="pqHfZKP75CvOlQylNhV4">Bill</option><option value="nPczCjzI2devNBz1zQrb">Brian</option><option value="onwK4e9ZLuTAKqWW03F9">Daniel</option></datalist>
<label for="elevenModel">ElevenLabs model</label>
<input id="elevenModel" list="elevenModels" autocomplete="off" spellcheck="false">
<datalist id="elevenModels"><option value="eleven_flash_v2_5">Flash: fastest, cheapest</option><option value="eleven_multilingual_v2">Multilingual: richest, slower</option></datalist>
</div>
<div id="kokoroFields">
<label for="kokoroVoice">Kokoro voice</label>
<input id="kokoroVoice" list="voices" autocomplete="off" spellcheck="false">
<datalist id="voices"><option>am_onyx</option><option>am_fenrir</option><option>am_santa</option><option>am_echo</option><option>am_adam</option><option>am_eric</option><option>am_liam</option><option>am_michael</option><option>am_puck</option><option>bm_george</option><option>bm_lewis</option><option>bm_daniel</option><option>bm_fable</option><option>af_heart</option><option>af_bella</option><option>bf_emma</option></datalist>
</div>
<label for="speed">Voice speed (lower is slower and spookier; ElevenLabs 0.7 to 1.2, Kokoro 0.5 to 2)</label>
<input id="speed" inputmode="decimal" autocomplete="off">
<p class="muted" id="ttsInfo"></p>
<label for="lang">Speech recognition language</label>
<input id="lang" autocomplete="off" spellcheck="false">
<button id="save">Save settings</button>
</details>
<p><a href="/debug">Back to controls</a></p>
</main><script>
const $ = id => document.getElementById(id);
if (location.pathname === '/') document.body.classList.add('simple');  // root page: just the button
const DEFAULTS = {
  prompt: 'You are a haunted toilet at a Halloween party. You are grumpy, dramatic and a little gross, ' +
    'and you love toilet puns, burps and gurgles. Stay family-friendly: no swearing and nothing truly ' +
    'frightening for young children. Reply in one or two short sentences.',
  ttsProvider: 'elevenlabs',
  elevenVoice: 'aA3082oYudyOqwtlJRIh',
  elevenModel: 'eleven_flash_v2_5',
  kokoroVoice: 'am_onyx',
  speed: '0.9',
  lang: 'en-US',
  localModel: '',
};
const FIELDS = Object.keys(DEFAULTS);
const Recognition = window.SpeechRecognition || window.webkitSpeechRecognition;

let settings = loadSettings();
let config = {localLlm: '', localTts: '', elevenlabsKey: ''};  // from the toilet's firmware (secrets.h / constants.h)
let history = [];
let sayState = null;   // the reply the toilet is currently speaking, if any
let recognition = null;
let heardSoFar = '';   // text from recognizers that already ended during this press
let lastHeard = '';    // text from the current recognizer, final and in-progress
let finished = true;   // whether this press's text has been handed on
let holding = false, busy = false, micUsed = false, wakeLock = null;

// Only known settings, so values from older versions of this page (like a saved API key) are dropped
function loadSettings() {
  const out = {...DEFAULTS};
  try {
    const saved = JSON.parse(localStorage.getItem('toiletSettings') || '{}');
    FIELDS.forEach(k => { if (typeof saved[k] === 'string') out[k] = saved[k]; });
  } catch (e) {}
  return out;
}
FIELDS.forEach(k => $(k).value = settings[k]);

function showVoiceFields() {
  const eleven = $('ttsProvider').value === 'elevenlabs';
  $('elevenFields').hidden = !eleven;
  $('kokoroFields').hidden = eleven;
  $('ttsInfo').textContent = eleven
    ? (config.elevenlabsKey ? 'The phone gets the voice from ElevenLabs and sends it to the toilet.' : NO_ELEVENLABS_KEY_MESSAGE)
    : 'The toilet gets its voice from ' + (config.localTts || 'nowhere yet') + ' (LOCAL_TTS_URL in constants.h).';
}
$('ttsProvider').onchange = showVoiceFields;

fetch('/config').then(r => r.json()).then(c => {
  config = c;
  $('localInfo').textContent = c.localLlm
    ? 'The toilet relays chat to ' + c.localLlm + ' (LOCAL_LLM_URL in constants.h).'
    : 'Set LOCAL_LLM_URL in constants.h to get chat replies.';
  showVoiceFields();
  if (!c.elevenlabsKey && settings.ttsProvider === 'elevenlabs') setState(NO_ELEVENLABS_KEY_MESSAGE);
}).catch(() => setState("Couldn't load the toilet's settings - is it on Wi-Fi?"));

const NO_ELEVENLABS_KEY_MESSAGE = 'Set ELEVENLABS_API_KEY in secrets.h and re-upload, or switch the voice to local Kokoro';
showVoiceFields();

$('save').onclick = () => {
  FIELDS.forEach(k => settings[k] = $(k).value.trim() || DEFAULTS[k]);
  try { localStorage.setItem('toiletSettings', JSON.stringify(settings)); } catch (e) {}
  $('settings').open = false;
  setState('Settings saved');
};

function setState(text) {
  $('state').textContent = text;
  showWorking();
}

// Dots on the root page from release until the toilet has finished its reply
function showWorking() { document.body.classList.toggle('working', busy || !!sayState); }

function addLine(who, text) {
  const div = document.createElement('div');
  div.className = 'line ' + who;
  const name = document.createElement('b');
  name.textContent = who === 'guest' ? 'Guest' : 'Toilet';
  div.append(name, text);
  $('log').append(div);
  div.scrollIntoView({block: 'nearest'});
}

async function keepAwake() {
  try {
    if ('wakeLock' in navigator && !wakeLock) {
      wakeLock = await navigator.wakeLock.request('screen');
      wakeLock.addEventListener('release', () => wakeLock = null);
    }
  } catch (e) {}
}
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && micUsed) keepAwake();
});

// ---- Hearing (hold the button; the browser does the speech recognition) ----
const joinText = (a, b) => [a, b].filter(Boolean).join(' ');

function resetButton() {
  talkButton.classList.remove('rec');
  $('talkLabel').textContent = 'Hold to talk';
}

// Hand this press's text on exactly once
function finish() {
  if (finished) return;
  finished = true;
  handleHeard(joinText(heardSoFar, lastHeard));
}

function listen() {
  const r = new Recognition();
  recognition = r;
  r.lang = settings.lang;
  r.continuous = true;
  r.interimResults = true;
  lastHeard = '';
  let failed = false;

  r.onresult = e => {
    let text = '';
    for (const result of e.results) text += result[0].transcript;
    lastHeard = text.trim();
    setState('Hearing: "' + joinText(heardSoFar, lastHeard) + '"');
  };
  r.onerror = e => {
    if (e.error === 'aborted' || e.error === 'no-speech') return;  // empty text is handled on release
    failed = true;
    holding = false;
    finished = true;
    busy = false;
    resetButton();
    setState(e.error === 'not-allowed' || e.error === 'service-not-allowed'
      ? 'Speech recognition is blocked. Allow the microphone and turn on Dictation in Settings.'
      : 'Speech recognition error: ' + e.error);
  };
  r.onend = () => {
    if (failed || r !== recognition) return;
    if (holding) {
      // iOS stopped listening on its own (e.g. after a pause); keep going
      heardSoFar = joinText(heardSoFar, lastHeard);
      listen();
      return;
    }
    finish();
  };
  r.start();
}

function startTalk(e) {
  e.preventDefault();
  if (busy || holding || !Recognition) return;
  if (!config.elevenlabsKey && settings.ttsProvider === 'elevenlabs') {
    setState(NO_ELEVENLABS_KEY_MESSAGE);
    return;
  }
  holding = true;
  micUsed = true;
  finished = false;
  heardSoFar = '';
  talkButton.setPointerCapture(e.pointerId);
  keepAwake();
  try {
    listen();
  } catch (err) {
    holding = false;
    finished = true;
    setState("Speech recognition didn't start: " + err.message);
    return;
  }
  talkButton.classList.add('rec');
  $('talkLabel').textContent = 'Listening... release to send';
  setState('Listening');
}

function stopTalk(e) {
  e.preventDefault();
  if (!holding) return;
  holding = false;
  busy = true;  // until this press's reply is done
  resetButton();
  setState('Finishing...');
  const r = recognition;
  r.stop();
  // If Safari never reports the end, go with what was heard so far
  setTimeout(() => {
    if (finished || r !== recognition) return;
    r.onend = null;
    try { r.abort(); } catch (err) {}
    finish();
  }, 3000);
}

const talkButton = $('talk');
talkButton.addEventListener('pointerdown', startTalk);
['pointerup', 'pointercancel'].forEach(ev => talkButton.addEventListener(ev, stopTalk));
talkButton.addEventListener('contextmenu', e => e.preventDefault());
if (!Recognition) {
  talkButton.disabled = true;
  setState("This browser doesn't have speech recognition");
}

// ---- Heard text -> local LLM -> toilet ----
async function handleHeard(heard) {
  busy = true;
  talkButton.disabled = true;
  const t0 = performance.now();
  const secs = (a, b) => ((b - a) / 1000).toFixed(1) + ' s';
  try {
    if (!heard) {
      setState("Didn't catch any words - try again");
      return;
    }
    addLine('guest', heard);

    setState('Thinking...');
    const reply = await chat(heard);
    const t1 = performance.now();
    addLine('toilet', reply);

    const eleven = settings.ttsProvider === 'elevenlabs';
    setState(eleven ? 'Finding its voice...' : 'Sending to the toilet...');
    await say(reply);
    // The toilet is speaking (ElevenLabs) or fetching the voice from Kokoro; poll() follows along from here
    const t2 = performance.now();
    sayState = {start: t0, sent: t2, replyTime: secs(t0, t1), voiceTime: eleven ? secs(t1, t2) : null, spoke: false};
    setState(eleven ? 'Toilet is talking' : 'Finding its voice...');
    $('timing').textContent = 'Reply ' + secs(t0, t1);
  } catch (err) {
    console.error(err);
    setState(err.message);
  } finally {
    busy = false;
    talkButton.disabled = false;
    showWorking();
  }
}

async function chat(text) {
  const messages = [{role: 'system', content: settings.prompt}, ...history, {role: 'user', content: text}];
  const data = await localChat(messages);
  // Some local models think out loud in <think> tags; the toilet shouldn't say that part
  const reply = (data.choices?.[0]?.message?.content || '').replace(/<think>[\s\S]*?<\/think>/g, '').trim();
  if (!reply) throw new Error('The chat model sent back an empty reply - try again');
  history = [...history, {role: 'user', content: text}, {role: 'assistant', content: reply}].slice(-10);
  return reply;
}

// Chat replies from llama.cpp's server (/v1/chat/completions). The toilet relays the
// request, because this HTTPS page isn't allowed to call an http:// address itself.
async function localChat(messages) {
  const body = {messages, max_tokens: 200};
  if (settings.localModel) body.model = settings.localModel;
  let res;
  try {
    res = await fetch('/llm', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
  } catch (e) {
    throw new Error("Couldn't reach the toilet - is it on Wi-Fi?");
  }
  if (res.status === 401) throw new Error('The local LLM rejected the API key - check LOCAL_LLM_API_KEY in secrets.h');
  if (!res.ok) throw new Error('Local LLM: ' + await relayError(res));
  return res.json();
}

// A readable message from a failed /llm relay or ElevenLabs request: the toilet's own errors are
// plain text, and the servers' errors are JSON (llama.cpp: error.message, Kokoro and ElevenLabs: detail.message).
async function relayError(res) {
  const text = await res.text();
  try {
    const err = JSON.parse(text);
    const msg = err.error?.message || err.detail?.message || err.detail || err.message;
    if (msg) return typeof msg === 'string' ? msg : JSON.stringify(msg);
  } catch (e) {}
  return text || 'HTTP ' + res.status;
}

// Hand the reply to the toilet. With ElevenLabs, this page fetches the voice as 16 kHz
// audio (the format /speak plays) and sends it on. With Kokoro, the toilet fetches the
// voice itself and plays it as it streams in, which is much faster than fetching the
// audio here and sending it back over this HTTPS connection.
async function say(text) {
  const speed = parseFloat(settings.speed) || 1;
  let path, body, type = 'application/json';
  if (settings.ttsProvider === 'elevenlabs') {
    path = '/speak';
    body = await elevenLabsVoice(text, Math.min(1.2, Math.max(0.7, speed)));
    type = 'application/octet-stream';
    setState('Sending to the toilet...');
  } else {
    path = '/say';
    body = JSON.stringify({model: 'kokoro', voice: settings.kokoroVoice, input: text,
                           speed: Math.min(2, Math.max(0.5, speed)), response_format: 'pcm', stream: true});
  }
  let res;
  try {
    res = await fetch(path, {method: 'POST', headers: {'Content-Type': type}, body});
  } catch (e) {
    throw new Error("Couldn't reach the toilet - is it on Wi-Fi?");
  }
  if (res.status === 409) throw new Error('The toilet is still talking - try again in a moment');
  if (!res.ok) throw new Error('Toilet error: ' + await res.text());
}

const MAX_SPEECH_BYTES = 16000 * 2 * 30;  // MAX_SPEECH_BYTES in constants.h

// Raw 16 kHz, 16-bit mono audio of the reply from ElevenLabs
async function elevenLabsVoice(text, speed) {
  const url = 'https://api.elevenlabs.io/v1/text-to-speech/' + encodeURIComponent(settings.elevenVoice) +
              '?output_format=pcm_16000';
  let res;
  try {
    res = await fetch(url, {
      method: 'POST',
      headers: {'xi-api-key': config.elevenlabsKey, 'Content-Type': 'application/json'},
      body: JSON.stringify({text, model_id: settings.elevenModel, voice_settings: {speed}}),
    });
  } catch (e) {
    throw new Error("Couldn't reach ElevenLabs - check the phone's internet connection");
  }
  if (res.status === 401) throw new Error('ElevenLabs rejected the API key - check ELEVENLABS_API_KEY in secrets.h');
  if (!res.ok) throw new Error('ElevenLabs: ' + await relayError(res));
  const audio = await res.arrayBuffer();
  if (audio.byteLength > MAX_SPEECH_BYTES) throw new Error('That reply is too long for the toilet (over 30 seconds)');
  return audio;
}

// ---- Other controls ----
$('burp').onclick = () => fetch('/cmd', {method: 'POST', body: new URLSearchParams({c: 'b'})});
$('reset').onclick = () => {
  history = [];
  $('log').replaceChildren();
  $('timing').textContent = '';
  setState('New conversation - the toilet has forgotten everything');
};

function poll() {
  const asked = performance.now();
  fetch('/status').then(r => r.json()).then(s => {
    $('toilet').textContent = !s.talking ? 'Toilet ready · sensor ' + (s.armed ? 'on' : 'paused')
      : s.speaking ? 'The toilet is talking' : 'The toilet is finding its voice';
    // Follow the reply the toilet is speaking (ignore answers to polls sent before it)
    if (!sayState || asked < sayState.sent) return;
    const now = performance.now();
    const secs = (a, b) => ((b - a) / 1000).toFixed(1) + ' s';
    if (s.speaking && !sayState.spoke) {
      sayState.spoke = true;
      setState('Toilet is talking');
      $('timing').textContent = 'Reply ' + sayState.replyTime + ' · Voice ' + (sayState.voiceTime || secs(sayState.sent, now)) +
        ' · First words after ' + secs(sayState.start, now);
      console.log($('timing').textContent);
    }
    if (!s.talking) {
      setState(s.voiceError ? 'Voice: ' + s.voiceError
        : sayState.spoke ? 'Hold the button while the guest talks.'
        : 'The toilet finished without speaking');
      sayState = null;
      showWorking();
    }
  }).catch(() => $('toilet').textContent = 'Toilet not reachable');
}
poll();
setInterval(poll, 1000);
</script></body></html>
)rawliteral";
