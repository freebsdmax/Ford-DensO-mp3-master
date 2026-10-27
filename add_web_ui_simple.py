p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()

# Add a simple ELM card by inserting into the HTML string content
# Find the footer line and insert before it
if '"<footer' in t:
    card = '"<div class=\\"card\\"><div class=\\"card-header\\"><h2>ELM327 Emulator</h2></div><div class=\\"card-body\\"><label><input type=\\"checkbox\\" id=\\"elm-en\\"> Enable</label><br><label><input type=\\"checkbox\\" id=\\"elm-wifi\\" checked> Wi-Fi TCP 35000</label><br><label><input type=\\"checkbox\\" id=\\"elm-bt\\"> Bluetooth SPP</label><br><br><button onclick=\\"loadElm()\\" class=\\"btn\\">Load</button> <button onclick=\\"saveElm()\\" class=\\"btn btn-primary\\">Save</button> <span id=\\"elm-st\\"></span></div></div>"\n'
    js = '"<script>function loadElm(){fetch(\\"/elm327_config\\").then(r=>r.json()).then(c=>{document.getElementById(\\"elm-en\\").checked=!!c.enabled;document.getElementById(\\"elm-wifi\\").checked=!!c.wifi_enabled;document.getElementById(\\"elm-bt\\").checked=!!c.bt_enabled;document.getElementById(\\"elm-st\\").innerText=\\"OK\\";});}function saveElm(){var d={enabled:document.getElementById(\\"elm-en\\").checked,wifi_enabled:document.getElementById(\\"elm-wifi\\").checked,bt_enabled:document.getElementById(\\"elm-bt\\").checked};fetch(\\"/elm327_config\\",{method:\\"POST\\",headers:{\\"Content-Type\\":\\"application/json\\"},body:JSON.stringify(d)}).then(r=>r.json()).then(()=>{document.getElementById(\\"elm-st\\").innerText=\\"Saved\\";setTimeout(()=>{document.getElementById(\\"elm-st\\").innerText=\\"\\";},1000);});}window.onload=function(){loadComponentLogs();try{loadElm();}catch(e){}};</script>"\n'
    
    # Replace footer
    footer = '"<footer style=\'text-align:center;padding:1rem;color:#666;font-size:0.9rem;\'>ESP32 Music Player � web control � by your device</footer>"\n'
    if card not in t:
        t2 = t.replace(footer, card + footer)
        print('card')
    else:
        t2 = t
        print('has')
    
    # Update onload
    if 'window.onload=function(){loadComponentLogs();try{loadElm();}' not in t2:
        old_on = '"window.onload = function() { loadComponentLogs(); };"\n'
        new_on = '"window.onload = function() { loadComponentLogs(); try{loadElm();}catch(e){} };"\n'
        if old_on in t2:
            t2 = t2.replace(old_on, new_on)
            print('onload')
    
    open(p,'w',encoding='utf-8').write(t2)
else:
    print('no')
