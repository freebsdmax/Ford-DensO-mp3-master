p = 'components/elm327_emu/elm327_emu.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# remove bt disconnect
old = '''    if (s_spp_connected && s_spp_handle != 0) {
        esp_spp_disconnect(s_spp_handle);
        s_spp_connected = false;
        s_spp_handle = 0;
    }'''
new = '''    if (s_spp_connected && s_spp_handle != 0) {
        s_spp_connected = false;
        s_spp_handle = 0;
    }'''
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
