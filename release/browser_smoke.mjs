/* Local release smoke test, JS dependencies: none (Node >=22 + Chrome). */
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createServer } from 'node:http';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { resolve,join,sep } from 'node:path';
import { tmpdir } from 'node:os';

const [kind, path] = process.argv.slice(2);
assert.ok(['ghettomachine','beamloom'].includes(kind) && path, 'usage: node release/browser_smoke.mjs ghettomachine|beamloom STATIC_ROOT');
const root=resolve(path);
const browser=process.env.CHROME_PATH || [
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome','/usr/bin/chromium'
].find(existsSync);
assert.ok(browser,'set CHROME_PATH for installed Chrome');
const profile=await mkdtemp(join(tmpdir(),'bong-release-browser-'));
const mime={html:'text/html', js:'text/javascript',mjs:'text/javascript', css:'text/css',
  json:'application/json',wasm:'application/wasm',svg:'image/svg+xml',glsl:'text/plain'};
const server=createServer(async (req,res)=>{
  let p=decodeURIComponent(new URL(req.url,'http://localhost').pathname);
  if(p.endsWith('/'))p+='index.html';
  const file=resolve(root,'.'+p);
  if(!file.startsWith(root+sep)){res.writeHead(403).end();return;}
  try{
    res.setHeader('Content-Type',mime[p.split('.').at(-1)]||'application/octet-stream');
    res.end(await readFile(file));
  }catch{res.writeHead(404).end();}
}).listen(0,'127.0.0.1');
await once(server,'listening');
const chrome=spawn(browser,['--headless','--autoplay-policy=no-user-gesture-required',
  '--no-first-run','--disable-background-networking','--remote-debugging-port=0','--user-data-dir='+profile,'about:blank'],
  {stdio:['ignore','ignore','ignore']});
