'use strict';
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));
const state = {
  lang:'ru', screen:'dash', fails:0, tele:null, tr:null, info:null,
  bt:{scanning:false,devices:[]}, btStatus:null,
  settings:{accent_color:'#3B82F6',units:'metric',brightness:70,mochi_enabled:true,splash_enabled:true}
};
function t(k,v){ let s=(window.I18N[state.lang]||{})[k]||k; if(v) for(const p in v) s=s.split('{'+p+'}').join(v[p]); return s; }
function applyI18n(){ $$('[data-i18n]').forEach(e=>{ e.textContent=t(e.dataset.i18n); }); }
async function api(p,o){ const r=await fetch(p,o); if(!r.ok) throw new Error(r.status); const x=await r.text(); try{return JSON.parse(x)}catch(e){return{}} }
function post(p,b){ return api(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})}); }
function fmtKB(n){ return (n/1024).toFixed(0)+' KB'; }
function toast(msg,kind,ms){ const el=document.createElement('div'); el.className='toast '+(kind||''); el.textContent=msg; $('#toasts').appendChild(el); setTimeout(()=>el.remove(), ms||3000); }
function showModal(o){ $('#mTitle').textContent=o.title||''; $('#mBody').innerHTML=o.body||''; const ac=$('#mActions'); ac.innerHTML=''; (o.actions||[{label:t('c.ok')}]).forEach(a=>{ const b=document.createElement('button'); b.className='btn '+(a.cls||'btn-dark'); b.textContent=a.label; b.onclick=()=>{ hideModal(); if(a.onClick) a.onClick(); }; ac.appendChild(b); }); $('#modalRoot').hidden=false; }
function hideModal(){ $('#modalRoot').hidden=true; }
function confirmModal(title,text,cb){ showModal({title, body:'<p>'+text+'</p>', actions:[{label:t('c.ok'),cls:'btn-primary',onClick:cb},{label:t('c.cancel')}]}); }
$('#modalRoot').querySelector('.modal-bg').onclick=hideModal;

/* ---------- роутер ---------- */
const TITLES={dash:'',trans:'trans.sub',settings:'set.theme',system:'sys.title'};
function showScreen(id){
  state.screen=id;
  $$('.screen').forEach(s=>s.classList.toggle('active', s.id==='screen-'+id));
  $('#btnBack').hidden = (id==='dash');
  $('#hdrTitle').textContent = id==='dash' ? 'Smart Gauge' : t({trans:'nav.trans',settings:'nav.settings',system:'nav.system'}[id]);
  if(id==='trans') loadTr();
  if(id==='settings') loadSettings();
  if(id==='system') loadSystem();
  window.scrollTo(0,0);
}
$$('.nav-card').forEach(a=>a.onclick=e=>{ e.preventDefault(); showScreen(a.getAttribute('href').slice(1)); });
$('#btnBack').onclick=()=>showScreen('dash');

/* ---------- главная ---------- */
function rpmColor(r){ const f=r/7000; return f<0.65?'#22c55e':(f<0.85?'#f59e0b':'#ef4444'); }
function renderDash(){
  const p=state.tele; if(!p) return;
  const imp = state.settings.units==='imperial';
  $('#dashStatus').textContent = p.linked ? t('dash.online') : t('dash.offline');
  $('#hdrDot').classList.toggle('on', !!p.linked);
  $('#gearBig').textContent = p.gear;
  $('#gearSub').textContent = p.linked ? '' : t('dash.waiting');
  const pct=Math.min(100,(p.rpm/7000)*100);
  const fg=$('#ringFg'); fg.style.strokeDasharray=(pct*0.75)+' 100'; fg.style.stroke=rpmColor(p.rpm);
  $('#vRpm').textContent=p.rpm+' rpm';
  $('#vSpeed').textContent=(imp?(p.speed*0.621371).toFixed(0):p.speed)+(imp?' mph':' km/h');
  $('#vTemp').textContent=(imp?(p.temp*9/5+32).toFixed(0):p.temp)+(imp?' °F':' °C');
  $('#vVolt').textContent=(p.voltage/1000).toFixed(2)+' V';
  $('#vFuel').textContent=p.fuel+'%';
}
async function pollDash(){
  try{ state.tele=await api('/api/telemetry'); state.fails=0; $('#lostOverlay').hidden=true; renderDash(); }
  catch(e){ if(++state.fails>=3) $('#lostOverlay').hidden=false; }
}
$('#btnRetry').onclick=()=>{ state.fails=0; $('#lostOverlay').hidden=true; pollDash(); };

