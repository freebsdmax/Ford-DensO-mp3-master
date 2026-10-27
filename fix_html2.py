p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()
# Fix broken line
old='"</div></div>\n      <div class="card">'
new='      <div class="card">'
# also try
if old in t:
    t2=t.replace(old,new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok1')
    exit()
old2='"</div></div>\n      <div class="card">'
# different escaping
old3='"</div></div>\n      <div class="card">\n'
new3='      <div class="card">\n'
if old3 in t:
    t2=t.replace(old3,new3)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok2')
    exit()
print('no')
