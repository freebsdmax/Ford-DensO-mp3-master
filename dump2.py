t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
lines=t.splitlines()
for i,l in enumerate(lines[760:795],760+1):
    print(str(i)+':'+l)