let ws;
const sleep=ms=>new Promise(res=>setTimeout(res,ms));
try{
  let port;
  for(let i=0;i<120&&!port;i++){
    try{port=(await readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0];}
    catch{await sleep(50);}
  }
  assert.ok(port,'Chrome starts with isolated profile');
  const [target]=await (await fetch('http://127.0.0.1:'+port+'/json/list')).json();
  ws=new WebSocket(target.webSocketDebuggerUrl);
  await once(ws,'open');
  let id=0;
  function call(method,params={}){
    const seq=++id;
    return new Promise((ok,bad)=>{
      const clear=()=>{clearTimeout(timer);ws.removeEventListener('message',on);};
      const timer=setTimeout(()=>{clear();bad(Error(method+' timed out'));},8000);
      const on=event=>{
        const r=JSON.parse(event.data);
        if(r.id!==seq)return;
        clear();
        if(r.error)bad(Error(JSON.stringify(r.error))); else ok(r.result);
      };
      ws.addEventListener('message',on);
      ws.send(JSON.stringify({id:seq,method,params}));
    });
  }
  async function value(expression){
    const r=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
    assert.ok(!r.exceptionDetails,JSON.stringify(r.exceptionDetails));
    return r.result.value;
  }
  async function wait(expression,message){
    for(let i=0;i<100;i++){if(await value(expression))return;await sleep(60);}
    throw Error(message);
  }
  async function nav(p,expect){
    await call('Page.navigate',{url:'http://127.0.0.1:'+server.address().port+p});
    await wait(expect,'did not load '+p);
  }
  await call('Runtime.enable'); await call('Network.enable');
  await call('Network.setBlockedURLs',{urls:['https://fonts.googleapis.com/*','https://fonts.gstatic.com/*']});
  await call('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'}]});
  for(const width of [390,1366]){
    await call('Emulation.setDeviceMetricsOverride',{width,height:844,deviceScaleFactor:1,mobile:width<600});
    if(kind==='ghettomachine'){
      await nav('/','location.pathname==="/" && !!document.querySelector(".gm-home")');
      const claim=await value(`(async()=>{
        const m=await (await fetch('/release.json')).json();
        return {label:document.querySelector('.ver').textContent.trim(),sealed:m.targets.web.landing_label,
        has128:document.querySelector('.gm-home .sub').textContent.includes('128-step'),
        device:m.targets.device.status};
      })()`);
      assert.equal(claim.label,claim.sealed);
      assert.ok(claim.has128 && claim.device==='not-distributed');
      await nav('/guide/recording/#record-notes','location.pathname==="/guide/recording/" && !!document.querySelector(".gm-guide")');
      await wait('getComputedStyle(document.documentElement).getPropertyValue("--bbs-line").trim()==="1px"','shared local theme is not loaded');
      assert.ok(await value('document.documentElement.scrollWidth <= innerWidth+2 && document.querySelector(".gm-guide").textContent.includes("Hold REC and press GLO.")'));
      if(width===390)assert.ok(await value('document.querySelector(".gm-toc-mobile summary").getBoundingClientRect().width>0'));
      await nav('/guide/sounds/#track-inserts','location.pathname==="/guide/sounds/" && !!document.getElementById("track-inserts")');
      assert.ok(await value('document.querySelector(".gm-guide").textContent.includes("Trem / Pan") && document.querySelector(".gm-guide").textContent.includes("hold SEL and tap FX")'));
      await nav('/emu/','location.pathname==="/emu/" && !!document.getElementById("go")');
      assert.ok(await value('document.documentElement.scrollWidth <= innerWidth+2 && document.getElementById("go").textContent==="Switch on"'));
      await value('document.getElementById("go").click()');
      await wait('!!window.emuLast && document.getElementById("start").hidden','the real browser synth/worklet did not switch on');
      await value('document.getElementById("controls").dispatchEvent(new KeyboardEvent("keydown",{code:"KeyP",key:"p",bubbles:true}))');
      await sleep(130);
      await value('window.dispatchEvent(new KeyboardEvent("keyup",{code:"KeyP",key:"p",bubbles:true}))');
      await wait('!!(window.emuLast.buttons & (1<<10))','PLAY did not set the transport light in the running emulator');
    }else{
      await nav('/beamloom/','location.pathname==="/beamloom/" && !!document.querySelector("#beamloom-live.is-ready")');
      await wait('getComputedStyle(document.documentElement).getPropertyValue("--bbs-line").trim()==="1px"','shared local theme is not loaded');
      const data=await value(`(async()=>{
        const root=document.querySelector('#beamloom-live');
        const launch=root.querySelector('[data-native-script="generate"]');
        const canvas=root.querySelector('[data-beamloom="native-canvas"]');
        const motion=root.querySelector('[data-beamloom="motion"]');
        const source=root.querySelector('[data-native-script="source"]');
        launch.click();
        for(let i=0;i<100&&launch.disabled;i++)await new Promise(r=>setTimeout(r,30));
        const first=canvas.toDataURL(); await new Promise(r=>setTimeout(r,150));
        const visible=root.classList.contains('is-native')&&getComputedStyle(canvas).display==='block';
        return {visible,width:canvas.width,height:canvas.height,source:source.value, label:launch.textContent,
         paused:motion.textContent==='Resume visual'&&first===canvas.toDataURL(),fits:launch.getBoundingClientRect().right<=innerWidth+1};
      })()`);
      assert.ok(data.visible&&data.paused&&data.fits,JSON.stringify(data));
      assert.deepEqual([data.width,data.height,data.label],[576,360,'Generate visual']);
      assert.match(data.source,/^native-signal 3\.0/);
      assert.match(data.source,/layer l1 = field\(\)/);
    }
    console.log(`PASS ${kind} ${width}px, local theme and real interactive runtime`);
  }
}finally{
  ws?.close();
  if(chrome.exitCode===null){chrome.kill();await once(chrome,'exit');}
  await new Promise(r=>server.close(r));
  await rm(profile,{recursive:true,force:true});
}
