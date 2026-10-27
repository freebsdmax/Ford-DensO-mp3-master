t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
# look around loadElm327Cfg
i = t.find('loadElm327Cfg')
if i>=0:
    print(t[i-200:i+100])
else:
    print('no')
