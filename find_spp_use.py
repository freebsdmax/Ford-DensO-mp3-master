t=open('components/elm327_emu/elm327_emu.c','r',encoding='utf-8',errors='ignore').read()
for i,l in enumerate(t.splitlines()):
    if 'esp_spp' in l:
        print(i+1,l.strip())
