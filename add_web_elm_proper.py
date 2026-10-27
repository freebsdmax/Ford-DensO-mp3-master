p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()

# Add card for ELM327 in the HTML - look for OTA card as anchor
if 'elm327_config' not in t or 'Enable ELM327' not in t:
    card = '''      <div class="card">
        <div class="card-header"><h2>ELM327 Emulator</h2></div>
        <div class="card-body">
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-enabled"> Enable ELM327</label>
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-wifi"> Wi-Fi TCP (port 35000)</label>
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-bt"> Bluetooth Classic SPP</label>
          <div style="margin-top:10px;">
            <button class="btn" type="button" onclick="loadElm327Cfg()">Reload</button>
            <button class="btn btn-primary" type="button" onclick="saveElm327Cfg()">Save</button>
            <span id="elm-status" style="margin-left:10px;"></span>
          </div>
        </div>
      </div>
'''
    # Try to insert after Settings card
    marker = '<div class="card">\n        <div class="card-header"><h2>OTA'
    if marker in t:
        t2 = t.replace(marker, card + '\n' + marker)
        print('ins1')
    else:
        marker2 = '</div>\n</div>\n</body>'
        t2 = t.replace(marker2, '</div>\n</div>\n' + card + '\n</body>')
        print('ins2')
    
    # Add JS
    js = '''
function loadElm327Cfg(){
  fetch('/elm327_config').then(function(r){return r.json();}).then(function(c){
    document.getElementById('elm-enabled').checked = !!c.enabled;
    document.getElementById('elm-wifi').checked = !!c.wifi_enabled;
    document.getElementById('elm-bt').checked = !!c.bt_enabled;
    var s=document.getElementById('elm-status'); if(s) s.textContent='Loaded';
  }).catch(function(e){ var s=document.getElementById('elm-status'); if(s) s.textContent='Error'; });
}
function saveElm327Cfg(){
  var data = {
    enabled: document.getElementById('elm-enabled').checked,
    wifi_enabled: document.getElementById('elm-wifi').checked,
    bt_enabled: document.getElementById('elm-bt').checked
  };
  fetch('/elm327_config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)}).then(function(r){return r.json();}).then(function(){
    var s=document.getElementById('elm-status'); if(s) s.textContent='Saved'; setTimeout(function(){if(s)s.textContent='';},1500);
  }).catch(function(e){ var s=document.getElementById('elm-status'); if(s) s.textContent='Error'; });
}
window.addEventListener('DOMContentLoaded', function(){ try{ loadElm327Cfg(); }catch(e){} });
'''
    if 'loadElm327Cfg' not in t2:
        if '</script>' in t2:
            t2 = t2.replace('</script>', js + '\n</script>')
            print('js')
        else:
            t2 = t2.replace('</body>', '<script>' + js + '</script>\n</body>')
            print('js2')
    open(p,'w',encoding='utf-8').write(t2)
else:
    print('exists')
