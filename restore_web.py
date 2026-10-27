p = 'main/web_ota.c'
# restore from backup if exists
import os
bak = p + '.bak'
if os.path.exists(bak):
    open(p,'w',encoding='utf-8').write(open(bak,'r',encoding='utf-8',errors='ignore').read())
    print('restored')
else:
    print('nobak')
