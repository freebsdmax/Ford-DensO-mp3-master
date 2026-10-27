p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# remove everything from bad injection
idx = t.find('ELM327 Emulator')
if idx >= 0:
    # find start of that div line
    # go back to quote
    # simpler: cut from "<div class=\"card\">..." added until before "</body>"
    # find marker
    marker = '      <div class="card">\n        <div class="card-header"><h2>ELM327 Emulator</h2></div>'
    if marker in t:
        s = t.find(marker)
        # find </div> for card
        # look for next card start or </body>
        # easier cut to </body>
        body = t.find('</body>')
        if body >= 0:
            t2 = t[:s] + '\n</body>' + t[body+7:]
            open(p,'w',encoding='utf-8').write(t2)
            print('fixed')
            exit()
# try another pattern
marker2 = 'ELM327 Emulator'
s = t.find(marker2)
if s >= 0:
    # go back to find <div
    div = t.rfind('<div', 0, s)
    if div >= 0:
        body = t.find('</body>', div)
        if body >= 0:
            t2 = t[:div] + '\n</body>' + t[body+7:]
            open(p,'w',encoding='utf-8').write(t2)
            print('fixed2')
            exit()
print('no')
