t = open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
i = t.find('loadComponentLogs')
print(i)
if i>=0:
    print(t[i-100:i+200])