/* ---------- трансмиссия ---------- */
const GEAR_NAMES={1:'1',2:'2',3:'3',4:'4',5:'5',6:'R'};
function renderTr(){
  const tr=state.tr; if(!tr) return;
  $('#trLiveK').textContent = tr.liveK>0 ? tr.liveK.toFixed(1) : '—';
  $('#trRecState').textContent = tr.rec>=1 ? t('trans.recGear',{g:GEAR_NAMES[tr.rec]}) : t('trans.idle');
  $('#trProg').style.width = (tr.rec>=1?tr.prog:0)+'%';
  const box=$('#gearRows'); box.innerHTML='';
  for(let g=1; g<=6; g++){
    const has=(tr.mask>>g)&1, k=tr.k[g]||0;
    const row=document.createElement('div'); row.className='g-row';
    row.innerHTML='<span class="g-name">'+GEAR_NAMES[g]+'</span>'+
      '<span class="g-k"><b>'+(has?k.toFixed(2):'—')+'</b><span class="sub">'+(has?'K':t('trans.notSet'))+'</span></span>'+
      '<span class="badge'+(has?' ok':'')+'">'+(has?'OK':'—')+'</span>'+
      '<input type="number" step="0.1" min="1" placeholder="K" data-g="'+g+'">'+
      '<button class="btn btn-dark" data-rec="'+g+'">'+t('trans.record')+'</button>';
    box.appendChild(row);
  }
  $$('#gearRows [data-rec]').forEach(b=>b.onclick=async()=>{
    const g=+b.dataset.rec;
    try{ await post('/api/transmission/record',{gear:g}); toast(t('trans.recStarted',{g:GEAR_NAMES[g]}),'ok'); loadTr(); }
    catch(e){ toast(t('toast.saveFail'),'err'); }
  });
  $$('#gearRows input').forEach(inp=>inp.onchange=async()=>{
    const g=+inp.dataset.g, k=parseFloat(inp.value);
    if(!(k>0)) return;
    try{ await post('/api/transmission/set',{gear:g,k:k}); toast(t('trans.saved'),'ok'); loadTr(); }
    catch(e){ toast(t('toast.saveFail'),'err'); }
  });
  $('#inpTol').value=Math.round(tr.tol*100);
  $('#inpRmax').value=tr.rmax;
  const wrap=$('#fdPropWrap');
  if(tr.prop && tr.prop.length && !tr.fd){ wrap.hidden=false; $('#fdPropList').innerHTML=tr.prop.map((v,i)=>'<div class="bt-dev"><span class="nm">'+t('trans.gears')+' '+(tr.prop.length-i)+'</span><b>'+v.toFixed(2)+'</b></div>').join(''); }
  else wrap.hidden=true;
}
async function loadTr(){ try{ state.tr=await api('/api/transmission'); renderTr(); }catch(e){} }
$('#btnFdStart').onclick=async()=>{ try{ await post('/api/transmission/freedrive',{action:'start'}); toast(t('trans.fdStart'),'ok'); loadTr(); }catch(e){ toast(t('toast.saveFail'),'err'); } };
$('#btnFdStop').onclick=async()=>{ try{ await post('/api/transmission/freedrive',{action:'stop'}); loadTr(); }catch(e){ toast(t('toast.saveFail'),'err'); } };
$('#btnFdConfirm').onclick=async()=>{ try{ await post('/api/transmission/freedrive',{action:'confirm'}); toast(t('trans.saved'),'ok'); loadTr(); }catch(e){ toast(t('toast.saveFail'),'err'); } };
$('#btnTrParams').onclick=async()=>{
  try{ await post('/api/transmission/params',{tol:(+$('#inpTol').value)/100, rmax:+$('#inpRmax').value}); toast(t('trans.saved'),'ok'); loadTr(); }
  catch(e){ toast(t('toast.saveFail'),'err'); }
};
$('#btnTrReset').onclick=()=>confirmModal(t('trans.resetAll'), t('trans.resetConfirm'), async()=>{
  try{ await post('/api/transmission/reset',{}); toast(t('trans.saved'),'ok'); loadTr(); }catch(e){ toast(t('toast.saveFail'),'err'); }
});

