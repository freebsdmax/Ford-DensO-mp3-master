import os
c=0
for r,d,f in os.walk('build'):
    for fn in f:
        if fn.endswith('.log') or fn.endswith('.txt') or fn.endswith('.out'):
            p=os.path.join(r,fn)
            try:
                t=open(p,encoding='utf-8',errors='ignore').read()
                if 'undefined reference' in t and 'esp_spp' in t:
                    c+=1
                    print(p)
                    break
            except:
                pass
print('count',c)
