t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i = t.find('ELM327')
if i>=0:
    print(repr(t[i-60:i+200]))
else:
    print('no')
