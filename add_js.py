p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
js = '''function loadElm327(){
  fetch('/elm327_config').then(function(r){return r.json();}).then(function(c){
    var e=document.getElementById('elm-en'); if(e) e.checked=!!c.enabled;
    var w=document.getElementById('elm-wifi'); if(w) w.checked=!!c.wifi_enabled;
    var b=document.getElementById('elm-bt'); if(b) b.checked=!!c.bt_enabled;
    var s=document.getElementById('elm327-status'); if(s) s.innerText='Loaded';
  });
}
function saveElm327(){
  var body=JSON.stringify({enabled:document.getElementById('elm-en')&&document.getElementById('elm-en').checked, wifi_enabled:document.getElementById('elm-wifi')&&document.getElementById('elm-wifi').checked, bt_enabled:document.getElementById('elm-bt')&&document.getElementById('elm-bt').checked});
  fetch('/elm327_config',{method:'POST',headers:{'Content-Type':'application/json'},body:body}).then(function(r){return r.json();}).then(function(){ var s=document.getElementById('elm327-status'); if(s) s.innerText='Saved'; });
}
window.addEventListener('load',function(){ try{ loadElm327(); }catch(e){} });
'''
if 'loadElm327' not in t:
    # append before last </script>
    idx = t.rfind('</script>')
    if idx >= 0:
        t2 = t[:idx] + js + '\n' + t[idx:]
        open(p,'w',encoding='utf-8').write(t2)
        print('ok')
    else:
        print('no')
else:
    print('has')
