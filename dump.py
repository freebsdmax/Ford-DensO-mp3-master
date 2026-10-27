t=open('main/web_ota.c','r',encoding='utf-8',errors='ignore').read()
lines=t.splitlines()
for i,l in enumerate(lines):
    if i>=765 and i<=785:
        print(str(i+1)+':'+l)
