t=open('components/elm327_emu/elm327_emu.c','r',encoding='utf-8',errors='ignore').read()
for l in t.splitlines()[:50]:
    if 'include' in l:
        print(l)
