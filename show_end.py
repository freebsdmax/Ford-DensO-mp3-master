t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i = t.find('</body>')
if i>=0:
    print(repr(t[i-60:i+40]))
else:
    print('no')
