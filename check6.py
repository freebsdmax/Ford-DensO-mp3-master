t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i = t.find('loadElm327')
if i>=0:
    print(t[i-5:i+80])
else:
    print('no')
