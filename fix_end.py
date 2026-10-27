p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
old = '</div>\n</div>\n</body></html>";'
new = '</div></div></body></html>";'
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
