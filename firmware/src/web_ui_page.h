#pragma once

// Single-page ESP32 Web UI (spec ch. 14.1/14.3): inline CSS/JS, no external
// dependencies. Served at GET / and /ui. All API calls carry the mc_session
// cookie (same-origin fetch), which the auth layer treats as the ADMIN role.
//
// Display-state rules of spec 14.3 are implemented in renderState()/renderTuple():
// labels and colors are exactly the mandated set, text is always redundant
// with color, and no completion vocabulary appears on the unverified path.
static const char kWebUiPage[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MacControl</title>
<style>
:root{--verified:#4A6FA5;--unverified:#7A8B99;--stale:#8BA3C7;--off:#6B8CBB;--unknown:#2E4A62;--ink:#1f2733;--bg:#f4f5f7;--card:#fff;--line:#d9dee5}
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--ink);font-size:14px}
header{background:#26313f;color:#e8ecf1;padding:10px 16px;display:flex;align-items:baseline;gap:12px;flex-wrap:wrap}
header h1{font-size:16px;margin:0;font-weight:600}
header .dev{color:#9fb3c8}
nav{background:#fff;border-bottom:1px solid var(--line);padding:0 16px;display:flex;gap:2px}
nav button{border:0;background:none;padding:10px 14px;cursor:pointer;font-size:14px;color:var(--ink);border-bottom:2px solid transparent}
nav button.on{border-bottom-color:var(--verified);color:var(--verified);font-weight:600}
main{padding:16px;max-width:1100px;margin:0 auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:14px;margin-bottom:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:#5c6b7a;margin:0 0 10px}
table{width:100%;border-collapse:collapse}
td,th{padding:5px 8px;text-align:left;border-bottom:1px solid #eceff3;vertical-align:top}
th{color:#5c6b7a;font-weight:600;font-size:12px}
button.act{background:var(--verified);color:#fff;border:0;border-radius:4px;padding:6px 12px;cursor:pointer;font-size:13px}
button.sec{background:#fff;color:var(--ink);border:1px solid var(--line);border-radius:4px;padding:5px 11px;cursor:pointer;font-size:13px}
button.dgr{background:#fff;color:#8a3b3b;border:1px solid #d8b4b4;border-radius:4px;padding:5px 11px;cursor:pointer;font-size:13px}
button.sm{padding:2px 7px;font-size:12px}
.chip{display:inline-block;padding:1px 8px;border-radius:10px;color:#fff;font-size:12px}
.chip.verified{background:var(--verified)} .chip.unverified{background:var(--unverified)}
.chip.stale{background:var(--stale)} .chip.off{background:var(--off)} .chip.unknown{background:var(--unknown)}
.badge{display:inline-block;border:1px solid var(--stale);color:var(--stale);border-radius:3px;font-size:10px;padding:0 4px;margin-left:6px;text-transform:uppercase}
.muted{color:#7b8794;font-size:12px}
.err{background:#f7eaea;border:1px solid #d8b4b4;color:#7d3232;border-radius:4px;padding:8px 10px;margin:8px 0;white-space:pre-wrap}
.warn{background:#fdf3e0;border:1px solid #e4c98a;color:#7a5b16;border-radius:4px;padding:8px 10px;margin:8px 0}
.ok{background:#e9f0e9;border:1px solid #b9d0b9;color:#2f5d2f;border-radius:4px;padding:8px 10px;margin:8px 0}
input,select,textarea{width:100%;padding:6px 8px;border:1px solid var(--line);border-radius:4px;font-size:13px;font-family:inherit}
label{display:block;font-size:12px;color:#5c6b7a;margin:8px 0 3px}
.row{display:flex;gap:10px;flex-wrap:wrap}
.row>div{flex:1;min-width:180px}
#login{max-width:380px;margin:60px auto}
.tabs{display:none}
.power button{margin:0 6px 6px 0}
.cmdlink{font-size:12px;margin-left:8px}
.paircode{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:34px;letter-spacing:.18em;text-align:center;padding:16px 10px;background:#f0f3f7;border:1px solid var(--line);border-radius:6px;margin:10px 0}
.paircount{text-align:center;font-size:15px;margin:6px 0}
footer{padding:8px 16px;color:#8a97a5;font-size:11px}
</style>
</head>
<body>
<header><h1>MacControl</h1><span class="dev" id="devname"></span><span style="flex:1"></span><span class="muted" id="modeinfo"></span><button class="sec sm" id="logoutbtn" style="display:none" onclick="logout()">Log out</button></header>

<div id="login" class="card">
  <h2 id="logintitle">Sign in</h2>
  <div id="loginerr"></div>
  <label for="pw">Admin password</label>
  <input type="password" id="pw" autocomplete="current-password">
  <div style="margin-top:12px"><button class="act" onclick="doLogin()" id="loginbtn">Sign in</button></div>
  <p class="muted" id="loginhint"></p>
</div>

<div class="tabs" id="app">
<nav>
  <button class="on" data-tab="dash" onclick="showTab('dash')">Dashboard</button>
  <button data-tab="device" onclick="showTab('device')">Device</button>
  <button data-tab="macros" onclick="showTab('macros')">Macros</button>
  <button data-tab="triggers" onclick="showTab('triggers')">Triggers</button>
  <button data-tab="pairing" onclick="showTab('pairing')">Pairing</button>
  <button data-tab="security" onclick="showTab('security')">Security</button>
  <button data-tab="logs" onclick="showTab('logs')">Logs</button>
</nav>
<main>
  <div id="err-global"></div>
  <div class="warn" id="httpwarn" style="display:none"><b>Unencrypted HTTP.</b> API keys and the admin password traverse the LAN in cleartext. Restrict this endpoint to a trusted management network (spec 13.1.1).</div>

  <section id="tab-dash">
    <div class="row">
      <div class="card" style="flex:1"><h2>Connection</h2><table id="conn-table"></table></div>
      <div class="card" style="flex:1"><h2>Mac (agent-reported)</h2><table id="mac-table"></table></div>
    </div>
    <div class="card"><h2>Last command</h2><div id="lastcmd" class="muted">No data</div></div>
    <div class="card"><h2>Run macro (webui_button triggers)</h2><div id="runbtns" class="power"><span class="muted">No enabled webui_button triggers</span></div></div>
    <div class="card"><h2>Power commands</h2><div class="power">
      <button class="act" onclick="powerCmd('wake')">Wake</button>
      <button class="act" onclick="powerCmd('sleep')">Sleep</button>
      <button class="act" onclick="powerCmd('restart')">Restart</button>
      <button class="act" onclick="powerCmd('shutdown')">Shutdown</button>
      <button class="act" onclick="powerCmd('lock')">Lock</button>
      <span id="powerresult"></span>
    </div></div>
  </section>

  <section id="tab-device" style="display:none">
    <div class="card"><h2>Device identity</h2><div id="devmsg"></div>
      <div class="row">
        <div><label for="id-name">Name</label><input id="id-name"></div>
        <div><label for="id-hostname">Hostname</label><input id="id-hostname"></div>
      </div>
      <div class="row">
        <div><label for="id-location">Location</label><input id="id-location"></div>
        <div><label for="id-description">Description</label><input id="id-description"></div>
      </div>
      <div style="margin-top:12px"><button class="act" onclick="saveIdentity()">Save identity</button></div>
      <p class="muted">device_id is immutable. A hostname change re-announces mDNS within 2 s.</p>
    </div>
  </section>

  <section id="tab-macros" style="display:none">
    <div class="card"><h2>Macros</h2><div id="macromsg"></div>
      <table id="macrolist"></table>
      <div style="margin-top:10px"><button class="sec" onclick="editMacro(null)">New macro</button></div>
    </div>
    <div class="card" id="macroeditor" style="display:none">
      <h2 id="macroedtitle">Edit macro</h2><div id="macroedmsg"></div>
      <div class="row">
        <div><label for="m-name">Name (1-32 chars)</label><input id="m-name"></div>
        <div><label for="m-timeout">Timeout ms (500-60000)</label><input id="m-timeout" type="number" value="10000"></div>
      </div>
      <label>Expected event (optional Mode B verification declaration)</label>
      <div class="row">
        <div><select id="m-evtype"><option value="">(none — always terminates Unverified)</option>
          <option>application_started</option><option>application_exited</option>
          <option>system_state_changed</option><option>user_session_changed</option>
          <option>screen_lock_changed</option></select></div>
        <div><input id="m-evmatch" placeholder='match JSON, e.g. {"bundle_id":"com.example.app"}'></div>
      </div>
      <label>Steps (executed in ascending order)</label>
      <table id="steptable"><thead><tr><th>#</th><th>Type</th><th>Key</th><th>Modifiers</th><th>Text value</th><th>Delay ms</th><th></th></tr></thead><tbody></tbody></table>
      <button class="sec sm" onclick="addStep()">Add step</button>
      <div style="margin-top:12px">
        <button class="act" onclick="saveMacro()">Save macro</button>
        <button class="sec" onclick="cancelMacroEdit()">Cancel</button>
      </div>
    </div>
  </section>

  <section id="tab-triggers" style="display:none">
    <div class="card"><h2>Triggers</h2><div id="trigmsg"></div>
      <table id="triggerlist"></table>
      <p class="muted">The <b>http_button</b> source is implicit: POST /api/v1/macros/{id}/execute needs no stored row. The RUN buttons on the Dashboard fire <b>webui_button</b> triggers.</p>
      <div style="margin-top:10px"><button class="sec" onclick="editTrigger(null)">New trigger</button></div>
    </div>
    <div class="card" id="triggereditor" style="display:none">
      <h2 id="trigedtitle">Edit trigger</h2><div id="trigedmsg"></div>
      <div class="row">
        <div><label for="t-source">Source</label><select id="t-source" onchange="trigSourceChanged()">
          <option value="webui_button">webui_button (Dashboard RUN button)</option>
          <option value="gpio">gpio (physical input)</option></select></div>
        <div><label for="t-macro">Macro</label><select id="t-macro"></select></div>
        <div><label for="t-enabled">Enabled</label><select id="t-enabled"><option value="true">enabled</option><option value="false">disabled</option></select></div>
      </div>
      <div class="row" id="t-gpiofields">
        <div><label for="t-pin">GPIO pin (0-21)</label><input id="t-pin" type="number" min="0" max="21"></div>
        <div><label for="t-edge">Edge</label><select id="t-edge"><option value="falling">falling</option><option value="rising">rising</option></select></div>
        <div><label for="t-debounce">Debounce ms (10-500)</label><input id="t-debounce" type="number" value="50" min="10" max="500"></div>
      </div>
      <div style="margin-top:12px">
        <button class="act" onclick="saveTrigger()">Save trigger</button>
        <button class="sec" onclick="cancelTriggerEdit()">Cancel</button>
      </div>
    </div>
  </section>

  <section id="tab-pairing" style="display:none">
    <div class="card"><h2>Pairing state</h2><div id="pairmsg"></div>
      <div id="pairstatus"></div>
      <div id="pairmode" class="muted" style="margin-top:6px"></div>
      <table id="pairrecord" style="margin-top:10px"></table>
      <p class="muted" style="margin-top:10px">Pairing binds a MacControlAgent instance to this endpoint. The agent token lives on the Mac and is never displayed or requested here.</p>
    </div>
    <div class="card" id="pairwindowcard" style="display:none"><h2>Open pairing window</h2>
      <div class="row">
        <div><label for="p-duration">Window duration (seconds, 60-600)</label><input id="p-duration" type="number" value="120" min="60" max="600"></div>
      </div>
      <div style="margin-top:10px"><button class="act" onclick="openPairingWindow()">Open pairing window</button></div>
    </div>
    <div class="card" id="paircodec" style="display:none"><h2>Pairing code</h2>
      <div id="paircode"></div>
      <div id="paircount" class="paircount"></div>
      <p class="muted">Enter this code in the MacControlAgent UI on the Mac (<code>python -m maccontrol_agent --pair-code &lt;code&gt;</code>).</p>
      <div><button class="sec" onclick="closePairingWindow()">Close window</button></div>
    </div>
    <div class="card" id="pairrevokec" style="display:none"><h2>Revoke pairing</h2>
      <p class="muted">Revoking disconnects the paired agent; it must pair again before this endpoint accepts it.</p>
      <button class="dgr" onclick="revokePairing()">Revoke pairing</button>
    </div>
  </section>

  <section id="tab-security" style="display:none">
    <div class="card"><h2>API keys</h2><div id="keymsg"></div>
      <table id="keylist"></table>
      <div class="row" style="margin-top:10px">
        <div><label for="k-label">New key label</label><input id="k-label" placeholder="e.g. Companion production"></div>
        <div><label for="k-role">Role</label><select id="k-role"><option>READ</option><option>CONTROL</option><option>ADMIN</option></select></div>
      </div>
      <div style="margin-top:10px"><button class="sec" onclick="createKey()">Create key</button></div>
      <div id="keyonce"></div>
      <p class="muted">Raw keys are shown exactly once at creation and are never stored or logged (spec 13.1.1). Keys authenticate controllers with <code>Authorization: Bearer</code>; roles form READ &lt; CONTROL &lt; ADMIN. Key management is available here (Web UI session only), not with API keys.</p>
    </div>
    <div class="card"><h2>Admin password</h2><div id="pwmsg"></div>
      <label for="k-pw">New password (minimum 10 characters)</label>
      <input type="password" id="k-pw" autocomplete="new-password">
      <div style="margin-top:10px"><button class="act" onclick="changePassword()">Change password</button></div>
      <p class="muted">5 consecutive failed logins lock the Web UI for 60 s (spec 13.1.1). Rate limits: READ 60, CONTROL 30, ADMIN 10 requests per minute per key.</p>
    </div>
  </section>

  <section id="tab-logs" style="display:none">
    <div class="card"><h2>Ring-buffer log (spec 15.2)</h2><div id="logmsg"></div>
      <div class="row">
        <div><label for="l-cat">Category</label><select id="l-cat"><option value="">(all)</option><option>command</option><option>session</option><option>auth</option><option>config</option><option>ota</option><option>system</option></select></div>
        <div><label for="l-level">Level</label><select id="l-level"><option value="">(all)</option><option>info</option><option>warn</option><option>error</option></select></div>
        <div><label for="l-limit">Limit</label><input id="l-limit" type="number" value="100" min="1" max="512"></div>
        <div style="flex:0;min-width:auto;align-self:flex-end"><button class="sec" onclick="refreshLogs(true)">Apply</button></div>
      </div>
      <div id="logdropped" class="muted"></div>
      <table id="loglist"></table>
      <div style="margin-top:8px"><button class="sec sm" onclick="logPage(-1)">&larr; prev</button> <button class="sec sm" onclick="logPage(1)">next &rarr;</button> <span class="muted">pages by entry seq</span></div>
    </div>
  </section>
</main>
</div>
<footer>MacControl endpoint Web UI — bound to the ADMIN role (spec ch. 14). HTTP is unencrypted; restrict to a trusted management network.</footer>

<script>
var editingMacroId=null, editingSteps=[], editingTriggerId=null;

function el(id){return document.getElementById(id);}
function esc(s){return (s==null?'':String(s)).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}

async function api(path,method,body){
  var opt={method:method||'GET',headers:{},credentials:'same-origin'};
  if(body!==undefined){opt.headers['Content-Type']='application/json';opt.body=typeof body==='string'?body:JSON.stringify(body);}
  var r=await fetch(path,opt);
  var txt=await r.text();
  var j=null;try{j=txt?JSON.parse(txt):null;}catch(e){}
  if(!r.ok){
    var err=(j&&j.error)?j.error:{code:'http_'+r.status,message:txt||r.statusText};
    if(r.status===401&&path.indexOf('/ui/')!==0){showLogin();}
    throw err;
  }
  return j;
}
function showErr(where,e){el(where).innerHTML='<div class="err">'+esc(e.code||'error')+': '+esc(e.message||'')+'</div>';}
function ageStr(iso){
  if(!iso)return null;
  var t=new Date(iso).getTime();if(isNaN(t))return null;
  var s=Math.max(0,Math.floor((Date.now()-t)/1000));
  if(s<60)return s+' s ago';if(s<3600)return Math.floor(s/60)+' min ago';return Math.floor(s/3600)+' h ago';
}
function renderTuple(t){
  // Spec 14.3: the five display states, text always redundant with color.
  if(!t||t.value===null||t.freshness==='unknown')
    return '<span class="chip unknown">No data</span>';
  var v=(typeof t.value==='boolean')?(t.value?'true':'false'):esc(t.value);
  if(t.freshness==='stale'){
    var a=ageStr(t.observed_at);
    return v+' <span class="chip stale">Last seen '+(a||'unknown time')+' ago</span><span class="badge">stale</span>';
  }
  if(t.freshness==='expected_offline')
    return v+' <span class="chip off">Expected offline (power window)</span>';
  return v+' <span class="muted">fresh</span>';
}
function stateChip(st,result,err){
  // Spec 14.3: terminal labels; no completion words on the unverified path.
  if(st==='completed')return '<span class="chip verified">Verified</span>';
  if(st==='unconfirmed')return '<span class="chip unverified">Unverified — HID only</span>';
  if(st==='failed')return '<span class="chip off">Failed</span>'+(err?(' <span class="muted">'+esc(err)+'</span>'):'');
  if(st==='timed_out')return '<span class="chip off">Timed out</span>'+(err?(' <span class="muted">'+esc(err)+'</span>'):'');
  return '<span class="muted">'+esc(st)+'</span>';
}
function showTab(name){
  document.querySelectorAll('nav button').forEach(function(b){b.classList.toggle('on',b.dataset.tab===name);});
  ['dash','device','macros','triggers','pairing','security','logs'].forEach(function(t){el('tab-'+t).style.display=(t===name)?'':'none';});
  if(name==='dash')refreshDash();
  if(name==='macros')refreshMacros();
  if(name==='triggers')refreshTriggers();
  if(name==='pairing')loadPairing();
  else pairingStopTimer();
  if(name==='security')refreshKeys();
  if(name==='logs')refreshLogs(true);
}

async function checkSession(){
  try{
    var s=await api('/ui/session');
    if(s.authenticated){showApp();}else{showLogin(s.password_set);}
  }catch(e){showLogin();}
}
function showLogin(passwordSet){
  el('login').style.display='';
  el('app').style.display='none';el('logoutbtn').style.display='none';
  if(passwordSet===false){
    el('logintitle').textContent='Create admin password';
    el('loginbtn').textContent='Create password & sign in';
    el('loginhint').textContent='No admin password is set. Choose one (minimum 10 characters); this form is the one-time setup.';
  }else{
    el('logintitle').textContent='Sign in';
    el('loginbtn').textContent='Sign in';
    el('loginhint').textContent='';
  }
}
function showApp(){
  el('login').style.display='none';
  // 'block', not '': the .tabs class carries display:none, so clearing the
  // inline style would leave the app shell hidden.
  el('app').style.display='block';
  el('logoutbtn').style.display='';el('httpwarn').style.display='';
  loadDevice();
  showTab('dash');
}
async function doLogin(){
  el('loginerr').innerHTML='';
  try{await api('/ui/login','POST',{password:el('pw').value});el('pw').value='';showApp();}
  catch(e){showErr('loginerr',e);}
}
async function logout(){try{await api('/ui/logout','POST',{});}catch(e){}showLogin();}
el('pw').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin();});

async function refreshDash(){
  try{
    var st=await api('/api/v1/status');
    el('devname').textContent=st.device&&st.device.name?st.device.name.value||'':'';
    el('modeinfo').textContent='mode: A (unpaired)';
    var rows='';
    ['usb','network','agent'].forEach(function(k){var t=st.connection[k];rows+='<tr><td>'+k+'</td><td>'+renderTuple(t)+'</td></tr>';});
    el('conn-table').innerHTML=rows;
    var mrows='';
    if(st.mac)Object.keys(st.mac).forEach(function(k){mrows+='<tr><td>'+k+'</td><td>'+renderTuple(st.mac[k])+'</td></tr>';});
    el('mac-table').innerHTML=mrows||'<tr><td><span class="chip unknown">No data</span></td></tr>';
  }catch(e){showErr('err-global',e);}
  try{
    var cmds=await api('/api/v1/commands?limit=1');
    if(cmds.commands&&cmds.commands.length){
      var c=cmds.commands[0];
      el('lastcmd').innerHTML=esc(c.type)+' → '+stateChip(c.state,c.result,c.error_code)+
        ' <span class="muted">'+esc(c.requested_at||'')+'</span> <a class="cmdlink" href="/api/v1/commands/'+c.command_id+'">'+c.command_id+'</a>';
    }else{el('lastcmd').innerHTML='<span class="chip unknown">No data</span>';}
  }catch(e){el('lastcmd').textContent='No data';}
  try{
    var trgs=await api('/api/v1/triggers');
    var names={};
    try{
      var ms=await api('/api/v1/macros');
      (ms.macros||[]).forEach(function(m){names[m.macro_id]=m.name||m.macro_id;});
    }catch(e2){}
    var html='';
    (trgs.triggers||[]).forEach(function(t){
      if(t.source==='webui_button'&&t.enabled){
        var label=names[t.macro_id]||t.macro_id;
        html+='<button class="act" onclick="runMacro(\''+t.macro_id+'\',this)" title="'+esc(t.macro_id)+'">RUN '+esc(label)+'</button> ';
      }});
    el('runbtns').innerHTML=html||'<span class="muted">No enabled webui_button triggers</span>';
  }catch(e){el('runbtns').innerHTML='<span class="muted">No enabled webui_button triggers</span>';}
}
async function runMacro(macroId,btn){
  try{
    var r=await api('/api/v1/macros/'+macroId+'/execute','POST',{});
    trackCommand(r,btn);
  }catch(e){showErr('err-global',e);}
}
async function powerCmd(name){
  try{
    var r=await api('/api/v1/system/'+name,'POST',{});
    trackCommand(r,null);
  }catch(e){showErr('err-global',e);}
}
function trackCommand(r,btn){
  var span=document.createElement('span');span.className='cmdlink';
  (btn?btn.parentNode:el('powerresult')).appendChild(span);
  var url=r.record_url,cid=r.command_id;
  async function poll(){
    try{
      var rec=await api(url);
      if(rec.state==='accepted'||rec.state==='dispatched'||rec.state==='confirming'){
        span.innerHTML=cid+' '+stateChip(rec.state)+'…';
        setTimeout(poll,1500);
      }else{
        span.innerHTML=cid+' '+stateChip(rec.state,rec.result,rec.error_code)+
          ' <a class="cmdlink" href="'+url+'">record</a>';
      }
    }catch(e){span.textContent=cid+' (poll failed)';}
  }
  span.innerHTML=cid+' <span class="muted">accepted</span>';
  setTimeout(poll,800);
}

async function loadDevice(){
  try{
    var id=await api('/api/v1/device/identity');
    el('id-name').value=id.device_name||'';
    el('id-hostname').value=id.hostname||'';
    el('id-location').value=id.location||'';
    el('id-description').value=id.description||'';
  }catch(e){}
}
async function saveIdentity(){
  el('devmsg').innerHTML='';
  var body={};
  // name/hostname are omitted when empty (hostname must stay valid);
  // location/description are always sent so they can be cleared.
  var n=el('id-name').value,h=el('id-hostname').value;
  if(n!=='')body.device_name=n;
  if(h!=='')body.hostname=h;
  body.location=el('id-location').value;
  body.description=el('id-description').value;
  try{
    await api('/api/v1/device/identity','POST',body);
    el('devmsg').innerHTML='<div class="ok">Identity saved.</div>';
    loadDevice();
  }catch(e){showErr('devmsg',e);}
}

var STEP_KEYS=['a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z','0','1','2','3','4','5','6','7','8','9','enter','space','esc','tab','backspace','delete','up','down','left','right','f1','f2','f3','f4','f5','f6','f7','f8','f9','f10','f11','f12','home','end','pageup','pagedown'];
async function refreshMacros(){
  el('macromsg').innerHTML='';
  try{
    var ms=await api('/api/v1/macros');
    var html='<tr><th>ID</th><th>Name</th><th>Rev</th><th>Timeout</th><th>Steps</th><th></th></tr>';
    (ms.macros||[]).forEach(function(m){
      html+='<tr><td>'+esc(m.macro_id)+'</td><td>'+esc(m.name)+'</td><td>'+m.revision+'</td><td>'+m.timeout_ms+' ms</td><td>'+(m.steps||[]).length+'</td><td>'+
        '<button class="sec sm" onclick="editMacro(\''+m.macro_id+'\')">Edit</button> '+
        '<button class="dgr sm" onclick="deleteMacro(\''+m.macro_id+'\')">Delete</button></td></tr>';});
    el('macrolist').innerHTML=html;
  }catch(e){showErr('macromsg',e);}
}
async function editMacro(id){
  editingMacroId=id;editingSteps=[];el('macroedmsg').innerHTML='';
  el('macroedtitle').textContent=id?('Edit macro '+id):'New macro';
  el('m-name').value='';el('m-timeout').value='10000';
  el('m-evtype').value='';el('m-evmatch').value='';
  if(id){
    try{
      var ms=await api('/api/v1/macros');
      var m=(ms.macros||[]).find(function(x){return x.macro_id===id;});
      if(m){
        el('m-name').value=m.name;el('m-timeout').value=m.timeout_ms;
        if(m.expected_event){el('m-evtype').value=m.expected_event.type||'';
          el('m-evmatch').value=m.expected_event.match?JSON.stringify(m.expected_event.match):'';}
        editingSteps=(m.steps||[]).map(function(s){return Object.assign({},s);});
      }
    }catch(e){showErr('macroedmsg',e);}
  }else{addStep();}
  renderSteps();
  el('macroeditor').style.display='';
}
function cancelMacroEdit(){el('macroeditor').style.display='none';}
function addStep(){editingSteps.push({type:'key_press',key:'enter',modifiers:[],value:'',delay_ms:100});renderSteps();}
function stepTypes(){return ['key_press','key_combo','modifier_down','modifier_up','key_release','text','delay'];}
function modBoxes(i,mods){
  return ['ctrl','shift','alt','cmd'].map(function(m){
    var on=(mods||[]).indexOf(m)>=0;
    return '<label style="display:inline;margin:0 6px 0 0"><input type="checkbox" style="width:auto" '+(on?'checked':'')+' onchange="stepMod('+i+',\''+m+'\',this.checked)"> '+m+'</label>';}).join('');
}
function renderSteps(){
  var tb=document.querySelector('#steptable tbody');var html='';
  editingSteps.forEach(function(s,i){
    var keyTd=(s.type==='key_press'||s.type==='key_combo'||s.type==='key_release')
      ?'<input list="keys" value="'+esc(s.key||'')+'" onchange="stepField('+i+',\'key\',this.value)"><datalist id="keys">'+STEP_KEYS.map(function(k){return '<option>'+k+'</option>';}).join('')+'</datalist>':'';
    var modTd=(s.type==='key_combo'||s.type==='modifier_down'||s.type==='modifier_up')?modBoxes(i,s.modifiers):'';
    var valTd=(s.type==='text')?'<input value="'+esc(s.value||'')+'" onchange="stepField('+i+',\'value\',this.value)">':'';
    var dlyTd=(s.type==='delay')?'<input type="number" value="'+(s.delay_ms||100)+'" onchange="stepField('+i+',\'delay_ms\',parseInt(this.value)||0)">':'';
    html+='<tr><td>'+(i+1)+'</td><td><select onchange="stepType('+i+',this.value)">'+
      stepTypes().map(function(t){return '<option'+(s.type===t?' selected':'')+'>'+t+'</option>';}).join('')+'</select></td><td>'+keyTd+'</td><td>'+modTd+'</td><td>'+valTd+'</td><td>'+dlyTd+'</td><td>'+
      '<button class="sec sm" onclick="stepMove('+i+',-1)">↑</button> <button class="sec sm" onclick="stepMove('+i+',1)">↓</button> <button class="dgr sm" onclick="stepDel('+i+')">×</button></td></tr>';
  });
  tb.innerHTML=html;
}
function stepField(i,f,v){editingSteps[i][f]=v;}
function stepType(i,t){editingSteps[i].type=t;renderSteps();}
function stepMod(i,m,on){
  var a=editingSteps[i].modifiers||[];
  var ix=a.indexOf(m);if(on&&ix<0)a.push(m);if(!on&&ix>=0)a.splice(ix,1);
  editingSteps[i].modifiers=a;
}
function stepMove(i,dir){
  var j=i+dir;if(j<0||j>=editingSteps.length)return;
  var t=editingSteps[i];editingSteps[i]=editingSteps[j];editingSteps[j]=t;renderSteps();
}
function stepDel(i){editingSteps.splice(i,1);renderSteps();}
async function saveMacro(){
  el('macroedmsg').innerHTML='';
  var steps=editingSteps.map(function(s,i){
    var o={order:i+1,type:s.type};
    if(s.key)o.key=s.key;
    if(s.modifiers&&s.modifiers.length)o.modifiers=s.modifiers;
    if(s.type==='text')o.value=s.value||'';
    if(s.type==='delay')o.delay_ms=s.delay_ms||0;
    return o;});
  var body={name:el('m-name').value,timeout_ms:parseInt(el('m-timeout').value)||10000,steps:steps};
  if(el('m-evtype').value){
    body.expected_event={type:el('m-evtype').value};
    if(el('m-evmatch').value){try{body.expected_event.match=JSON.parse(el('m-evmatch').value);}catch(e){showErr('macroedmsg',{code:'bad_request',message:'match is not valid JSON'});return;}}
  }
  try{
    if(editingMacroId){await api('/api/v1/macros/'+editingMacroId,'PUT',body);}
    else{await api('/api/v1/macros','POST',body);}
    el('macroeditor').style.display='none';
    refreshMacros();refreshTriggers();
  }catch(e){showErr('macroedmsg',e);}
}
async function deleteMacro(id){
  if(!confirm('Delete macro '+id+'? Triggers bound to it will be auto-disabled.'))return;
  el('macromsg').innerHTML='';
  try{await api('/api/v1/macros/'+id,'DELETE');refreshMacros();refreshTriggers();}
  catch(e){showErr('macromsg',e);}
}

async function refreshTriggers(){
  el('trigmsg').innerHTML='';
  try{
    var ts=await api('/api/v1/triggers');
    var names={};
    try{
      var ms=await api('/api/v1/macros');
      (ms.macros||[]).forEach(function(m){names[m.macro_id]=m.name||m.macro_id;});
    }catch(e2){}
    var html='<tr><th>ID</th><th>Source</th><th>GPIO</th><th>Edge</th><th>Debounce</th><th>Macro</th><th>Enabled</th><th></th></tr>';
    (ts.triggers||[]).forEach(function(t){
      var mlabel=names[t.macro_id]||t.macro_id;
      html+='<tr><td>'+esc(t.trigger_id)+'</td><td>'+esc(t.source)+'</td><td>'+(t.gpio_pin!=null?t.gpio_pin:'—')+'</td><td>'+(t.edge||'—')+'</td><td>'+(t.debounce_ms||'—')+'</td><td title="'+esc(t.macro_id)+'">'+esc(mlabel)+'</td><td>'+(t.enabled?'yes':'no')+'</td><td>'+
        '<button class="sec sm" onclick="editTrigger(\''+t.trigger_id+'\')">Edit</button> '+
        '<button class="dgr sm" onclick="deleteTrigger(\''+t.trigger_id+'\')">Delete</button></td></tr>';});
    el('triggerlist').innerHTML=html;
  }catch(e){showErr('trigmsg',e);}
}
async function editTrigger(id){
  editingTriggerId=id;el('trigedmsg').innerHTML='';
  el('trigedtitle').textContent=id?('Edit trigger '+id):'New trigger';
  el('t-source').value='webui_button';el('t-pin').value='4';el('t-edge').value='falling';
  el('t-debounce').value='50';el('t-enabled').value='true';
  try{
    var ms=await api('/api/v1/macros');
    el('t-macro').innerHTML=(ms.macros||[]).map(function(m){return '<option value="'+m.macro_id+'">'+esc(m.name)+' ('+m.macro_id+')</option>';}).join('');
    if(id){
      var ts=await api('/api/v1/triggers');
      var t=(ts.triggers||[]).find(function(x){return x.trigger_id===id;});
      if(t){
        el('t-source').value=t.source;el('t-macro').value=t.macro_id;
        el('t-enabled').value=t.enabled?'true':'false';
        if(t.gpio_pin!=null)el('t-pin').value=t.gpio_pin;
        el('t-edge').value=t.edge||'falling';el('t-debounce').value=t.debounce_ms||50;
      }
    }
  }catch(e){showErr('trigedmsg',e);}
  trigSourceChanged();
  el('triggereditor').style.display='';
}
function trigSourceChanged(){el('t-gpiofields').style.display=(el('t-source').value==='gpio')?'':'none';}
function cancelTriggerEdit(){el('triggereditor').style.display='none';}
async function saveTrigger(){
  el('trigedmsg').innerHTML='';
  var body={source:el('t-source').value,macro_id:el('t-macro').value,enabled:el('t-enabled').value==='true'};
  if(body.source==='gpio'){
    body.gpio_pin=parseInt(el('t-pin').value);
    body.edge=el('t-edge').value;
    body.debounce_ms=parseInt(el('t-debounce').value)||50;
  }
  try{
    if(editingTriggerId){await api('/api/v1/triggers/'+editingTriggerId,'PUT',body);}
    else{await api('/api/v1/triggers','POST',body);}
    el('triggereditor').style.display='none';
    refreshTriggers();
  }catch(e){showErr('trigedmsg',e);}
}
async function deleteTrigger(id){
  if(!confirm('Delete trigger '+id+'?'))return;
  el('trigmsg').innerHTML='';
  try{await api('/api/v1/triggers/'+id,'DELETE');refreshTriggers();}
  catch(e){showErr('trigmsg',e);}
}

// ---- Security tab (spec 13.1.1: keys managed through the Web UI only).
async function refreshKeys(){
  el('keymsg').innerHTML='';el('keyonce').innerHTML='';
  try{
    var d=await api('/api/v1/keys');
    var rows='<tr><th>ID</th><th>Label</th><th>Role</th><th>Created</th><th>Last used</th><th>State</th><th></th></tr>';
    (d.keys||[]).forEach(function(k){
      rows+='<tr><td>'+esc(k.key_id)+'</td><td>'+esc(k.label)+'</td><td>'+esc(k.role)+'</td><td class="muted">'+esc(k.created_at||'')+'</td><td class="muted">'+(k.last_used_at?esc(k.last_used_at):'never')+'</td><td>'+esc(k.state)+'</td><td>'+(k.state==='active'?('<button class="dgr sm" onclick="revokeKey(\''+esc(k.key_id)+'\')">Revoke</button>'):'')+'</td></tr>';
    });
    el('keylist').innerHTML=rows;
  }catch(e){showErr('keymsg',e);}
}
async function createKey(){
  el('keymsg').innerHTML='';el('keyonce').innerHTML='';
  try{
    var k=await api('/api/v1/keys','POST',{role:el('k-role').value,label:el('k-label').value});
    el('keyonce').innerHTML='<div class="ok">Key <b>'+esc(k.key_id)+'</b> created. Copy it now — it is shown exactly once:<br><code>'+esc(k.key)+'</code></div>';
    el('k-label').value='';
    refreshKeys();
  }catch(e){showErr('keymsg',e);}
}
async function revokeKey(id){
  if(!confirm('Revoke key '+id+'? Controllers using it lose access immediately.'))return;
  el('keymsg').innerHTML='';
  try{await api('/api/v1/keys/'+id,'DELETE');refreshKeys();}
  catch(e){showErr('keymsg',e);}
}
async function changePassword(){
  el('pwmsg').innerHTML='';
  try{
    await api('/ui/password','POST',{password:el('k-pw').value});
    el('k-pw').value='';
    el('pwmsg').innerHTML='<div class="ok">Password changed.</div>';
  }catch(e){showErr('pwmsg',e);}
}

// ---- Pairing tab (spec 14.1): pairing state, one-time code display, window countdown.
var pairingTimer=null, pairCodeShown=null, pairSecondsLeft=0;

function pairingStopTimer(){if(pairingTimer){clearInterval(pairingTimer);pairingTimer=null;}}
function pairingEnsureTimer(){if(!pairingTimer)pairingTimer=setInterval(pairingTick,2000);}

function pairStateLabel(st){
  if(st==='unpaired')return{label:'No agent paired',chip:'unknown'};
  if(st==='pairing_window')return{label:'Pairing window open',chip:'unverified'};
  if(st==='active')return{label:'Paired',chip:'verified'};
  if(st==='revoked')return{label:'Pairing revoked',chip:'off'};
  return{label:st||'unknown',chip:'unknown'};
}

async function loadPairing(){
  pairingStopTimer();
  el('pairmsg').innerHTML='';
  var d;
  try{d=await api('/api/v1/pairing');}
  catch(e){showErr('pairmsg',e);return;}
  renderPairing(d);
  if(d.state==='pairing_window'&&d.window&&d.window.open)pairingEnsureTimer();
  try{
    var c=await api('/api/v1/capabilities');
    var m=(c&&c.mode!=null)?String(c.mode).toLowerCase():'';
    var paired=(m==='b'||m==='mode_b'||m==='paired'||m==='agent'||m==='agent_paired'||m==='mode b');
    el('pairmode').textContent=paired?'Mode B (agent paired)':'Mode A (agentless)';
  }catch(e){el('pairmode').textContent='';}
}

function renderPairing(d){
  var st=d.state||'unknown';
  var sl=pairStateLabel(st);
  el('pairstatus').innerHTML='<span class="chip '+sl.chip+'">'+esc(sl.label)+'</span>';
  var r=d.record,rows='';
  if(r&&st==='active'){
    rows='<tr><th>pairing_id</th><td>'+esc(r.pairing_id)+'</td></tr>'+
      '<tr><th>agent_instance_id</th><td>'+esc(r.agent_instance_id)+'</td></tr>'+
      '<tr><th>device_id</th><td>'+esc(r.device_id)+'</td></tr>'+
      '<tr><th>created_at</th><td class="muted">'+esc(r.created_at||'')+'</td></tr>'+
      '<tr><th>last_used_at</th><td class="muted">'+(r.last_used_at?esc(r.last_used_at):'never')+'</td></tr>';
  }
  el('pairrecord').innerHTML=rows;
  var win=d.window||{};
  var winOpen=(st==='pairing_window')&&!!win.open;
  el('pairwindowcard').style.display=((st==='unpaired'||st==='revoked')&&!winOpen)?'':'none';
  el('pairrevokec').style.display=(st==='active')?'':'none';
  if(winOpen){
    el('paircodec').style.display='';
    if(pairCodeShown){
      el('paircode').innerHTML='<div class="paircode">'+esc(pairCodeShown)+'</div>';
    }else{
      el('paircode').innerHTML='<div class="warn">A pairing window is open, but the pairing code is only displayed when the window is opened from this page. Close the window and open a new one to see the code.</div>';
    }
    if(typeof win.seconds_remaining==='number')pairSecondsLeft=win.seconds_remaining;
    renderPairCountdown(win.failed_attempts||0);
  }else{
    el('paircodec').style.display='none';
  }
}

function renderPairCountdown(failed){
  var s=Math.max(0,pairSecondsLeft);
  var txt=(s>=60)?(Math.floor(s/60)+' min '+(s%60)+' s'):(s+' s');
  var f=(failed>0)?(' <span class="muted">failed attempts: '+failed+'</span>'):'';
  el('paircount').innerHTML='Window closes in <b>'+txt+'</b>'+f;
}

async function pairingTick(){
  try{
    var d=await api('/api/v1/pairing');
    if(d.state!=='pairing_window'||!(d.window&&d.window.open)){
      // Window expired, paired, or closed elsewhere: drop the code and re-render.
      pairingStopTimer();pairCodeShown=null;renderPairing(d);pairingEnsureTimer();return;
    }
    renderPairing(d);
  }catch(e){
    // Transient poll failure: count down locally; fall back to a full reload at 0.
    pairSecondsLeft=Math.max(0,pairSecondsLeft-2);
    renderPairCountdown(0);
    if(pairSecondsLeft<=0){pairingStopTimer();loadPairing();}
  }
}

async function openPairingWindow(){
  el('pairmsg').innerHTML='';
  var dur=parseInt(el('p-duration').value,10);
  if(isNaN(dur))dur=120;
  try{
    var r=await api('/api/v1/pairing/window','POST',{duration_s:dur});
    pairCodeShown=r.pairing_code||null;
    loadPairing();
  }catch(e){showErr('pairmsg',e);}
}
async function closePairingWindow(){
  el('pairmsg').innerHTML='';
  try{
    await api('/api/v1/pairing/window','DELETE');
    pairCodeShown=null;loadPairing();
  }catch(e){showErr('pairmsg',e);}
}
async function revokePairing(){
  if(!confirm('Revoke pairing? The paired agent loses access and must pair again.'))return;
  el('pairmsg').innerHTML='';
  try{
    await api('/api/v1/pairing/revoke','POST',{});
    loadPairing();
  }catch(e){showErr('pairmsg',e);}
}

// ---- Logs tab (spec 15.2): ascending seq, filter selects, seq-cursor paging.
var logSince=0, logLastSeq=0, logDropped=0;
async function refreshLogs(reset){
  if(reset)logSince=0;
  el('logmsg').innerHTML='';
  try{
    var q='?limit='+((parseInt(el('l-limit').value)||100));
    if(el('l-cat').value)q+='&category='+el('l-cat').value;
    if(el('l-level').value)q+='&level='+el('l-level').value;
    q+='&since_seq='+logSince;
    var d=await api('/api/v1/logs'+q);
    var rows='<tr><th>seq</th><th>ts</th><th>level</th><th>category</th><th>event</th><th>actor</th><th>command</th></tr>';
    var last=logSince;
    (d.entries||[]).forEach(function(e){
      last=e.seq;
      rows+='<tr><td>'+e.seq+'</td><td class="muted">'+esc(e.ts||'')+'</td><td>'+esc(e.level)+'</td><td>'+esc(e.category)+'</td><td>'+esc(e.event||'')+'</td><td class="muted">'+(e.actor?esc(e.actor):'')+'</td><td class="muted">'+(e.command_id?esc(e.command_id):'')+'</td></tr>';
    });
    el('loglist').innerHTML=rows;
    el('logdropped').textContent=(d.dropped>0)?(d.dropped+' entries before seq '+(logSince+1)+' have been overwritten (ring buffer, 512 entries)'):'';
    logLastSeq=last;logDropped=d.dropped||0;
  }catch(e){showErr('logmsg',e);}
}
function logPage(dir){
  if(dir>0){if(logLastSeq>logSince)logSince=logLastSeq;}
  else{logSince=0;}
  refreshLogs(false);
}

checkSession();
</script>
</body>
</html>
)HTML";
