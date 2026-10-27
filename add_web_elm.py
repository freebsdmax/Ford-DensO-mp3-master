p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
if 'ELM327 Emulator' not in t:
    section = '''      <div class="card">
        <div class="card-header"><h2>ELM327 Emulator</h2></div>
        <div class="card-body">
          <label><input type="checkbox" id="elm-en"> Enable ELM327 emulator</label><br><br>
          <label><input type="checkbox" id="elm-wifi"> Enable Wi-Fi TCP (port 35000)</label><br>
          <label><input type="checkbox" id="elm-bt"> Enable Bluetooth Classic SPP</label><br><br>
          <button class="btn" type="button" onclick="loadElm327()">Load</button>
          <button class="btn btn-primary" type="button" onclick="saveElm327()">Save</button>
          <div id="elm327-status" style="margin-top:10px;"></div>
        </div>
      </div>
'''
    marker = '<div class="card">\n        <div class="card-header"><h2>OTA'
    if marker in t:
        t2 = t.replace(marker, section + '\n' + marker)
    else:
        t2 = t.replace('</body>', section + '\n</body>')
    js = '''<script>
function loadElm327(){
  fetch('/elm327_config').then(function(r){return r.json();}).then(function(c){
    document.getElementById('elm-en').checked = !!c.enabled;
    document.getElementById('elm-wifi').checked = !!c.wifi_enabled;
    document.getElementById('elm-bt').checked = !!c.bt_enabled;
    var s=document.getElementById('elm327-status'); if(s) s.innerText='Loaded';
  });
}
function saveElm327(){
  var body = JSON.stringify({enabled: document.getElementById('elm-en').checked, wifi_enabled: document.getElementById('elm-wifi').checked, bt_enabled: document.getElementById('elm-bt').checked});
  fetch('/elm327_config',{method:'POST',headers:{'Content-Type':'application/json'},body:body}).then(function(r){return r.json();}).then(function(){ var s=document.getElementById('elm327-status'); if(s) s.innerText='Saved'; });
}
window.addEventListener('load', function(){ try{ loadElm327(); }catch(e){} });
</script>
'''
    if 'function loadElm327' not in t2:
        t2 = t2.replace('</body>', js + '\n</body>')
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('has')
