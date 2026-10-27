p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()
# Fix the broken line
old='\n</body></html>";'
new='\n"</body></html>";'
if old in t:
    t2=t.replace(old,new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok1')
elif '</body></html>";' in t and not t.count('"') > 0:  # rough
    # try to add quote
    pass
# Try to find and fix
old2='</body></html>";'
if old2 in t and not old2.startswith('"'):
    t2=t.replace(old2,'"'+old2)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok2')
else:
    print('no')