/* ---------- настройки ---------- */
function applyAccent(){ document.documentElement.style.setProperty('--accent', state.settings.accent_color); $$('#swatchRow .swatch').forEach(b=>b.classList.toggle('sel', b.dataset.color.toUpperCase()===state.settings.accent_color.toUpperCase())); }
function renderUnits(){ $('#btnUnitMetric').classList.toggle('on', state.settings.units==='metric'); $('#btnUnitImperial').classList.toggle('on', state.settings.units==='imperial'); }
function renderSwitches(){ $('#swMochi').checked=!!state.settings.mochi_enabled; $('#swSplash').checked=!!state.settings.splash_enabled; }
async function loadSettings(){
  try{ const s=await api('/api/settings'); Object.assign(state.settings,s); if(s.language&&s.language!==state.lang){ state.lang=s.language; applyI18n(); } }catch(e){}
  applyAccent(); renderUnits(); renderSwitches();
  $('#rngBright').value=state.settings.brightness; $('#brightVal').textContent=state.settings.brightness;
}
$$('#swatchRow .swatch').forEach(b=>b.onclick=()=>{ state.settings.accent_color=b.dataset.color; applyAccent(); });
$('#btnUnitMetric').onclick=()=>{ state.settings.units='metric'; renderUnits(); };
$('#btnUnitImperial').onclick=()=>{ state.settings.units='imperial'; renderUnits(); };
$('#rngBright').oninput=e=>{ state.settings.brightness=+e.target.value; $('#brightVal').textContent=e.target.value; };
$('#swMochi').onchange=e=>state.settings.mochi_enabled=e.target.checked;
$('#swSplash').onchange=e=>state.settings.splash_enabled=e.target.checked;
$('#btnSaveSettings').onclick=async()=>{
  try{
    await post('/api/settings',{accent_color:state.settings.accent_color,units:state.settings.units,brightness:+state.settings.brightness,mochi_enabled:!!state.settings.mochi_enabled,splash_enabled:!!state.settings.splash_enabled,language:state.lang});
    toast(t('set.saved'),'ok');
  }catch(e){ toast(t('toast.saveFail'),'err'); }
};

/* ---------- система ---------- */
async function loadSystem(){
  try{
    const i=await api('/api/info'); state.info=i;
    $('#sysVer').textContent=i.firmware_version||'—';
    $('#sysDev').textContent=i.device_name||'Smart Gauge';
    $('#sysDiag').textContent=t('sys.diagLine',{u:i.uptime||0,h:Math.round((i.free_heap||0)/1024)});
    $('#footVer').textContent='Smart Gauge v'+(i.firmware_version||'0');
  }catch(e){}
  loadBt();
}
async function loadBt(){
  try{
    const s=await api('/api/bt/status'); state.btStatus=s;
    $('#btStatus').textContent = s.connected ? t('sys.btConnected',{m:s.mac}) : t('sys.btDisconnected');
    const l=await api('/api/bt/list'); state.bt=l;
    const box=$('#btList');
    if(l.scanning){ box.innerHTML='<div class="sub">'+t('sys.btScanning')+'</div>'; }
    else if(!l.devices.length){ box.innerHTML='<div class="sub">'+t('sys.btEmpty')+'</div>'; }
    else{
      box.innerHTML='';
      l.devices.forEach(d=>{
        const row=document.createElement('div'); row.className='bt-dev';
        row.innerHTML='<span class="nm">'+(d.name||d.mac)+'</span><span class="sub">'+d.mac+'</span><button class="btn btn-primary">'+t('sys.btConnect')+'</button>';
        row.querySelector('button').onclick=async()=>{
          try{ const r=await post('/api/bt/connect',{mac:d.mac}); if(r.success) toast(t('sys.btConnected',{m:d.mac}),'ok'); else toast(r.error||t('toast.saveFail'),'err'); loadBt(); }
          catch(e){ toast(t('toast.saveFail'),'err'); }
        };
        box.appendChild(row);
      });
    }
  }catch(e){}
}
$('#btnBtScan').onclick=async()=>{ try{ await post('/api/bt/scan',{}); setTimeout(loadBt,1500); setTimeout(loadBt,5000); setTimeout(loadBt,9000); }catch(e){ toast(t('toast.saveFail'),'err'); } };
$('#btnBtDisc').onclick=async()=>{ try{ await post('/api/bt/disconnect',{}); loadBt(); }catch(e){ toast(t('toast.saveFail'),'err'); } };

