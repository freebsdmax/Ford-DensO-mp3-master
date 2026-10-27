p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()
old='</div></div>      <div class="card">'
new='</div></div>\n      <div class="card">'
if old in t:
    t2=t.replace(old,new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
