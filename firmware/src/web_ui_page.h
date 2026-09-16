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
.ok{background:#e9f0e9;border:1px solid #b9d0b9;color:#2f5d2f;border-radius:4px;padding:8px 10px;margin:8px 0}
input,select,textarea{width:100%;padding:6px 8px;border:1px solid var(--line);border-radius:4px;font-size:13px;font-family:inherit}
label{display:block;font-size:12px;color:#5c6b7a;margin:8px 0 3px}
.row{display:flex;gap:10px;flex-wrap:wrap}
.row>div{flex:1;min-width:180px}
#login{max-width:380px;margin:60px auto}
.tabs{display:none}
.power button{margin:0 6px 6px 0}
.cmdlink{font-size:12px;margin-left:8px}
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
</nav>
<main>
  <div id="err-global"></div>

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
        <div><label>Name</label><input id="id-name"></div>
        <div><label>Hostname</label><input id="id-hostname"></div>
      </div>
      <div class="row">
        <div><label>Location</label><input id="id-location"></div>
        <div><label>Description</label><input id="id-description"></div>
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
        <div><label>Name (1-32 chars)</label><input id="m-name"></div>
        <div><label>Timeout ms (500-60000)</label><input id="m-timeout" type="number" value="10000"></div>
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
        <div><label>Source</label><select id="t-source" onchange="trigSourceChanged()">
          <option value="webui_button">webui_button (Dashboard RUN button)</option>
          <option value="gpio">gpio (physical input)</option></select></div>
        <div><label>Macro</label><select id="t-macro"></select></div>
        <div><label>Enabled</label><select id="t-enabled"><option value="true">enabled</option><option value="false">disabled</option></select></div>
      </div>
      <div class="row" id="t-gpiofields">
        <div><label>GPIO pin (0-21)</label><input id="t-pin" type="number" min="0" max="21"></div>
        <div><label>Edge</label><select id="t-edge"><option value="falling">falling</option><option value="rising">rising</option></select></div>
        <div><label>Debounce ms (10-500)</label><input id="t-debounce" type="number" value="50" min="10" max="500"></div>
      </div>
      <div style="margin-top:12px">
        <button class="act" onclick="saveTrigger()">Save trigger</button>
        <button class="sec" onclick="cancelTriggerEdit()">Cancel</button>
      </div>
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
  ['dash','device','macros','triggers'].forEach(function(t){el('tab-'+t).style.display=(t===name)?'':'none';});
  if(name==='dash')refreshDash();
  if(name==='macros')refreshMacros();
  if(name==='triggers')refreshTriggers();
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
  el('app').style.display='';el('logoutbtn').style.display='';
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
    var html='';
    (trgs.triggers||[]).forEach(function(t){
      if(t.source==='webui_button'&&t.enabled){
        html+='<button class="act" onclick="runMacro(\''+t.macro_id+'\',this)">RUN '+esc(t.macro_id)+'</button> ';
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
    var st=await api('/api/v1/status');
    el('id-name').value=st.device&&st.device.name&&st.device.name.value?st.device.name.value:'';
    el('id-hostname').value=st.device&&st.device.hostname&&st.device.hostname.value?st.device.hostname.value:'';
  }catch(e){}
  try{
    var caps=await api('/api/v1/capabilities');
    el('id-location').value='';el('id-description').value='';
  }catch(e){}
}
async function saveIdentity(){
  el('devmsg').innerHTML='';
  var body={};
  ['name','hostname','location','description'].forEach(function(f){
    var v=el('id-'+f).value;if(v!=='')body[f==='name'?'device_name':f]=v;});
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
    var html='<tr><th>ID</th><th>Source</th><th>GPIO</th><th>Edge</th><th>Debounce</th><th>Macro</th><th>Enabled</th><th></th></tr>';
    (ts.triggers||[]).forEach(function(t){
      html+='<tr><td>'+esc(t.trigger_id)+'</td><td>'+esc(t.source)+'</td><td>'+(t.gpio_pin!=null?t.gpio_pin:'—')+'</td><td>'+(t.edge||'—')+'</td><td>'+(t.debounce_ms||'—')+'</td><td>'+esc(t.macro_id)+'</td><td>'+(t.enabled?'yes':'no')+'</td><td>'+
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

checkSession();
</script>
</body>
</html>
)HTML";
