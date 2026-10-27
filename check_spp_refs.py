t=open('components/elm327_emu/elm327_emu.c','r',encoding='utf-8',errors='ignore').read()
# find lines with esp_spp
for i,l in enumerate(t.splitlines()):
    if 'esp_spp' in l and '//' not in l.split('esp_spp')[0]:
        print(i+1,l.strip())
        break
else:
    print('none')
