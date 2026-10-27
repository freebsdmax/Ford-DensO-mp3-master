p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()

# Find the ELM card and move it inside INDEX_HTML properly
# Look for the card in the C string literal area
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

# Remove any standalone version outside strings
# Find the broken version
old_broken = '      <div class="card">\n        <div class="card-header"><h2>ELM327 Emulator</h2></div>\n        <div class="card-body">\n          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-enabled"> Enable ELM327</label><br>\n          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-wifi"> Wi-Fi TCP (port 35000)</label><br>\n          <label style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="elm-bt"> Bluetooth Classic SPP</label><br><br>\n          <button class="btn" type="button" onclick="loadElm327Cfg()">Reload</button>\n          <button class="btn btn-primary" type="button" onclick="saveElm327Cfg()">Save</button>\n          <span id="elm-status" style="margin-left:10px;"></span>\n        </div>\n      </div>\n'
if old_broken in t:
    t = t.replace(old_broken, '')
    print('removed')
    
# Also remove the version at line 771
lines = t.splitlines()
# Find lines that are not in string literals - look for standalone HTML
new_lines = []
for i,l in enumerate(lines):
    # Skip if it's a standalone HTML line not in a string (not starting with quote and continuing string)
    # But easier: look for our card pattern not after "
    stripped = l.strip()
    if '<div class="card">' in stripped and '<h2>ELM327 Emulator</h2>' in stripped:
        # Check if this is in a string literal context
        if not stripped.startswith('"') and '"' not in l.split('<')[0]:
            continue  # skip
        # Also skip if it's not properly quoted
    new_lines.append(l)

t2 = '\n'.join(new_lines)
open(p,'w',encoding='utf-8').write(t2)
print('done')
