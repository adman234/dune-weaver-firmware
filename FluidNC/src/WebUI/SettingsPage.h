// Sand-table fork: the table's own settings page, served at /settings.
// It covers what the phone app has no screen for: MQTT, axis direction (a
// config.yaml edit + restart), a motion test, and the LED look rotation
// ($LED/RunLooks, IdleLooks, LookMinutes, LookShuffle).  One self-contained
// page with no external assets.  It holds no secrets: everything is read
// through the API (/sand_settings never lists MQTT/Password, which is
// write-only here too), values are placed with .value/.textContent only,
// and writes go as POST bodies so nothing lands in a URL or a log.

#pragma once

namespace WebUI {
    const char PAGE_SETTINGS[] = R"HTML(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Table Settings</title>
<style>
:root{color-scheme:dark}
body{font-family:-apple-system,system-ui,Segoe UI,Roboto,sans-serif;background:#141210;color:#eee8dd;margin:0;display:flex;justify-content:center}
main{width:100%;max-width:560px;padding:20px 16px 60px;box-sizing:border-box}
h1{font-size:22px;margin:4px 0}
h2{font-size:17px;margin:0 0 4px}
.dim{color:#a89f90;font-size:13px}
section{background:#1c1813;border:1px solid #3a352c;border-radius:12px;padding:16px;margin:16px 0}
label{display:block;font-size:13px;color:#a89f90;margin:10px 0 2px}
input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #3a352c;background:#141210;color:#eee8dd;font-size:15px}
input[type=checkbox]{width:auto;margin-right:6px}
.chk{display:flex;align-items:center;color:#eee8dd;font-size:15px}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}
button{padding:10px 14px;margin:10px 6px 0 0;border-radius:9px;border:1px solid #3a352c;background:#2a241c;color:#eee8dd;font-size:15px;cursor:pointer}
button.primary{background:#c98f3d;border-color:#c98f3d;color:#1a140c;font-weight:600}
button.warn{border-color:#7a3b2e;color:#f0c4b0}
button:disabled{opacity:.5;cursor:default}
.msg{font-size:14px;margin-top:10px;min-height:1em}
.ok{color:#9fd49a}.bad{color:#f0a890}
.pill{display:inline-block;padding:2px 9px;border-radius:20px;background:#2a241c;font-size:13px}
.axis{display:flex;justify-content:space-between;align-items:center;border-top:1px solid #2a251e;padding:8px 0}
.look{display:grid;grid-template-columns:1.3fr 1fr 56px 56px 40px 30px 30px;gap:6px;align-items:center;margin:6px 0}
.look input,.look select{padding:6px;font-size:14px}
.look input[type=color]{padding:0;height:32px}
.look button{margin:0;padding:6px 0}
.hdr{font-size:12px;color:#a89f90;margin:8px 0 0}
#keybox{display:none}
.ids{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:4px 0 8px;font-size:14px}
.ids span{color:#a89f90}.ids b{font-weight:600;word-break:break-all}
a{color:#c98f3d}
</style></head><body><main>
<h1 id="title">Table settings</h1>
<div class="dim"><span id="fw"></span> &middot; <span class="pill" id="state">&hellip;</span></div>

<section>
<h2>This table</h2>
<div class="ids"><span>IP</span><b id="i_ip">&hellip;</b><span>Network name</span><b id="i_mdns">&hellip;</b><span>Home Assistant name</span><b id="i_ha">&hellip;</b></div>
<div class="row2">
<div><label>Network name (mDNS, letters/digits/-)</label><input id="i_host" maxlength="32" autocomplete="off"></div>
<div><label>Name in Home Assistant</label><input id="m_name" maxlength="40"></div>
</div>
<button class="primary" onclick="saveIds(this)">Save names</button>
<div class="msg" id="i_msg"></div>
</section>

<section id="keybox">
<h2>Table password</h2>
<div class="dim">This table is locked. Enter its password to change settings.</div>
<input id="key" type="password" autocomplete="current-password">
<button class="primary" onclick="setKey()">Unlock</button>
</section>

<section>
<h2>MQTT / Home Assistant</h2>
<div class="dim">Status: <span id="mqst">&hellip;</span></div>
<label class="chk"><input type="checkbox" id="m_enable">Enabled</label>
<div class="row2">
<div><label>Broker (IP or host)</label><input id="m_broker" autocomplete="off"></div>
<div><label>Port</label><input id="m_port" type="number" min="1" max="65535"></div>
</div>
<div class="row2">
<div><label>User name</label><input id="m_user" autocomplete="off"></div>
<div><label>Password</label><input id="m_pass" type="password" autocomplete="new-password" placeholder="unchanged"></div>
</div>
<label class="chk"><input type="checkbox" id="m_clearpass">Remove the saved password</label>
<label>Device id (topic root)</label><input id="m_id">
<label class="chk"><input type="checkbox" id="m_disc">Home Assistant discovery</label>
<label>Discovery prefix</label><input id="m_prefix">
<button class="primary" onclick="saveMqtt(this)">Save MQTT</button>
<div class="msg" id="m_msg"></div>
</section>

<section>
<h2>Motion test</h2>
<div class="dim">Ball at &theta; <span id="th">&ndash;</span> rad, &rho; <span id="rh">&ndash;</span>. Home first. &ldquo;Edge&rdquo; must move the ball outward and &ldquo;Center&rdquo; inward; if they are swapped, flip Rho below. If patterns draw mirrored, flip Theta.</div>
<button onclick="go('/sand_home')">Home</button>
<button onclick="gotoRT(null,0)">Center</button>
<button onclick="gotoRT(null,1)">Edge</button>
<button onclick="turn(1)">Turn +90&deg;</button>
<button onclick="turn(-1)">Turn &minus;90&deg;</button>
<button class="warn" onclick="go('/sand_stop')">Stop</button>
<div class="msg" id="t_msg"></div>
</section>

<section>
<h2>Axis direction</h2>
<div class="dim">Flipping edits config.yaml on the table (the old one is kept as /config-backup.yaml) and restarts it, which then homes. Stop any pattern first. Only flip an axis that moves the wrong way.</div>
<div class="axis"><span>Theta (rotation): <b id="ax_x">&hellip;</b></span><button class="warn" onclick="flip('x',this)">Flip</button></div>
<div class="axis"><span>Rho (in / out): <b id="ax_y">&hellip;</b></span><button class="warn" onclick="flip('y',this)">Flip</button></div>
<div class="axis"><span>Both axes</span><button class="warn" onclick="flip('xy',this)">Flip both</button></div>
<div class="msg" id="a_msg"></div>
</section>

<section>
<h2>LED looks</h2>
<div class="dim">The table rotates through these looks: one step each time it starts drawing (or goes idle), and every few minutes in between. Speed 1&ndash;255, intensity 0&ndash;255; blank = the effect&rsquo;s default. &#9654; shows a look now.</div>
<div class="row2">
<div><label>Change look every (minutes, 0 = only on state change)</label><input id="l_min" type="number" min="0" max="1440"></div>
<div><label>&nbsp;</label><label class="chk"><input type="checkbox" id="l_shuf">Shuffle order</label></div>
</div>
<h2 style="margin-top:16px">While drawing</h2>
<div class="hdr look"><span>Effect</span><span>Palette</span><span>Speed</span><span>Intens.</span><span>Color</span><span></span><span></span></div>
<div id="run"></div>
<button onclick="addLook('run')">+ Add look</button> <span class="dim" id="run_n"></span>
<h2 style="margin-top:16px">While idle</h2>
<div class="hdr look"><span>Effect</span><span>Palette</span><span>Speed</span><span>Intens.</span><span>Color</span><span></span><span></span></div>
<div id="idle"></div>
<button onclick="addLook('idle')">+ Add look</button> <span class="dim" id="idle_n"></span>
<div><button class="primary" onclick="saveLooks(this)">Save looks</button></div>
<div class="msg" id="l_msg"></div>
</section>
<p class="dim">Wi-Fi setup: <a href="/wifi">/wifi</a> &middot; HTTP API: <a href="/help">/help</a></p>
</main>
<script>
var key='';try{key=sessionStorage.getItem('sandkey')||''}catch(e){}
var S={},M={effects:[],palettes:[]},ST={};
function $(i){return document.getElementById(i)}
function say(id,t,ok){var e=$(id);e.textContent=t;e.className='msg '+(ok?'ok':'bad')}
function setKey(){key=$('key').value;$('key').value='';try{sessionStorage.setItem('sandkey',key)}catch(e){}$('keybox').style.display='none';load()}
async function api(path,opt){
  opt=opt||{};opt.headers=opt.headers||{};
  if(key)opt.headers['X-Sand-Key']=key;
  var r=await fetch(path,opt);var t=await r.text();
  if(r.status==401){$('keybox').style.display='block';throw new Error('enter the table password above')}
  if(!r.ok)throw new Error((t||('HTTP '+r.status)).trim());
  return t;
}
async function cmd(line){
  var t=await api('/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'plain='+encodeURIComponent(line)});
  if(/error|invalid|requires idle/i.test(t))throw new Error(t.trim());
  return t;
}
function onoff(v){return String(v).toUpperCase()=='ON'||v===true||v==1}
async function load(){
  try{
    S=JSON.parse(await api('/sand_settings'));
    M=JSON.parse(await api('/sand_settings_meta'));
  }catch(e){say('m_msg','Could not read settings: '+e.message);return}
  if(M.locked&&!key)$('keybox').style.display='block';
  $('m_enable').checked=onoff(S['MQTT/Enable']);
  $('m_broker').value=S['MQTT/Broker']||'';
  $('m_port').value=S['MQTT/Port']||1883;
  $('m_user').value=S['MQTT/User']||'';
  $('m_pass').value='';$('m_clearpass').checked=false;
  $('m_name').value=S['MQTT/DeviceName']||'';
  $('m_id').value=S['MQTT/DeviceId']||'';
  $('m_disc').checked=onoff(S['MQTT/Discovery']);
  $('m_prefix').value=S['MQTT/Prefix']||'';
  $('mqst').textContent=M.mqtt;
  showIds();
  $('l_min').value=S['LED/LookMinutes']||0;
  $('l_shuf').checked=onoff(S['LED/LookShuffle']);
  fillLooks('run',S['LED/RunLooks']||'');fillLooks('idle',S['LED/IdleLooks']||'');
  loadAxes();
}
async function saveMqtt(b){
  b.disabled=true;
  var want={'MQTT/Enable':$('m_enable').checked?'ON':'OFF','MQTT/Broker':$('m_broker').value.trim(),
    'MQTT/Port':$('m_port').value.trim(),'MQTT/User':$('m_user').value.trim(),
    'MQTT/DeviceId':$('m_id').value.trim(),
    'MQTT/Discovery':$('m_disc').checked?'ON':'OFF','MQTT/Prefix':$('m_prefix').value.trim()};
  var pw=$('m_pass').value,n=0;
  try{
    for(var k in want){if(String(S[k]||'')!=want[k]){await cmd('$'+k+'='+want[k]);S[k]=want[k];n++}}
    if(pw){await cmd('$MQTT/Password='+pw);n++}
    else if($('m_clearpass').checked){await cmd('$MQTT/Password=');n++}
    $('m_pass').value='';$('m_clearpass').checked=false;
    say('m_msg',n?'Saved. Reconnecting…':'Nothing changed.',true);
    if(n)setTimeout(mqttStatus,6000);
  }catch(e){say('m_msg','Not saved: '+e.message)}
  b.disabled=false;
}
function showIds(){
  $('i_ip').textContent=M.ip||'?';$('i_mdns').textContent=(M.host||'?')+'.local';
  $('i_ha').textContent=S['MQTT/DeviceName']||'';$('i_host').value=M.host||'';
  $('m_name').value=S['MQTT/DeviceName']||'';
  document.title=(M.host||'Table')+' settings';$('title').textContent=(M.host||'Table')+' settings';
}
async function saveIds(b){
  var h=$('i_host').value.trim(),nm=$('m_name').value.trim(),n=0;
  if(!/^[A-Za-z0-9-]{1,32}$/.test(h)){say('i_msg','Network name: letters, digits and - only.');return}
  if(!nm){say('i_msg','Home Assistant name cannot be empty.');return}
  b.disabled=true;
  try{
    if(h!=M.host){await cmd('$Hostname='+h);n++}
    if(nm!=(S['MQTT/DeviceName']||'')){await cmd('$MQTT/DeviceName='+nm);S['MQTT/DeviceName']=nm;n++}
    M=JSON.parse(await api('/sand_settings_meta'));showIds();
    say('i_msg',n?('Saved.'+(h!=location.hostname.replace(/\.local$/,'')&&/\.local$/.test(location.hostname)?' Reopen the page at '+h+'.local.':'')):'Nothing changed.',true);
  }catch(e){say('i_msg','Not saved: '+e.message)}
  b.disabled=false;
}
async function mqttStatus(){try{M=JSON.parse(await api('/sand_settings_meta'));$('mqst').textContent=M.mqtt}catch(e){}}
async function go(p){try{await api(p);say('t_msg','Sent.',true)}catch(e){say('t_msg',e.message)}}
function gotoRT(th,rh){
  if(th===null)th=ST.theta||0;if(rh===null)rh=Math.max(0,Math.min(1,ST.rho||0));
  go('/sand_goto?theta='+th.toFixed(4)+'&rho='+rh.toFixed(4));
}
function turn(d){gotoRT((ST.theta||0)+d*Math.PI/2,null)}
async function poll(){
  try{var r=await fetch('/sand_status');ST=await r.json();
    $('state').textContent=ST.state;$('fw').textContent=(ST.hostname||'')+' · '+(ST.fw||'');
    $('title').textContent=(ST.hostname||'Table')+' settings';
    if(typeof ST.theta=='number'){$('th').textContent=ST.theta.toFixed(2);$('rh').textContent=ST.rho.toFixed(2)}
  }catch(e){$('state').textContent='offline'}
}
// Axis direction lives in config.yaml; $Config/FlipAxis edits it on the board (no
// file round-trip through the browser, which ran a busy table out of memory).
function showAxes(t){
  ['x','y'].forEach(function(a){var m=t.match(new RegExp('\\b'+a+'=(low|normal)'));
    $('ax_'+a).textContent=m?(m[1]=='low'?'reversed (:low)':'normal'):'not found'});
}
async function loadAxes(){
  try{showAxes(await cmd('$Config/FlipAxis'))}catch(e){say('a_msg','Could not read the axis directions: '+e.message)}
}
async function flip(ax,b){
  var name=ax=='x'?'Theta':ax=='y'?'Rho':'Theta and Rho';
  if(!confirm('Flip '+name+' and restart the table? It will home again after the restart.'))return;
  b.disabled=true;
  try{
    if(ST.state&&ST.state!='Idle'&&ST.state!='Alarm')throw new Error('the table is '+ST.state+'; stop it first');
    say('a_msg','Saving config.yaml…',true);
    showAxes(await cmd('$Config/FlipAxis='+ax));
    say('a_msg','Saved. Restarting…',true);
    try{await cmd('$Bye')}catch(e){}
    setTimeout(function(){say('a_msg','Restarted. The table is homing; test it above.',true);load()},20000);
  }catch(e){say('a_msg','Not changed: '+e.message)}
  b.disabled=false;
}
function opt(sel,val,text){var o=document.createElement('option');o.value=val;o.textContent=text;sel.appendChild(o)}
function addLook(list,spec){
  var f=(spec||'rainbow').split(':'),row=document.createElement('div');row.className='look';
  var ef=document.createElement('select');M.effects.forEach(function(n){opt(ef,n,n)});ef.value=f[0];
  if(ef.value!==f[0]){opt(ef,f[0],f[0]+' (?)');ef.value=f[0]}
  var pa=document.createElement('select');opt(pa,'','default');M.palettes.forEach(function(n){opt(pa,n,n)});pa.value=f[1]||'';
  var sp=document.createElement('input');sp.type='number';sp.min=1;sp.max=255;sp.value=f[2]||'';
  var it=document.createElement('input');it.type='number';it.min=0;it.max=255;it.value=f[3]||'';
  var co=document.createElement('input');co.type='color';co.value=f[4]?'#'+f[4]:'#000000';co.dataset.set=f[4]?'1':'';
  co.oninput=function(){co.dataset.set='1';count()};co.title='Color (set = used by the effect)';
  var x=document.createElement('button');x.textContent='×';x.title='Remove';x.onclick=function(){row.remove();count()};
  var t=document.createElement('button');t.textContent='▶';t.title='Try it now';t.onclick=function(){tryLook(row)};
  [ef,pa,sp,it].forEach(function(e){e.oninput=count});
  [ef,pa,sp,it,co,t,x].forEach(function(e){row.appendChild(e)});
  function cl(e,lo){if(e.value==='')return '';var v=Math.round(+e.value);v=isNaN(v)?lo:Math.max(lo,Math.min(255,v));e.value=v;return String(v)}
  row.spec=function(){var a=[ef.value,pa.value,cl(sp,1),cl(it,0),co.dataset.set?co.value.slice(1):''];
    while(a.length>1&&!a[a.length-1])a.pop();return a.join(':')};
  row.parts={effect:ef,palette:pa,speed:sp,intensity:it,color:co};
  $(list).appendChild(row);count();
}
function fillLooks(list,str){$(list).innerHTML='';str.split(',').forEach(function(s){if(s.trim())addLook(list,s.trim())});count()}
function specOf(list){return Array.prototype.map.call($(list).children,function(r){return r.spec()}).join(',')}
function count(){['run','idle'].forEach(function(l){var s=specOf(l),n=$(l).children.length;
  var e=$(l+'_n');e.textContent=n+' looks, '+s.length+'/240 characters';e.className=s.length>240?'bad':'dim'})}
async function tryLook(row){
  var p=row.parts,q=['effect='+encodeURIComponent(p.effect.value)];
  if(p.palette.value)q.push('palette='+p.palette.value);
  if(p.speed.value)q.push('speed='+p.speed.value);
  if(p.intensity.value)q.push('intensity='+p.intensity.value);
  if(p.color.dataset.set)q.push('color='+p.color.value.slice(1));
  try{await api('/sand_led?'+q.join('&'));say('l_msg','Showing '+row.spec()+' (the rotation takes over again at the next state change).',true)}
  catch(e){say('l_msg','Try failed: '+e.message)}
}
async function saveLooks(b){
  var run=specOf('run'),idle=specOf('idle');
  if(run.length>240||idle.length>240){say('l_msg','A list is over 240 characters: remove a look or drop some optional fields.');return}
  b.disabled=true;
  try{
    var w={'LED/RunLooks':run,'LED/IdleLooks':idle,'LED/LookMinutes':$('l_min').value||'0','LED/LookShuffle':$('l_shuf').checked?'ON':'OFF'},n=0;
    for(var k in w){if(String(S[k]||'')!=w[k]){await cmd('$'+k+'='+w[k]);S[k]=w[k];n++}}
    say('l_msg',n?'Saved.':'Nothing changed.',true);
  }catch(e){say('l_msg','Not saved: '+e.message)}
  b.disabled=false;
}
load();poll();setInterval(function(){if(!document.hidden)poll()},2000);
</script></body></html>)HTML";
}
