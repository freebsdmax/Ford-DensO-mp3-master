p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()

# Check if proper ELM card exists
if 'id="elm-enabled"' not in t and '<h2>ELM327 Emulator</h2>' not in t:
    card = '''      <div class="card">
        <div class="card-header"><h2>ELM327 Emulator</h2></div>
        <div class="card-body">
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-enabled"> Enable ELM327</label><br>
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-wifi"> Wi-Fi TCP (port 35000)</label><br>
          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-bt"> Bluetooth Classic SPP</label><br><br>
          <button class="btn" type="button" onclick="loadElm327Cfg()">Reload</button>
          <button class="btn btn-primary" type="button" onclick="saveElm327Cfg()">Save</button>
          <span id="elm-status" style="margin-left:10px;"></span>
        </div>
      </div>
'''
    # Insert before OTA card or at end
    ota = '<div class="card">\n        <div class="card-header"><h2>OTA'
    if ota in t:
        t2 = t.replace(ota, card + '\n' + ota)
        print('added')
    else:
        # insert before closing body div
        end = t.find('</body>')
        if end >= 0:
            t2 = t[:end] + card + '\n</body>' + t[end+7:]
            print('added2')
        else:
            print('no')
    
    # Add JS
    js = '''
function loadElm327Cfg(){
  fetch('/elm327_config').then(r=>r.json()).then(c=>{
    document.getElementById('elm-enabled').checked = !!c.enabled;
    document.getElementById('elm-wifi').checked = !!c.wifi_enabled;
    document.getElementById('elm-bt').checked = !!c.bt_enabled;
    var s=document.getElementById('elm-status'); if(s){ s.textContent='Loaded'; setTimeout(()=>{s.textContent='';},1000); }
  });
}
function saveElm327Cfg(){
  var data={enabled:document.getElementById('elm-enabled').checked, wifi_enabled:document.getElementById('elm-wifi').checked, bt_enabled:document.getElementById('elm-bt').checked};
  fetch('/elm327_config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)}).then(r=>r.json()).then(()=>{
    var s=document.getElementById('elm-status'); if(s){ s.textContent='Saved'; setTimeout(()=>{s.textContent='';},1500); }
  });
}
window.addEventListener('load', function(){ try{ loadElm327Cfg(); }catch(e){} });
'''
    if 'loadElm327Cfg' not in t2:
        if '</script>' in t2:
            t2 = t2.replace('</script>', js + '\n</script>')
            print('js')
    open(p,'w',encoding='utf-8').write(t2)
else:
    print('exists')