/* OTA */
$('#fileOta').onchange=e=>{
  const f=e.target.files[0]; if(!f) return; e.target.value='';
  confirmModal(t('sys.update'), t('sys.otaConfirm',{f:f.name,s:fmtKB(f.size)}), ()=>startOta(f));
};
function startOta(f){
  const fd=new FormData(); fd.append('firmware', f);
  const xhr=new XMLHttpRequest();
  $('#otaWrap').hidden=false; $('#otaPct').textContent=t('sys.otaUp');
  xhr.open('POST','/api/ota');
  xhr.upload.onprogress=e=>{ if(e.lengthComputable){ const p=Math.round(e.loaded/e.total*100); $('#otaBar').style.width=p+'%'; $('#otaPct').textContent=p+'%'; } };
  xhr.onload=()=>{ $('#otaWrap').hidden=true; $('#otaBar').style.width='0';
    if(xhr.status===200) confirmModal(t('sys.update'), t('sys.otaOk'), ()=>post('/api/reboot',{}));
    else toast(t('sys.otaFail'),'err'); };
  xhr.onerror=()=>{ $('#otaWrap').hidden=true; toast(t('sys.otaFail'),'err'); };
  xhr.send(fd);
}
const FW_URL='https://github.com/pashapchelkinnn?tab=repositories';
$('#rowDownload').onclick=()=>showModal({title:t('sys.download'),
  body:'<p>'+t('sys.downloadDesc')+'</p><p style="word-break:break-all;margin-top:8px"><a href="'+FW_URL+'" target="_blank" rel="noopener" style="color:var(--accent)">'+FW_URL+'</a></p>',
  actions:[{label:t('sys.openLink'),cls:'btn-primary',onClick:()=>window.open(FW_URL,'_blank')},
           {label:t('sys.copyLink'),onClick:()=>{ if(navigator.clipboard) navigator.clipboard.writeText(FW_URL); toast(t('sys.copied'),'ok'); }},
           {label:t('c.cancel')}]});
/* бэкап */
$('#rowBackup').onclick=()=>showModal({title:t('sys.backupTitle'),body:'<p>'+t('sys.backupDesc2')+'</p>',
  actions:[{label:t('sys.export'),cls:'btn-primary',onClick:backupExport},{label:t('sys.import'),onClick:()=>$('#fileImport').click()},{label:t('c.cancel')}]});
async function backupExport(){
  try{
    const s=await api('/api/settings'), tr=await api('/api/transmission');
    const blob=new Blob([JSON.stringify({settings:s,transmission:tr},null,2)],{type:'application/json'});
    const a=document.createElement('a'); a.href=URL.createObjectURL(blob); a.download='smart-gauge-settings.json'; a.click();
    toast(t('sys.exportOk'),'ok');
  }catch(e){ toast(t('toast.saveFail'),'err'); }
}
$('#fileImport').onchange=e=>{
  const f=e.target.files[0]; if(!f) return; e.target.value='';
  const rd=new FileReader();
  rd.onload=async()=>{
    try{
      const d=JSON.parse(rd.result);
      if(d.settings) await post('/api/settings',d.settings);
      if(d.transmission&&d.transmission.k){ for(let g=1;g<=6;g++){ if(d.transmission.k[g]>0) await post('/api/transmission/set',{gear:g,k:d.transmission.k[g]}); } }
      toast(t('sys.importOk'),'ok'); loadSettings();
    }catch(err){ toast(t('toast.saveFail'),'err'); }
  };
  rd.readAsText(f);
};
/* прочее */
$('#rowReboot').onclick=()=>confirmModal(t('sys.reboot'), t('sys.rebootConfirm'), async()=>{ toast(t('sys.rebooting'),'warn',6000); try{ await post('/api/reboot',{}); }catch(e){} });
$('#rowWifi').onclick=()=>{
  showModal({title:t('sys.wifiTitle'), body:'<input id="wifiPass" type="text" placeholder="'+t('sys.wifiPh')+'">',
    actions:[{label:t('c.save'),cls:'btn-primary',onClick:async()=>{
      const p=$('#wifiPass').value;
      if(p.length<8){ toast(t('sys.wifiShort'),'err'); return; }
      try{ await post('/api/wifi/password',{password:p}); toast(t('sys.wifiOk'),'ok',5000); }catch(e){ toast(t('toast.saveFail'),'err'); }
    }},{label:t('c.cancel')}]});
};
$('#rowService').onclick=()=>toast(t('sys.serviceOk'),'info',5000);
$('#rowReset').onclick=()=>confirmModal(t('sys.reset'), t('sys.resetConfirm'), async()=>{ try{ await post('/api/factory_reset',{}); toast(t('sys.resetOk'),'ok',5000); }catch(e){} });

/* ---------- тикер ---------- */
setInterval(()=>{
  if(state.screen==='dash') pollDash();
  else if(state.screen==='trans') loadTr();
  else if(state.screen==='system' && state.bt && state.bt.scanning) loadBt();
},1000);

/* старт */
applyI18n(); applyAccent(); pollDash();