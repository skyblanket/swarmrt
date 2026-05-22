/*
 * SwarmRT LiveView: Embedded Client JavaScript
 *
 * The diff client. The server holds UI state; this client holds two
 * arrays — `statics` (sent once at mount) and `dynamics` (patched by
 * index on every diff) — stitches them into HTML, and morphdom-patches
 * the DOM in place.
 *
 * Wire protocol:
 *   server -> client   {t:"m", s:[statics], d:[dynamics]}   mount
 *   server -> client   {t:"d", c:{"idx":"newval", ...}}      diff
 *   client -> server   {t:"e", n:"event", v:{...}}           event
 *
 * Event attributes:
 *   sw-click="event"   -> {t:e,n:event,v:<sw-value-* attrs>} on click
 *   sw-submit="event"  -> {t:e,n:event,v:<form data>}        on submit
 *   sw-input="event"   -> {t:e,n:event,v:<value>}            on keystroke
 *   sw-change="event"  -> {t:e,n:event,v:<value>}            on change
 *   sw-value-name="v"  -> extra data carried on sw-click events
 *
 * otonomy.ai
 */

#ifndef SWARMRT_LIVEVIEW_JS_H
#define SWARMRT_LIVEVIEW_JS_H

static const char SWARMRT_LIVEVIEW_JS[] =
"(function(){\n"
"\n"
"/* Minimal morphdom: patch existing DOM in-place from a new node. */\n"
"function morph(f,t){\n"
"  if(f.nodeType!==t.nodeType||f.nodeName!==t.nodeName){\n"
"    f.parentNode.replaceChild(t.cloneNode(true),f);return}\n"
"  if(f.nodeType===3||f.nodeType===8){\n"
"    if(f.nodeValue!==t.nodeValue)f.nodeValue=t.nodeValue;return}\n"
"  if(f.nodeType===1){\n"
"    var i,a=t.attributes;\n"
"    for(i=0;i<a.length;i++)\n"
"      if(f.getAttribute(a[i].name)!==a[i].value)\n"
"        f.setAttribute(a[i].name,a[i].value);\n"
"    a=f.attributes;\n"
"    for(i=a.length-1;i>=0;i--)\n"
"      if(!t.hasAttribute(a[i].name))f.removeAttribute(a[i].name);\n"
"    /* Don't stomp a focused input's value mid-type. */\n"
"    if(f.nodeName==='INPUT'||f.nodeName==='TEXTAREA'){\n"
"      if(document.activeElement!==f&&f.value!==t.getAttribute('value'))\n"
"        if(t.hasAttribute('value'))f.value=t.getAttribute('value')}\n"
"    var fc=Array.prototype.slice.call(f.childNodes);\n"
"    var tc=Array.prototype.slice.call(t.childNodes);\n"
"    for(i=0;i<tc.length;i++){\n"
"      if(i<fc.length)morph(fc[i],tc[i]);\n"
"      else f.appendChild(tc[i].cloneNode(true))}\n"
"    while(f.childNodes.length>tc.length)f.removeChild(f.lastChild)}\n"
"}\n"
"\n"
"/* LiveView client state: the two halves of every render. */\n"
"var ws,retries=0,statics=[],dynamics=[];\n"
"\n"
"/* Stitch statics + dynamics into HTML, then morph #live-root. */\n"
"function render(){\n"
"  var html=statics.length?statics[0]:'';\n"
"  for(var i=0;i<dynamics.length;i++)\n"
"    html+=(dynamics[i]==null?'':dynamics[i])+(statics[i+1]==null?'':statics[i+1]);\n"
"  var root=document.getElementById('live-root');\n"
"  if(!root)return;\n"
"  var tmp=document.createElement('div');\n"
"  tmp.innerHTML=html;\n"
"  var fc=Array.prototype.slice.call(root.childNodes);\n"
"  var tc=Array.prototype.slice.call(tmp.childNodes),i;\n"
"  for(i=0;i<tc.length;i++){\n"
"    if(i<fc.length)morph(fc[i],tc[i]);\n"
"    else root.appendChild(tc[i])}\n"
"  while(root.childNodes.length>tc.length)root.removeChild(root.lastChild);\n"
"  bind();\n"
"}\n"
"\n"
"/* WebSocket connection with exponential backoff. */\n"
"function connect(){\n"
"  var proto=location.protocol==='https:'?'wss:':'ws:';\n"
"  ws=new WebSocket(proto+'//'+location.host+'/live/ws');\n"
"  ws.onopen=function(){retries=0};\n"
"  ws.onmessage=function(e){\n"
"    var msg;\n"
"    try{msg=JSON.parse(e.data)}catch(err){return}\n"
"    if(msg.t==='m'){\n"
"      statics=msg.s||[];dynamics=msg.d||[];render()}\n"
"    else if(msg.t==='d'){\n"
"      var c=msg.c||{},k;\n"
"      for(k in c)dynamics[parseInt(k,10)]=c[k];\n"
"      render()}\n"
"  };\n"
"  ws.onclose=function(){\n"
"    var delay=Math.min(1000*Math.pow(2,retries++),30000);\n"
"    setTimeout(connect,delay)};\n"
"  ws.onerror=function(){};\n"
"}\n"
"\n"
"/* Send an event to the server. */\n"
"function send(n,v){\n"
"  if(ws&&ws.readyState===1)\n"
"    ws.send(JSON.stringify({t:'e',n:n,v:v||{}}))\n"
"}\n"
"\n"
"/* Collect sw-value-* attributes off an element. */\n"
"function getVals(el){\n"
"  var v={};\n"
"  Array.prototype.forEach.call(el.attributes,function(a){\n"
"    if(a.name.indexOf('sw-value-')===0)\n"
"      v[a.name.slice(9)]=a.value});\n"
"  return v\n"
"}\n"
"\n"
"/* (Re)bind event handlers after each render. */\n"
"function bind(){\n"
"  document.querySelectorAll('[sw-click]').forEach(function(el){\n"
"    el.onclick=function(e){e.preventDefault();\n"
"      send(el.getAttribute('sw-click'),getVals(el))}});\n"
"  document.querySelectorAll('[sw-submit]').forEach(function(el){\n"
"    el.onsubmit=function(e){e.preventDefault();\n"
"      var v={};\n"
"      new FormData(el).forEach(function(val,k){v[k]=val});\n"
"      send(el.getAttribute('sw-submit'),v)}});\n"
"  document.querySelectorAll('[sw-input]').forEach(function(el){\n"
"    el.oninput=function(){\n"
"      send(el.getAttribute('sw-input'),el.value)}});\n"
"  document.querySelectorAll('[sw-change]').forEach(function(el){\n"
"    el.onchange=function(){\n"
"      send(el.getAttribute('sw-change'),el.value)}})\n"
"}\n"
"\n"
"connect();\n"
"\n"
"})();\n";

#endif /* SWARMRT_LIVEVIEW_JS_H */
