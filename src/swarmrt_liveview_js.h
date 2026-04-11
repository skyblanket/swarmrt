/*
 * SwarmRT LiveView: Embedded Client JavaScript
 *
 * Contains:
 * 1. Minimal morphdom — in-place DOM diffing (~30 lines)
 * 2. LiveView client — WebSocket + event bindings (~50 lines)
 *
 * Event attributes:
 *   sw-click="event"   → sends {event, value} on click
 *   sw-submit="event"  → sends {event, value} on form submit (value = form data)
 *   sw-input="event"   → sends {event, value} on each keystroke
 *   sw-change="event"  → sends {event, value} on change
 *   sw-value-name="v"  → extra data attached to sw-click events
 *
 * otonomy.ai
 */

#ifndef SWARMRT_LIVEVIEW_JS_H
#define SWARMRT_LIVEVIEW_JS_H

static const char SWARMRT_LIVEVIEW_JS[] =
"(function(){\n"
"\n"
"/* Minimal morphdom: patch existing DOM in-place from new HTML */\n"
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
"    var fc=Array.from(f.childNodes),tc=Array.from(t.childNodes);\n"
"    for(i=0;i<tc.length;i++){\n"
"      if(i<fc.length)morph(fc[i],tc[i]);\n"
"      else f.appendChild(tc[i].cloneNode(true))}\n"
"    while(f.childNodes.length>tc.length)f.removeChild(f.lastChild)}\n"
"}\n"
"\n"
"/* WebSocket connection with exponential backoff */\n"
"var ws,retries=0;\n"
"function connect(){\n"
"  var proto=location.protocol==='https:'?'wss:':'ws:';\n"
"  ws=new WebSocket(proto+'//'+location.host+'/live/ws');\n"
"  ws.onopen=function(){retries=0};\n"
"  ws.onmessage=function(e){\n"
"    try{\n"
"      var msg=JSON.parse(e.data);\n"
"      if(msg.type==='html'){\n"
"        var tmp=document.createElement('div');\n"
"        tmp.innerHTML=msg.body;\n"
"        var root=document.getElementById('live-root');\n"
"        if(root&&tmp.firstChild)morph(root,tmp.firstChild);\n"
"        bind()}\n"
"    }catch(err){console.error('LiveView:',err)}\n"
"  };\n"
"  ws.onclose=function(){\n"
"    var delay=Math.min(1000*Math.pow(2,retries++),30000);\n"
"    setTimeout(connect,delay)};\n"
"  ws.onerror=function(){}\n"
"}\n"
"\n"
"/* Send event to server */\n"
"function send(ev,val){\n"
"  if(ws&&ws.readyState===1)\n"
"    ws.send(JSON.stringify({event:ev,value:val||{}}))\n"
"}\n"
"\n"
"/* Collect sw-value-* attributes from element */\n"
"function getVals(el){\n"
"  var v={};\n"
"  Array.from(el.attributes).forEach(function(a){\n"
"    if(a.name.indexOf('sw-value-')===0)\n"
"      v[a.name.slice(9)]=a.value});\n"
"  return v\n"
"}\n"
"\n"
"/* Bind event handlers after each morph */\n"
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
"connect()\n"
"\n"
"})();\n";

#endif /* SWARMRT_LIVEVIEW_JS_H */
