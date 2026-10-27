t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i = t.find('ELM327')
if i>=0:
    print(t[i-10:i+60])
else:
    print('no')
