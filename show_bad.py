t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i=t.find('</div></div>')
if i>=0:
    print(repr(t[i:i+80]))
else:
    print('no')
